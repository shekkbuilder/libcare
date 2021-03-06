#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <regex.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/vfs.h>
#include <sys/stat.h>

#include <gelf.h>
#include <libunwind.h>
#include <libunwind-ptrace.h>

#include "kpatch_user.h"
#include "kpatch_process.h"
#include "kpatch_file.h"
#include "kpatch_common.h"
#include "kpatch_elf.h"
#include "kpatch_ptrace.h"
#include "list.h"
#include "kpatch_log.h"


/*****************************************************************************
 * Utilities.
 ****************************************************************************/

/* Return -1 to indicate error, -2 to stop immediately */
typedef int (callback_t)(int pid, void *data);

static int
processes_do(int pid, callback_t callback, void *data);

/*****************************************************************************
 * Patch storage subroutines.
 ****************************************************************************/

static int
patch_file_verify(struct kp_file *kpfile)
{
	GElf_Ehdr *hdr;
	struct kpatch_file *k = kpfile->patch;
	ssize_t size = kpfile->size;

	kpdebug("Verifying patch for '%s'...", k->modulename);

	if (memcmp(k->magic, KPATCH_FILE_MAGIC1, sizeof(k->magic))) {
		kperr("'%s' patch is invalid: Invalid magic.\n",
		      k->modulename);
		return -1;
	}
	if (k->total_size > size) {
		kperr("'%s' patch is invalid: Invalid size: %u/%ld.\n",
		      k->modulename, k->total_size, size);
		return -1;
	}
	hdr = (void *)k + k->kpatch_offset;
	if (memcmp(hdr->e_ident, ELFMAG, SELFMAG) ||
			hdr->e_type != ET_REL ||
			hdr->e_shentsize != sizeof(GElf_Shdr)) {
		kperr("'%s' patch is invalid: Wrong ELF header or not ET_REL\n",
		      k->modulename);
		return -1;
	}
	kpdebug("OK\n");
	return 1;
}

static int
storage_init(kpatch_storage_t *storage,
	     const char *fname)
{
	int patch_fd = -1;
	struct stat stat = { .st_mode = 0 };

	if (fname != NULL) {
		patch_fd = open(fname, O_RDONLY);
		if (patch_fd < 0)
			goto out_err;

		if (fstat(patch_fd, &stat) < 0)
			goto out_close;
	}

	storage->patch_fd = patch_fd;
	storage->is_patch_dir = S_ISDIR(stat.st_mode);
	storage->path = NULL;

	if (storage->is_patch_dir) {
		rb_init(&storage->tree);
	} else {
		int ret;

		ret = kpatch_open_fd(storage->patch_fd, &storage->kpfile);
		if (ret < 0)
			goto out_close;

		ret = patch_file_verify(&storage->kpfile);
		if (ret < 0) {
			kpatch_close_file(&storage->kpfile);
			goto out_close;
		}
	}

	storage->path = strdup(fname);

	return 0;

out_close:
	close(patch_fd);
out_err:
	kplogerror("cannot open storage '%s'\n", fname);
	return -1;
}

static void
free_storage_patch_cb(struct rb_node *node)
{
	struct kpatch_storage_patch *patch;

	patch = rb_entry(node, struct kpatch_storage_patch, node);
	kpatch_close_file(&patch->kpfile);

	free(patch);
}

static void
storage_free(kpatch_storage_t *storage)
{
	close(storage->patch_fd);
	if (storage->is_patch_dir)
		rb_destroy(&storage->tree, free_storage_patch_cb);
	free(storage->path);
}

static int
cmp_buildid(struct rb_node *node, unsigned long key)
{
	const char *bid = (const char *)key;
	struct kpatch_storage_patch *patch;

	patch = rb_entry(node, struct kpatch_storage_patch, node);

	return strcmp(patch->buildid, bid);
}

#define PATCHLEVEL_TEMPLATE_NUM	0

static char *pathtemplates[] = {
	"%s/latest/kpatch.bin",
	"%s.kpatch"
};

static int
readlink_patchlevel(int dirfd, const char *fname)
{
	ssize_t r;
	char buf[32];

	*strrchr(fname, '/') = '\0';
	r = readlinkat(dirfd, fname, buf, sizeof(buf));
	if (r > 0 && r < 32) {
		buf[r] = '\0';
		return atoi(buf);
	} else if (r >= 32) {
		r = -1;
		errno = ERANGE;
	}

	kplogerror("can't readlink '%s' to find patchlevel\n",
		   fname);
	return -1;
}

enum {
	PATCH_OPEN_ERROR = -1,
	PATCH_NOT_FOUND = 0,
	PATCH_FOUND = 1,
};

static inline int
storage_open_patch(kpatch_storage_t *storage,
		   const char *buildid,
		   struct kpatch_storage_patch* patch)
{
	char fname[96];
	int i, rv;

	for (i = 0; i < ARRAY_SIZE(pathtemplates); i++) {
		sprintf(fname, pathtemplates[i], buildid);

		rv = kpatch_openat_file(storage->patch_fd, fname, &patch->kpfile);
		if (rv == 0) {
			rv = patch_file_verify(&patch->kpfile);

			if (rv < 0)
				kpatch_close_file(&patch->kpfile);
			else
				rv = PATCH_FOUND;
			break;
		}
	}

	if (rv == PATCH_FOUND && i == PATCHLEVEL_TEMPLATE_NUM) {
		rv = readlink_patchlevel(storage->patch_fd, fname);
		if (rv < 0) {
			rv = PATCH_OPEN_ERROR;
			kpatch_close_file(&patch->kpfile);
		} else {
			patch->patchlevel = rv;
			patch->kpfile.patch->user_level = patch->patchlevel;
			rv = PATCH_FOUND;
		}

	}

	return rv;
}

static inline int
storage_stat_patch(kpatch_storage_t *storage,
		   const char *buildid,
		   struct kpatch_storage_patch* patch)
{
	char fname[96];
	struct stat buf;
	int i, rv;

	for (i = 0; i < ARRAY_SIZE(pathtemplates); i++) {
		sprintf(fname, pathtemplates[i], buildid);

		rv = fstatat(storage->patch_fd, fname, &buf, /* flags */ 0);

		if (rv == 0) {
			rv = PATCH_FOUND;
			patch->kpfile.size = buf.st_size;
			break;
		} else if (rv < 0 && errno == ENOENT) {
			rv = PATCH_NOT_FOUND;
		}
	}

	if (rv == PATCH_FOUND && i == PATCHLEVEL_TEMPLATE_NUM) {
		rv = readlink_patchlevel(storage->patch_fd, fname);
		rv = rv >= 0 ? PATCH_FOUND : PATCH_OPEN_ERROR;
	}

	return rv;
}

/*
 * TODO(pboldin) I duplicate a lot of code kernel has for filesystems already.
 * Should we avoid this caching at all?
 */
/* PATCH_FOUND -- found, PATCH_OPEN_ERROR -- error,
 * PATCH_NOT_FOUND -- not found */
static int
storage_find_patch(kpatch_storage_t *storage, const char *buildid,
		   struct kp_file **pkpfile)
{
	struct kpatch_storage_patch *patch = NULL;
	struct rb_node *node;
	int rv;

	if (!storage->is_patch_dir) {
		if (!strcmp(storage->kpfile.patch->uname, buildid)) {
			if (pkpfile != NULL)
				*pkpfile = &storage->kpfile;
			return PATCH_FOUND;
		}
		return PATCH_NOT_FOUND;
	}

	/* Look here, could be loaded already */
	node = rb_search_node(&storage->tree, cmp_buildid,
			      (unsigned long)buildid);
	if (node != NULL) {
		struct kpatch_storage_patch *patch;
		patch = rb_entry(node, struct kpatch_storage_patch, node);
		if (pkpfile != NULL)
			*pkpfile = &patch->kpfile;
		/* Just checking, no need to load the data */
		return patch->kpfile.size > 0 ? PATCH_FOUND : PATCH_NOT_FOUND;
	}

	/* OK, look at the filesystem */
	patch = malloc(sizeof(*patch));
	if (patch == NULL)
		return -1;

	memset(patch, 0, sizeof(*patch));

	if (pkpfile != NULL) {
		rv = storage_open_patch(storage, buildid, patch);
		if (rv == PATCH_FOUND)
			*pkpfile = &patch->kpfile;
	} else {
		rv = storage_stat_patch(storage, buildid, patch);
	}

	if (rv == PATCH_OPEN_ERROR) {
		free(patch);
		return rv;
	}

	strcpy(patch->buildid, buildid);

	rb_insert_node(&storage->tree,
		       &patch->node,
		       cmp_buildid,
		       (unsigned long)buildid);

	return rv;
}

static int
storage_lookup_patches(kpatch_storage_t *storage, kpatch_process_t *proc)
{
	struct kp_file *pkpfile;
	struct object_file *o;
	const char *bid;
	int found = 0, ret;

	list_for_each_entry(o, &proc->objs, list) {
		if (!o->is_elf || is_kernel_object_name(o->name))
			continue;

		bid = kpatch_get_buildid(o);

		ret = storage_find_patch(storage, bid, &pkpfile);
		if (ret == PATCH_OPEN_ERROR) {
			kplogerror("error finding patch for %s (%s)\n",
				   o->name, bid);
			continue;
		}

		if (ret == PATCH_FOUND) {
			o->skpfile = pkpfile;
			found++;
		}
	}

	kpinfo("%d object(s) have valid patch(es)\n", found);

	kpdebug("Object files dump:\n");
	list_for_each_entry(o, &proc->objs, list)
		kpatch_object_dump(o);

	return found;
}

enum {
	ACTION_APPLY_PATCH,
	ACTION_UNAPPLY_PATCH
};

static inline int
is_addr_in_info(unsigned long addr,
		struct kpatch_info *info,
		int direction)
{
#define IS_ADDR_IN_HALF_INTERVAL(addr, start, len) ((addr >= start) && (addr < start + len))
	if (direction == ACTION_APPLY_PATCH)
		return IS_ADDR_IN_HALF_INTERVAL(addr, info->daddr, info->dlen);
	if (direction == ACTION_UNAPPLY_PATCH)
		return IS_ADDR_IN_HALF_INTERVAL(addr, info->saddr, info->slen);
	return 0;
}

/**
 * Verify that the function from file `o' is safe to be patched.
 *
 * If retip is given then the safe address is returned in it.
 * What is considered a safe address depends on the `paranoid' value. When it
 * is true, safe address is the upper of ALL functions that do have a patch.
 * When it is false, safe address is the address of the first function
 * instruction that have no patch.
 *
 * That is, for the call chain from left to right with functions that have
 * patch marked with '+':
 *
 * foo -> bar+ -> baz -> qux+
 *
 * With `paranoid=true' this function will return address of the `bar+'
 * instruction being executed with *retip pointing to the `foo' instruction
 * that comes after the call to `bar+'. With `paranoid=false' this function
 * will return address of the `qux+' instruction being executed with *retip
 * pointing to the `baz' instruction that comes after call to `qux+'.
 */
static unsigned long
object_patch_verify_safety_single(struct object_file *o,
				  unw_cursor_t *cur,
				  unsigned long *retip,
				  int paranoid,
				  int direction)
{
	unw_word_t ip;
	struct kpatch_info *info = o->info;
	size_t i, ninfo = o->ninfo;
	int prev = 0;
	unsigned long last = 0;

	if (direction != ACTION_APPLY_PATCH &&
	    direction != ACTION_UNAPPLY_PATCH)
		kpfatal("unknown direction");

	do {
		unw_get_reg(cur, UNW_REG_IP, &ip);

		for (i = 0; i < ninfo; i++) {
			if (is_new_func(&info[i]))
				continue;

			if (is_addr_in_info((long)ip, &info[i], direction)) {
				if (direction == ACTION_APPLY_PATCH)
					last = info[i].daddr;
				else if (direction == ACTION_UNAPPLY_PATCH)
					last = info[i].saddr;
				prev = 1;
				break;
			}
		}

		if (prev && i == ninfo) {
			prev = 0;
			if (retip)
				*retip = ip;
			if (!paranoid)
				break;
		}
	} while (unw_step(cur) > 0);

	return last;
}

#define KPATCH_CORO_STACK_UNSAFE (1 << 20)

static int
patch_verify_safety(struct object_file *o,
		    unsigned long *retips,
		    int direction)
{
	size_t nr = 0, failed = 0, count = 0;
	struct kpatch_ptrace_ctx *p;
	struct kpatch_coro *c;
	unsigned long retip, ret;
	unw_cursor_t cur;

	list_for_each_entry(c, &o->proc->coro.coros, list) {
		void *ucoro;

		kpdebug("Verifying safety for coroutine %zd...", count);
		ucoro = _UCORO_create(c, proc2pctx(o->proc)->pid);
		if (!ucoro) {
			kplogerror("can't create unwind coro context\n");
			return -1;
		}

		ret = unw_init_remote(&cur, o->proc->coro.unwd, ucoro);
		if (ret) {
			kplogerror("can't create unwind remote context\n");
			_UCORO_destroy(ucoro);
			return -1;
		}

		ret = object_patch_verify_safety_single(o, &cur, NULL, 0, direction);
		_UCORO_destroy(ucoro);
		if (ret) {
			kperr("safety check failed to %lx\n", ret);
			failed++;
		} else {
			kpdebug("OK\n");
		}
		count++;
	}
	if (failed)
		return failed | KPATCH_CORO_STACK_UNSAFE;

	list_for_each_entry(p, &o->proc->ptrace.pctxs, list) {
		void *upt;

		kpdebug("Verifying safety for pid %d...", p->pid);
		upt = _UPT_create(p->pid);
		if (!upt) {
			kplogerror("can't create unwind ptrace context\n");
			return -1;
		}

		ret = unw_init_remote(&cur, o->proc->ptrace.unwd, upt);
		if (ret) {
			kplogerror("can't create unwind remote context\n");
			_UPT_destroy(upt);
			return -1;
		}

		ret = object_patch_verify_safety_single(o, &cur, &retip, 0, direction);
		_UPT_destroy(upt);
		if (ret) {
			/* TODO: dump full backtrace, with symbols where possible (shared libs) */
			if (retips) {
				kperr("safety check failed for %lx, will continue until %lx\n",
				      ret, retip);
				retips[nr] = retip;
			} else {
				kperr("safety check failed for %lx\n", ret);
				errno = -EBUSY;
			}
			failed++;
		}
		kpdebug("OK\n");
		nr++;
	}

	return failed;
}

/*
 * Ensure that it is safe to apply/unapply patch for the object file `o`.
 *
 * First, we verify the safety of the patch.
 *
 * It is safe to apply patch (ACTION_APPLY_PATCH) when no threads or coroutines
 * are executing the functions to be patched.
 *
 * It is safe to unapply patch (ACTION_UNAPPLY_PATCH) when no threads or
 * coroutines are executing the patched functions.
 *
 * If it is not safe to do the action we continue threads execution until they
 * are out of the functions that we want to patch/unpatch. This is done using
 * `kpatch_ptrace_execute_until` function with default timeout of 3000 seconds
 * and checking for action safety again.
 */
static int
patch_ensure_safety(struct object_file *o,
		    int action)
{
	struct kpatch_ptrace_ctx *p;
	unsigned long ret, *retips;
	size_t nr = 0, i;

	list_for_each_entry(p, &o->proc->ptrace.pctxs, list)
		nr++;
	retips = malloc(nr * sizeof(unsigned long));
	if (retips == NULL)
		return -1;

	memset(retips, 0, nr * sizeof(unsigned long));

	ret = patch_verify_safety(o, retips, action);
	/*
	 * For coroutines we can't "execute until"
	 */
	if (ret && !(ret & KPATCH_CORO_STACK_UNSAFE)) {
		i = 0;
		list_for_each_entry(p, &o->proc->ptrace.pctxs, list) {
			p->execute_until = retips[i];
			i++;
		}

		ret = kpatch_ptrace_execute_until(o->proc, 3000, 0);

		/* OK, at this point we may have new threads, discover them */
		if (ret == 0)
			ret = kpatch_process_attach(o->proc);
		if (ret == 0)
			ret = patch_verify_safety(o, NULL, action);
	}

	free(retips);

	return ret ? -1 : 0;
}

/*****************************************************************************
 * Patch application subroutines and cmd_patch_user
 ****************************************************************************/
/*
 * This flag is local, i.e. it is never stored to the
 * patch applied to patient's memory.
 */
#define PATCH_APPLIED	(1 << 31)

#define HUNK_SIZE 5

static int
patch_apply_hunk(struct object_file *o, size_t nhunk)
{
	int ret;
	char code[HUNK_SIZE] = {0xe9, 0x00, 0x00, 0x00, 0x00}; /* jmp IMM */
	struct kpatch_info *info = &o->info[nhunk];
	unsigned long pundo;

	if (is_new_func(info))
		return 0;

	pundo = o->kpta + o->kpfile.patch->user_undo + nhunk * HUNK_SIZE;
	kpinfo("%s origcode from 0x%lx+0x%x to 0x%lx\n",
	       o->name, info->daddr, HUNK_SIZE, pundo);
	ret = kpatch_process_memcpy(o->proc, pundo,
				    info->daddr, HUNK_SIZE);
	if (ret < 0)
		return ret;

	kpinfo("%s hunk 0x%lx+0x%x -> 0x%lx+0x%x\n",
	       o->name, info->daddr, info->dlen, info->saddr, info->slen);
	*(unsigned int *)(code + 1) = (unsigned int)(info->saddr - info->daddr - 5);
	ret = kpatch_process_mem_write(o->proc,
				       code,
				       info->daddr,
				       sizeof(code));
	/*
	 * NOTE(pboldin): This is only stored locally, as information have
	 * been copied to patient's memory already.
	 */
	info->flags |= PATCH_APPLIED;
	return ret ? -1 : 0;
}

static int
duplicate_kp_file(struct object_file *o)
{
	struct kpatch_file *patch;

	patch = malloc(o->skpfile->size);
	if (patch == NULL)
		return -1;

	memcpy(patch, o->skpfile->patch, o->skpfile->size);
	o->kpfile.patch = patch;
	o->kpfile.size = o->skpfile->size;

	return 0;
}

static int
object_apply_patch(struct object_file *o)
{
	struct kpatch_file *kp;
	size_t sz, i;
	int undef, ret;

	if (o->skpfile == NULL || o->is_patch)
		return 0;

	if (o->applied_patch) {
		kpinfo("Object '%s' already have a patch, not patching\n",
		       o->name);
		return 0;
	}

	ret = duplicate_kp_file(o);
	if (ret < 0) {
		kplogerror("can't duplicate kp_file\n");
		return -1;
	}

	ret = kpatch_elf_load_kpatch_info(o);
	if (ret < 0)
		return ret;

	kp = o->kpfile.patch;

	sz = ROUND_UP(kp->total_size, 8);
	undef = kpatch_count_undefined(o);
	if (undef) {
		o->jmp_table = kpatch_new_jmp_table(undef);
		kp->jmp_offset = sz;
		kpinfo("Jump table %d bytes for %d syms at offset 0x%x\n",
		       o->jmp_table->size, undef, kp->jmp_offset);
		sz = ROUND_UP(sz + o->jmp_table->size, 128);
	}

	kp->user_info = (unsigned long)o->info -
			(unsigned long)o->kpfile.patch;
	kp->user_undo = sz;
	sz = ROUND_UP(sz + HUNK_SIZE * o->ninfo, 16);

	sz = ROUND_UP(sz, 4096);

	/*
	 * Map patch as close to the original code as possible.
	 * Otherwise we can't use 32-bit jumps.
	 */
	ret = kpatch_object_allocate_patch(o, sz);
	if (ret < 0)
		return ret;
	ret = kpatch_resolve(o);
	if (ret < 0)
		return ret;
	ret = kpatch_relocate(o);
	if (ret < 0)
		return ret;
	ret = kpatch_process_mem_write(o->proc,
				       kp,
				       o->kpta,
				       kp->total_size);
	if (ret < 0)
		return -1;
	if (o->jmp_table) {
		ret = kpatch_process_mem_write(o->proc,
					       o->jmp_table,
					       o->kpta + kp->jmp_offset,
					       o->jmp_table->size);
		if (ret < 0)
			return ret;
	}

	ret = patch_ensure_safety(o, ACTION_APPLY_PATCH);
	if (ret < 0)
		return ret;

	for (i = 0; i < o->ninfo; i++) {
		ret = patch_apply_hunk(o, i);
		if (ret < 0)
			return ret;
	}

	return 1;
}

static int
object_unapply_patch(struct object_file *o, int check_flag);

static int
object_unapply_old_patch(struct object_file *o)
{
	struct kpatch_file *kpatch_applied, *kpatch_storage;
	int ret;

	if (o->skpfile == NULL || o->is_patch || o->applied_patch == NULL)
		return 0;

	kpatch_applied = o->applied_patch->kpfile.patch;
	kpatch_storage = o->skpfile->patch;

	if (kpatch_applied->user_level >= kpatch_storage->user_level) {
		kpinfo("'%s' applied patch level is %d (storage has %d\n)\n",
		       o->name,
		       kpatch_applied->user_level,
		       kpatch_storage->user_level);
		return 1;
	}

	printf("%s: replacing patch level %d with level %d\n",
	       o->name,
	       kpatch_applied->user_level,
	       kpatch_storage->user_level);
	ret = object_unapply_patch(o, /* check_flag */ 0);
	if (ret < 0)
		kperr("can't unapply patch for %s\n", o->name);
	else {
		/* TODO(pboldin): handle joining the holes here */
		o->applied_patch = NULL;
		o->info = NULL;
		o->ninfo = 0;
	}

	return ret;
}

static int
kpatch_apply_patches(kpatch_process_t *proc)
{
	struct object_file *o;
	int applied = 0, ret;

	list_for_each_entry(o, &proc->objs, list) {

		ret = object_unapply_old_patch(o);
		if (ret < 0)
			break;

		ret = object_apply_patch(o);
		if (ret < 0)
			goto unpatch;
		if (ret)
			applied++;
	}
	return applied;

unpatch:
	kperr("Patching %s failed, unapplying partially applied patch\n", o->name);
	/*
	 * TODO(pboldin): close the holes so the state is the same
	 * after unpatch
	 */
	ret = object_unapply_patch(o, /* check_flag */ 1);
	if (ret < 0) {
		kperr("Can't unapply patch for %s\n", o->name);
	}
	return -1;
}

struct patch_data {
	kpatch_storage_t *storage;
	int is_just_started;
	int send_fd;
};

static int process_patch(int pid, void *_data)
{
	int ret;
	kpatch_process_t _proc, *proc = &_proc;
	struct patch_data *data = _data;

	kpatch_storage_t *storage = data->storage;
	int is_just_started = data->is_just_started;
	int send_fd = data->send_fd;

	ret = kpatch_process_init(proc, pid, is_just_started, send_fd);
	if (ret < 0) {
		kperr("cannot init process %d\n", pid);
		goto out;
	}

	kpatch_process_print_short(proc);

	ret = kpatch_process_attach(proc);
	if (ret < 0)
		goto out_free;

	/*
	 * In case the process was just started we continue execution up to the
	 * entry point of a program just to allow ld.so to load up libraries
	 */
	ret = kpatch_process_load_libraries(proc);
	if (ret < 0)
		goto out_free;

	/*
	 * For each object file that we want to patch (either binary
	 * itself or shared library) we need its ELF structure
	 * to perform relocations. Because we know uniq BuildID of
	 * the object the section addresses stored in the patch
	 * are valid for the original object.
	 */
	ret = kpatch_process_map_object_files(proc);
	if (ret < 0)
		goto out_free;

	/*
	 * Lookup for patches appicable for proc in storage.
	 */
	ret = storage_lookup_patches(storage, proc);
	if (ret <= 0)
		goto out_free;

	ret = kpatch_find_coroutines(proc);
	if (ret < 0)
		goto out_free;

	ret = kpatch_apply_patches(proc);

out_free:
	kpatch_process_free(proc);

out:
	if (ret < 0) {
		printf("Failed to apply patch '%s'\n", storage->path);
		kperr("Failed to apply patch '%s'\n", storage->path);
	} else if (ret == 0)
		printf("No patch(es) applicable to PID '%d' have been found\n", pid);
	else {
		printf("%d patch hunk(s) have been successfully applied to PID '%d'\n", ret, pid);
		ret = 0;
	}

	return ret;
}

static int
processes_patch(kpatch_storage_t *storage,
		int pid, int is_just_started, int send_fd)
{
	struct patch_data data = {
		.storage = storage,
		.is_just_started = is_just_started,
		.send_fd = send_fd,
	};

	return processes_do(pid, process_patch, &data);
}

/* Check if system is suitable */
static int kpatch_check_system(void)
{
	return 1;
}

static int usage_patch(const char *err)
{
	if (err)
		fprintf(stderr, "err: %s\n", err);
	fprintf(stderr, "usage: libcare-doctor patch [options] <-p PID> <-r fd> <patch>\n");
	fprintf(stderr, "\nOptions:\n");
	fprintf(stderr, "  -h          - this message\n");
	fprintf(stderr, "  -s          - process was just executed\n");
	fprintf(stderr, "  -p <PID>    - target process\n");
	fprintf(stderr, "  -r fd       - fd used with LD_PRELOAD=execve.so.\n");
	return -1;
}

int cmd_patch_user(int argc, char *argv[])
{
	kpatch_storage_t storage;
	int opt, pid = -1, is_pid_set = 0, ret, start = 0, send_fd = -1;

	if (argc < 4)
		return usage_patch(NULL);

	while ((opt = getopt(argc, argv, "hsp:r:")) != EOF) {
		switch (opt) {
		case 'h':
			return usage_patch(NULL);
		case 'p':
			if (strcmp(optarg, "all"))
				pid = atoi(optarg);
			is_pid_set = 1;
			break;
		case 'r':
			send_fd = atoi(optarg);
			break;
		case 's':
			start = 1;
			break;
		default:
			return usage_patch("unknown option");
		}
	}

	argc -= optind;
	argv += optind;

	if (!is_pid_set)
		return usage_patch("PID argument is mandatory");

	if (!kpatch_check_system())
		goto out_err;

	ret = storage_init(&storage, argv[argc - 1]);
	if (ret < 0)
		goto out_err;


	ret = processes_patch(&storage, pid, start, send_fd);

	storage_free(&storage);

out_err:
	return ret;
}

/*****************************************************************************
 * Patch cancellcation subroutines and cmd_unpatch_user
 ****************************************************************************/
static int
object_find_applied_patch_info(struct object_file *o)
{
	struct kpatch_info tmpinfo;
	struct kpatch_info *remote_info;
	size_t nalloc = 0;
	struct process_mem_iter *iter;
	int ret;

	if (o->info != NULL)
		return 0;

	iter = kpatch_process_mem_iter_init(o->proc);
	if (iter == NULL)
		return -1;

	remote_info = (void *)o->kpta + o->kpfile.patch->user_info;
	do {
		ret = REMOTE_PEEK(iter, tmpinfo, remote_info);
		if (ret < 0)
			goto err;

		if (is_end_info(&tmpinfo))
		    break;

		if (o->ninfo == nalloc) {
			nalloc += 16;
			o->info = realloc(o->info, nalloc * sizeof(tmpinfo));
		}

		o->info[o->ninfo] = tmpinfo;

		remote_info++;
		o->ninfo++;
	} while (1);

	o->applied_patch->info = o->info;
	o->applied_patch->ninfo = o->ninfo;

err:
	kpatch_process_mem_iter_free(iter);

	return ret;
}

static int
object_unapply_patch(struct object_file *o, int check_flag)
{
	int ret;
	size_t i;
	unsigned long orig_code_addr;

	ret = object_find_applied_patch_info(o);
	if (ret < 0)
		return ret;

	ret = patch_ensure_safety(o, ACTION_UNAPPLY_PATCH);
	if (ret < 0)
		return ret;

	orig_code_addr = o->kpta + o->kpfile.patch->user_undo;

	for (i = 0; i < o->ninfo; i++) {
		if (is_new_func(&o->info[i]))
			continue;

		if (check_flag && !(o->info[i].flags & PATCH_APPLIED))
			continue;

		ret = kpatch_process_memcpy(o->proc,
					    o->info[i].daddr,
					    orig_code_addr,
					    HUNK_SIZE);
		/* XXX(pboldin) We are in deep trouble here, handle it
		 * by restoring the patch back */
		if (ret < 0)
			return ret;

		orig_code_addr += HUNK_SIZE;
	}

	ret = kpatch_munmap_remote(proc2pctx(o->proc),
				   o->kpta,
				   o->kpfile.size);

	return ret;
}

static int
kpatch_should_unapply_patch(struct object_file *o,
			    char *buildids[],
			    int nbuildids)
{
	int i;
	const char *bid;

	if (nbuildids == 0)
		return 1;

	bid = kpatch_get_buildid(o);

	for (i = 0; i < nbuildids; i++) {
		if (!strcmp(bid, buildids[i]) ||
		    !strcmp(o->name, buildids[i]))
			return 1;
	}

	return 0;
}

static int
kpatch_unapply_patches(kpatch_process_t *proc,
		       char *buildids[],
		       int nbuildids)
{
	struct object_file *o;
	int ret;
	size_t unapplied = 0;

	ret = kpatch_process_associate_patches(proc);
	if (ret < 0)
		return ret;

	list_for_each_entry(o, &proc->objs, list) {
		if (o->applied_patch == NULL)
			continue;

		if (!kpatch_should_unapply_patch(o, buildids, nbuildids))
			continue;

		ret = object_unapply_patch(o, /* check_flag */ 0);
		if (ret < 0)
			return ret;
		unapplied++;
	}

	return unapplied;
}

struct unpatch_data {
	char **buildids;
	int nbuildids;
};

static int
process_unpatch(int pid, void *_data)
{
	int ret;
	kpatch_process_t _proc, *proc = &_proc;
	struct unpatch_data *data = _data;
	char **buildids = data->buildids;
	int nbuildids = data->nbuildids;

	ret = kpatch_process_init(proc, pid, /* start */ 0, /* send_fd */ -1);
	if (ret < 0)
		return -1;

	kpatch_process_print_short(proc);

	ret = kpatch_process_attach(proc);
	if (ret < 0)
		goto out;

	ret = kpatch_process_map_object_files(proc);
	if (ret < 0)
		goto out;

	ret = kpatch_find_coroutines(proc);
	if (ret < 0)
		goto out;

	ret = kpatch_unapply_patches(proc, buildids, nbuildids);

out:
	kpatch_process_free(proc);

	if (ret < 0)
		printf("Failed to cancel patches for %d\n", pid);
	else if (ret == 0)
		printf("No patch(es) cancellable from PID '%d' were found\n", pid);
	else
		printf("%d patch hunk(s) were successfully cancelled from PID '%d'\n", ret, pid);

	return ret;
}

static int
processes_unpatch(int pid, char *buildids[], int nbuildids)
{
	struct unpatch_data data = {
		.buildids = buildids,
		.nbuildids = nbuildids
	};

	return processes_do(pid, process_unpatch, &data);
}

static int usage_unpatch(const char *err)
{
	if (err)
		fprintf(stderr, "err: %s\n", err);
	fprintf(stderr, "usage: libcare-doctor unpatch [options] <-p PID> "
		"[Build-ID or name ...]\n");
	fprintf(stderr, "\nOptions:\n");
	fprintf(stderr, "  -h          - this message\n");
	fprintf(stderr, "  -p <PID>    - target process\n");
	return -1;
}

int cmd_unpatch_user(int argc, char *argv[])
{
	int opt, pid = -1, is_pid_set = 0;

	if (argc < 3)
		return usage_unpatch(NULL);

	while ((opt = getopt(argc, argv, "hp:")) != EOF) {
		switch (opt) {
		case 'h':
			return usage_unpatch(NULL);
		case 'p':
			if (strcmp(optarg, "all"))
				pid = atoi(optarg);
			is_pid_set = 1;
			break;
		default:
			return usage_unpatch("unknown option");
		}
	}

	argc -= optind;
	argv += optind;

	if (!is_pid_set)
		return usage_patch("PID argument is mandatory");

	if (!kpatch_check_system())
		return -1;

	return processes_unpatch(pid, argv, argc);
}

static
int usage_info(const char *err)
{
	if (err)
		fprintf(stderr, "err: %s\n", err);
	fprintf(stderr, "usage: libcare-doctor info [options] [-b BUILDID] [-p PID] [-s STORAGE] [-r REGEXP]\n");
	fprintf(stderr, "\nOptions:\n");
	fprintf(stderr, "  -h		- this message\n");
	fprintf(stderr, "  -b <BUILDID>	- output all processes having object with specified BuildID loaded\n");
	fprintf(stderr, "  -p <PID>	- target process, 'all' or omitted for all the system processes\n");
	fprintf(stderr, "  -s <STORAGE>	- only show BuildIDs of object having patches in STORAGE\n");
	fprintf(stderr, "  -r <REGEXP>	- only show BuildIDs of object having name matching REGEXP\n");
	return -1;
}

struct info_data {
	const char *buildid;
	kpatch_storage_t *storage;
	regex_t *name_re;
};

static int
process_info(int pid, void *_data)
{
	int ret, pid_printed = 0;
	kpatch_process_t _proc, *proc = &_proc;
	struct info_data *data = _data;
	struct object_file *o;

	ret = kpatch_process_init(proc, pid, /* start */ 0, /* send_fd */ -1);
	if (ret < 0)
		return -1;

	ret = kpatch_process_attach(proc);
	if (ret < 0)
		goto out;

	ret = kpatch_process_parse_proc_maps(proc);
	if (ret < 0)
		goto out;


	list_for_each_entry(o, &proc->objs, list) {
		const char *buildid;

		if (!o->is_elf || is_kernel_object_name(o->name))
			continue;


		if (data->name_re != NULL &&
		    regexec(data->name_re, o->name,
			    0, NULL, REG_EXTENDED) == REG_NOMATCH)
			continue;

		buildid = kpatch_get_buildid(o);

		if (data->buildid) {
			if (!strcmp(data->buildid, buildid)) {
				printf("pid=%d comm=%s\n", pid, proc->comm);
				printf("%s %s\n", o->name, buildid);
				break;
			}
			continue;
		}

		if (data->storage &&
		    !storage_find_patch(data->storage, buildid, NULL))
			continue;

		if (!pid_printed) {
			printf("pid=%d comm=%s\n", pid, proc->comm);
			pid_printed = 1;
		}
		printf("%s %s\n", o->name, buildid);
	}

out:
	kpatch_process_free(proc);

	return ret;
}

static int
processes_info(int pid,
	       const char *buildid,
	       const char *storagepath,
	       const char *regexp)
{
	int ret = -1;
	struct info_data data = { 0, };
	kpatch_storage_t storage;
	regex_t regex;

	data.buildid = buildid;
	if (regexp != NULL) {
		ret = regcomp(&regex, regexp, REG_EXTENDED);
		if (ret != 0) {
			ret = -1;
			goto out_err;
		}
		data.name_re = &regex;
	}

	if (storagepath != NULL) {
		ret = storage_init(&storage, storagepath);
		if (ret < 0)
			goto out_err;
		data.storage = &storage;
	}

	ret = processes_do(pid, process_info, &data);

out_err:
	if (data.storage != NULL) {
		storage_free(data.storage);
	}
	if (data.name_re != NULL) {
		regfree(data.name_re);
	}

	return ret;
}

int cmd_info_user(int argc, char *argv[])
{
	int opt, pid = -1, verbose = 0;
	const char *buildid = NULL, *storagepath = NULL, *regexp = NULL;

	while ((opt = getopt(argc, argv, "hb:p:s:r:v")) != EOF) {
		switch (opt) {
		case 'b':
			buildid = optarg;
			break;
		case 'p':
			if (strcmp(optarg, "all"))
				pid = atoi(optarg);
			break;
		case 's':
			storagepath = optarg;
			break;
		case 'r':
			regexp = optarg;
			break;
		case 'v':
			verbose = 1;
			break;
		}
	}

	if (!verbose)
		log_level = LOG_ERR;

	if ((regexp != NULL && buildid != NULL)  ||
	    (buildid != NULL && storagepath != NULL)) {
		return usage_info("regexp & buildid | buildid & storage are mutual");
	}


	return processes_info(pid, buildid, storagepath, regexp);
}

/*****************************************************************************
 * Utilities.
 ****************************************************************************/
static int
processes_do(int pid, callback_t callback, void *data)
{
	DIR *dir;
	struct dirent *de;
	int ret = 0, rv;
	char *tmp;

	if (pid != -1)
		return callback(pid, data);

	dir = opendir("/proc");
	if (!dir) {
		kplogerror("can't open '/proc' directory\n");
		return -1;
	}

	while ((de = readdir(dir))) {
		if (de->d_name[0] == '.')
			continue;

		pid = strtoul(de->d_name, &tmp, 10);
		if (pid == 0 || *tmp != '\0')
			continue;

		if (pid == 1 || pid == getpid())
			continue;

		rv = callback(pid, data);
		if (rv < 0)
			ret = -1;
		if (rv == -2)
			break;
	}

	closedir(dir);

	return ret;
}

static int usage(const char *err)
{
	if (err)
		fprintf(stderr, "err: %s\n", err);
	fprintf(stderr, "usage: libcare-doctor [options] <cmd> [args]\n");
	fprintf(stderr, "\nOptions:\n");
	fprintf(stderr, "  -v          - verbose mode\n");
	fprintf(stderr, "  -h          - this message\n");
	fprintf(stderr, "\nCommands:\n");
	fprintf(stderr, "  patch  - apply patch to a user-space process\n");
	fprintf(stderr, "  unpatch- unapply patch from a user-space process\n");
	fprintf(stderr, "  info   - show info on applied patches\n");
	return -1;
}

/* entry point */
int main(int argc, char *argv[])
{
	int opt;
	char *cmd;

	while ((opt = getopt(argc, argv, "+vh")) != EOF) {
		switch (opt) {
			case 'v':
				log_level += 1;
				break;
			case 'h':
				return usage(NULL);
			default:
				return usage("unknown option");
		}
	}

	cmd = argv[optind];
	argc -= optind;
	argv += optind;

	if (argc < 1)
		return usage("not enough arguments.");

	optind = 1;
	if (!strcmp(cmd, "patch") || !strcmp(cmd, "patch-user"))
		return cmd_patch_user(argc, argv);
	else if (!strcmp(cmd, "unpatch") || !strcmp(cmd, "unpatch-user"))
		return cmd_unpatch_user(argc, argv);
	else if (!strcmp(cmd, "info") || !strcmp(cmd, "info-user"))
		return cmd_info_user(argc, argv);
	else
		return usage("unknown command");
}
