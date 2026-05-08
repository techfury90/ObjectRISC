/*
 * vfs.c — Phase 45g: path-aware front door over hostfsd.
 *
 * Programs that want to interact with the filesystem should prefer
 * vfs_*() over the bare hf_*() helpers. Each call dir_walks the
 * user-visible path into a (kind, remainder) pair, then dispatches:
 *
 *   - For MOUNT-resolved paths, the remainder is handed to the
 *     existing hf_* layer (which still SENDs to the program's boot
 *     O10 hostfsd). v1 explicitly does NOT use the resolved service
 *     ref as the SEND recipient — every mount today routes back to
 *     the same hostfsd anyway, and pretending otherwise would cost
 *     us a SEND-recipient indirection without payoff. Multi-backend
 *     dispatch is straightforward to add later by overriding O10
 *     from DIR_RESULT_SLOT around each hf_* call.
 *
 *   - For DIR-resolved paths (e.g. `/sys/cpu`), vfs_open / vfs_opendir
 *     fail (kind != MOUNT means there's no underlying file backend).
 *     vfs_list dispatches to dir_list to enumerate the in-memory
 *     children of the directory tree.
 *
 *   - For LEAF-resolved paths (services registered via dir_register,
 *     e.g. `/sys/cpu/0/supervisor`), all vfs_* operations fail —
 *     leaves are for service-discovery, not file I/O.
 *
 * Pre-conditions enforced by the caller (NOT by vfs):
 *   - `task_init()` has run (we use OR-store slots via O12)
 *   - `hf_init()` has run (vfs delegates to hf_*)
 *   - DIR_SLOT in O12 is populated (supervisor wires it at boot;
 *     other programs lazily query their parent supervisor — see
 *     dir.c's dir_init for the bootstrap protocol)
 *
 * No private state — every call is a stateless walk + delegate. The
 * fd returned by vfs_open / vfs_opendir is the same hostfsd fd the
 * underlying hf_open returned; vfs_read / vfs_close pass through
 * unchanged. That keeps the read-loop in cmd_cat etc. unchanged
 * across the migration.
 *
 * No-directory fallback
 * ---------------------
 * When DIR_SLOT and BOOT_PARENT_SLOT are both null (e.g. the
 * standalone shell tests that launch a CPU directly without an
 * oriscdir + supervisor), `dir_walk` returns -6 from its `dir_init`
 * bootstrap check. vfs_* detects this and falls back to direct
 * `hf_*` access using the input path verbatim — semantically the
 * entire hostfsd jail acts as a single implicit mount. That keeps
 * the existing shell-test fixtures working without forcing each
 * test to spin up its own oriscdir.
 *
 * Real I/O errors from a daemon that's wired but unresponsive also
 * surface as -6, in which case fallback would mask them. We accept
 * that trade-off: in practice the daemon is either present
 * throughout the program's lifetime or absent throughout, and the
 * user-visible behaviour is "shell tests work without changes",
 * which is the higher-value outcome.
 */

#include "liborisc.h"

/* Path-buffer size for vfs internals. The directory tree's
 * remainder is at most prefix-plus-leftover, both of which are
 * each capped at DIR_PATH_BUF_SIZE (256) inside dir.c, so 512
 * is comfortably above any walk's maximum output. */
#define VFS_REM_BUF_SIZE  512

/* Sentinel from dir_init when neither DIR_SLOT nor BOOT_PARENT_SLOT
 * is populated — see dir.c. We treat this as "no directory wired
 * up" and fall back to direct hostfsd. */
#define VFS_NO_DIRECTORY  (-6)

/* vfs_walk_kind: thin wrapper over dir_walk that drops the
 * remainder buffer (callers who only want the kind shouldn't have
 * to allocate one themselves). On success the resolved ref is
 * still in DIR_RESULT_SLOT (per dir_walk's contract) — useful for
 * future multi-backend code that wants to inspect it. */
int
vfs_walk_kind(const char *path, int *kind_out)
{
	char rem[VFS_REM_BUF_SIZE];
	int rc = dir_walk(path, kind_out, rem, sizeof(rem));
	if (rc == VFS_NO_DIRECTORY) {
		/* Fallback: probe via hf_opendir. If hostfsd accepts the
		 * path, treat it as MOUNT-equivalent so callers that gate
		 * on `kind == MOUNT || kind == DIR` still proceed. */
		int fd = hf_opendir(path);
		if (fd < 0) return fd;
		hf_close(fd);
		if (kind_out) *kind_out = DIR_KIND_MOUNT;
		return 0;
	}
	return rc;
}

/* vfs_open: walk + hand the remainder to hf_open. Returns hostfsd's
 * fd on success, negative on error. */
int
vfs_open(const char *path, int flags)
{
	int kind;
	char rem[VFS_REM_BUF_SIZE];
	int rc = dir_walk(path, &kind, rem, sizeof(rem));
	if (rc == VFS_NO_DIRECTORY) return hf_open(path, flags);
	if (rc < 0) return rc;
	if (kind != DIR_KIND_MOUNT) return -1;
	return hf_open(rem, flags);
}

/* vfs_opendir: same shape as vfs_open but using OP_OPENDIR. Returns
 * a hostfsd fd whose subsequent reads yield directory listing bytes
 * (NUL-separated names, "/"-suffixed for subdirs — see hostfsd's
 * OP_OPENDIR docs). For DIR-resolved paths use vfs_list, which
 * dispatches to dir_list automatically. */
int
vfs_opendir(const char *path)
{
	int kind;
	char rem[VFS_REM_BUF_SIZE];
	int rc = dir_walk(path, &kind, rem, sizeof(rem));
	if (rc == VFS_NO_DIRECTORY) return hf_opendir(path);
	if (rc < 0) return rc;
	if (kind != DIR_KIND_MOUNT) return -1;
	return hf_opendir(rem);
}

/* vfs_close / vfs_read / vfs_write: delegate. The fd is hostfsd's,
 * so no further translation is needed. Kept as wrappers (rather
 * than letting callers use hf_* directly) so the abstraction stays
 * coherent — programs that started with vfs_open finish with
 * vfs_close, even if the implementation is currently trivial. */
int
vfs_close(int fd)
{
	return hf_close(fd);
}

int
vfs_read(int fd, char *buf, int count)
{
	return hf_read(fd, buf, count);
}

int
vfs_write(int fd, const char *buf, int count)
{
	return hf_write(fd, buf, count);
}

/* vfs_list: enumerate `path`. Two regimes:
 *
 *   - DIR (oriscdir-managed, e.g. `/`, `/sys`, `/sys/cpu`):
 *     dir_list returns NUL-separated names directly. We forward
 *     `buf` and `cap` straight through; on success the byte
 *     length is recoverable from buf's contents (caller can
 *     iterate NUL-terminated runs).
 *
 *   - MOUNT (e.g. `/programs`): there's no DIR-listing wire op
 *     on hostfsd's existing protocol — but the existing
 *     hf_opendir + hf_read pair returns the same NUL-separated
 *     name format, just streamed in chunks. We accumulate chunks
 *     into `buf` until we hit a short-read (end of listing) or
 *     run out of capacity. Truncated listings are silently
 *     truncated (matches `cmd_ls`'s prior behaviour, which would
 *     simply stop printing on hf_read failure).
 *
 * Returns the number of BYTES written into buf on success,
 * negative on error. (dir_list's contract is the entry count;
 * we normalize to byte length here so callers don't have to
 * branch on which regime served them.) */
int
vfs_list(const char *path, char *buf, int cap)
{
	int kind;
	char rem[VFS_REM_BUF_SIZE];
	int rc = dir_walk(path, &kind, rem, sizeof(rem));
	if (rc == VFS_NO_DIRECTORY) {
		/* No directory wired up — stream directly from hostfsd
		 * using the input path. */
		int fd = hf_opendir(path);
		if (fd < 0) return fd;
		int total = 0;
		while (total < cap) {
			int got = hf_read(fd, buf + total, cap - total);
			if (got <= 0) break;
			total += got;
		}
		hf_close(fd);
		return total;
	}
	if (rc < 0) return rc;

	if (kind == DIR_KIND_DIR) {
		int count = dir_list(path, buf, cap);
		if (count < 0) return count;
		/* dir_list packs `count` entries as "name1\0name2\0..." —
		 * machine-friendly but useless for a `ls` print: oriscterm
		 * eats embedded NULs, so the names visually collide
		 * ("programs/sys/" instead of two lines). Walk through the
		 * buffer, translate each NUL into a newline (matches
		 * hostfsd's listing format), and stop once we've seen
		 * `count` separators — at which point `n` is the total
		 * byte length to return. Empty directory: count == 0,
		 * loop exits immediately, return 0. */
		int n = 0;
		int found = 0;
		while (found < count && n < cap) {
			if (buf[n] == '\0') {
				buf[n] = '\n';
				found++;
			}
			n++;
		}
		return n;
	}

	if (kind != DIR_KIND_MOUNT) return -1;

	/* MOUNT regime: open the remainder via hostfsd, stream its
	 * listing into buf. */
	int fd = hf_opendir(rem);
	if (fd < 0) return fd;
	int total = 0;
	while (total < cap) {
		int got = hf_read(fd, buf + total, cap - total);
		if (got <= 0) break;
		total += got;
	}
	hf_close(fd);
	return total;
}
