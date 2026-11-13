#ifndef NB_FILE_H
#define NB_FILE_H

#include "nb/base.h"
#include "nb/buf.h"
#include "nb/err.h"
#include "nb/vec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Creates caller-owned non-inheritable pipe endpoints; failures set both -1. */
NB_EXPORT nb_status nb_pipe_open(int descriptors[2], nb_err *err);
/* Cancellation-aware full write on an exclusively borrowed fd, preserving
 * its flags and isolating generated SIGPIPE on POSIX. */
NB_EXPORT nb_status nb_fd_write_until(int fd, const void *data, size_t len, int64_t deadline_ns,
                                      int (*cancelled)(void *), void *ctx, nb_err *err);

NB_EXPORT bool nb_file_exists(const char *path);
NB_EXPORT bool nb_file_is_dir(const char *path);
NB_EXPORT nb_status nb_file_size(const char *path, uint64_t *size, nb_err *err);

/* Replaces out on success; failures preserve its contents. path may borrow
 * from out. max_bytes=0 is unlimited; larger inputs return NB_ERR_TOO_BIG.
 * The limit is enforced while reading, including files growing after stat. */
NB_EXPORT nb_status nb_file_read_all(const char *path, nb_buf *out, size_t max_bytes, nb_err *err);
NB_EXPORT nb_status nb_file_write_all(const char *path, const void *data,
                                      size_t len, nb_err *err);

typedef struct nb_file_write_options {
    uint32_t mode;          /* 0 preserves an existing file; new files 0644 */
    uint32_t new_file_mode; /* creation-only mode; 0 falls back to mode/0644 */
    bool create_only;       /* fail with NB_ERR_EXISTS instead of replacing */
    bool sync_dir;          /* fsync the parent directory after publishing */
    bool follow_symlink;    /* false rejects symlinks and non-regular targets */
    bool *published;        /* optional; set true once the name is published */
} nb_file_write_options;

/* Atomically replaces path with len bytes from data. options may be NULL;
 * NULL uses mode 0, create_only=false, sync_dir=true, follow_symlink=false.
 *
 * The data is written to a temporary file created with mkstemp in the same
 * directory as path, flushed with fsync, closed, and renamed over path. The
 * temporary file is removed on every failure path. With a path containing no
 * '/', the temporary file is created in the current directory. An existing
 * target keeps its permission bits unless options->mode is nonzero; a new file
 * uses options->mode or 0644. create_only publishes without replacing an
 * existing target, sync_dir fsyncs the parent after publishing, and a false
 * follow_symlink rejects symlinks and non-regular targets.
 *
 * An explicit mode overrides both existing and new files. When mode is zero,
 * an existing target keeps its permission bits and a new file uses
 * new_file_mode, falling back to 0644. When published is non-NULL it is set
 * false on entry and true as soon as the final name is published; a later
 * parent-directory sync failure still reports true.
 *
 * Returns NB_ERR_INVAL for invalid arguments, NB_ERR_NOMEM when the temporary
 * path cannot be allocated, and NB_ERR_IO with os_errno set for system call
 * failures. */
NB_EXPORT nb_status nb_file_write_all_atomic(const char *path, const void *data,
                                             size_t len,
                                             const nb_file_write_options *options,
                                             nb_err *err);

NB_EXPORT nb_status nb_file_append(const char *path, const void *data,
                                   size_t len, nb_err *err);
NB_EXPORT nb_status nb_file_remove(const char *path, nb_err *err);
NB_EXPORT nb_status nb_file_rename(const char *from, const char *to, nb_err *err);
/* Fails with NB_ERR_EXISTS when to already exists. */
NB_EXPORT nb_status nb_file_rename_noreplace(const char *from, const char *to,
                                             nb_err *err);
NB_EXPORT nb_status nb_file_mkdir(const char *path, bool recursive, nb_err *err);
NB_EXPORT nb_status nb_file_rmdir(const char *path, nb_err *err);

/* Creates a new temporary file and writes its path to path_out. The caller
 * owns the file and is responsible for removing it. */
NB_EXPORT nb_status nb_file_temp(nb_buf *path_out, nb_err *err);
/* Creates a new temporary file inside dir. A non-NULL fd_out must point to
 * -1 and receives an owned non-inheritable read/write binary descriptor on success;
 * other readers may open its path while the descriptor remains owned;
 * NULL closes the descriptor. Failures preserve both outputs. */
NB_EXPORT nb_status nb_file_temp_in(nb_buf *path_out, const char *dir, int *fd_out, nb_err *err);
/* Creates a new private temporary directory inside dir, or the system temp
 * directory when dir is NULL. prefix is a filename component (NULL uses
 * "nbase-"). The caller owns and removes the directory. */
NB_EXPORT nb_status nb_file_temp_dir(nb_buf *path_out, const char *dir, const char *prefix,
                                     nb_err *err);
/* Flushes directory metadata after rename/create when supported. */
NB_EXPORT nb_status nb_file_sync_dir(const char *dir, nb_err *err);

typedef struct nb_file_info {
    uint64_t size;
    int64_t mtime_ns;
    uint32_t mode;
    bool is_regular;
    bool is_dir;
    bool is_symlink;
    bool is_char_device;
    bool is_block_device;
    bool is_fifo;
    bool is_socket;
} nb_file_info;

/* follow_links=false reports the link itself. On Windows, mode is a
 * synthetic read-only/read-write value. */
NB_EXPORT nb_status nb_file_stat(const char *path, bool follow_links,
                                 nb_file_info *out, nb_err *err);

typedef struct nb_file_lock nb_file_lock;

/* Advisory cross-process lock. The lock file is created if missing and is not
 * removed on release. A non-blocking conflict returns NB_ERR_AGAIN. */
NB_EXPORT nb_status nb_file_lock_acquire(const char *path, bool exclusive,
                                         bool blocking, nb_file_lock **out,
                                         nb_err *err);
NB_EXPORT void nb_file_lock_release(nb_file_lock *lock);

/* Sets permission bits. On Windows only the write bits are meaningful: a
 * writable mode clears FILE_ATTRIBUTE_READONLY, a read-only mode sets it. */
NB_EXPORT nb_status nb_file_set_mode(const char *path, uint32_t mode,
                                     nb_err *err);

/* Creates a symbolic link at link_path pointing to target. Windows may
 * require Developer Mode or SeCreateSymbolicLinkPrivilege and reports a
 * privilege failure as NB_ERR_UNSUPPORTED. */
NB_EXPORT nb_status nb_file_symlink(const char *target, const char *link_path,
                                    nb_err *err);

/* Reads a symbolic link target into out, replacing it only on success. */
NB_EXPORT nb_status nb_file_readlink(nb_buf *out, const char *path,
                                     nb_err *err);

typedef struct nb_file_copy_entry {
    nb_file_info info;
    size_t depth; /* root is zero */
    /* Borrowed for the filter call; this exact target is used when copying. */
    const char *symlink_target;
} nb_file_copy_entry;

typedef nb_status (*nb_file_copy_filter)(const char *source, const char *destination,
                                         const nb_file_copy_entry *entry, bool *include,
                                         void *context, nb_err *err);

typedef struct nb_file_copy_options {
    bool recursive;      /* copy directory trees */
    bool preserve_mode;  /* apply the source permission bits */
    bool copy_symlinks;  /* recreate links instead of following them */
    bool overwrite;      /* replace existing destination entries */
    bool nofollow_source; /* reject a source symlink, including at open time */
    /* New regular files when preserve_mode is false; zero uses 0644.
     * Existing destination modes remain unchanged. */
    uint32_t new_file_mode;
    uint32_t directory_mode; /* nonzero overrides directory permission bits */
    size_t max_depth;        /* maximum child depth; zero is unlimited */
    nb_file_copy_filter filter;
    void *filter_context;
} nb_file_copy_options;

/* Copies src to dst. options may be NULL; defaults are recursive=false,
 * preserve_mode=true, copy_symlinks=true, overwrite=false. An existing
 * directory is merged when overwrite is true. Special files and recursive
 * copies through directory symlinks return NB_ERR_UNSUPPORTED. Regular file
 * content is streamed through a temporary file in the destination directory
 * and published atomically; memory use does not grow with file size. A filter
 * runs before destination changes, with include initialized true. Setting it
 * false skips an entry and its subtree. Returning an error stops the copy;
 * newly created destination directories are removed on failure. Existing
 * merged destinations retain completed writes. Filter strings and entry views
 * are borrowed only for the call. */
NB_EXPORT nb_status nb_file_copy(const char *src, const char *dst,
                                 const nb_file_copy_options *options,
                                 nb_err *err);

/* Removes path recursively without following symbolic links. A missing path
 * is a successful no-op. max_depth is the maximum child depth; zero is
 * unlimited. A bound violation returns NB_ERR_LOOP. */
NB_EXPORT nb_status nb_file_remove_all(const char *path, size_t max_depth, nb_err *err);

/* Path builders replace out only on success; inputs may borrow from out. */
NB_EXPORT nb_status nb_path_join(nb_buf *out, const char *a, const char *b);
/* Appends a path component without discarding a when b begins with a separator.
 * Exactly one separator is retained at the join boundary. Replaces out on
 * success, preserving it on failure. Inputs may borrow out. */
NB_EXPORT nb_status nb_path_concat(nb_buf *out, const char *a, const char *b);
/* Expands a leading '~' using HOME (USERPROFILE when HOME is absent).
 * Other paths are copied. No shell expansion or filesystem access occurs. */
NB_EXPORT nb_status nb_path_expand(nb_buf *out, const char *path, nb_err *err);
NB_EXPORT nb_status nb_path_absolute(nb_buf *out, const char *path, nb_err *err);

/* Resolves the longest existing ancestor of path with realpath semantics and
 * appends the components that do not exist yet. The output is absolute. The
 * unresolved tail is normalized for "." and repeated separators but rejects
 * ".." with NB_ERR_INVAL, since an uncreated directory has no filesystem
 * meaning for that component. */
NB_EXPORT nb_status nb_path_canonical_missing(nb_buf *out, const char *path, nb_err *err);

/* Returns true when path is absolute on the current platform. NULL and empty
 * paths are not absolute. Windows drive-relative paths ("C:foo") are not
 * absolute. */
NB_EXPORT bool nb_path_is_absolute(const char *path);

/* Resolves symbolic links and writes a canonical absolute path to out.
 * Missing paths fail with NB_ERR_NOT_FOUND and leave out unchanged. */
NB_EXPORT nb_status nb_path_canonical(nb_buf *out, const char *path,
                                      nb_err *err);

/* True when both paths canonicalize and path is root or a descendant of root.
 * allow_missing resolves the longest existing ancestor plus safe missing tail;
 * false requires both paths to exist. Any resolution failure returns false. */
NB_EXPORT bool nb_path_is_within(const char *path, const char *root, bool allow_missing);

/* Produces a relative path only when path is root or its descendant. A
 * path outside root returns NB_ERR_PERMISSION; an exact match yields an
 * empty string. Canonicalization failures preserve out and propagate. */
NB_EXPORT nb_status nb_path_relative_under(nb_buf *out, const char *path, const char *root,
                                           bool allow_missing, nb_err *err);

/* Absolute path of the running executable. */
NB_EXPORT nb_status nb_path_executable(nb_buf *out, nb_err *err);

/* Platform temporary directory (TMPDIR on POSIX, GetTempPath on Windows). */
NB_EXPORT nb_status nb_path_temp_dir(nb_buf *out, nb_err *err);

/* Splits path lexically into borrowed directory and name slices. Trailing
 * separators are ignored. The directory slice is empty for a relative path
 * without a separator and is the root for a root path. The name slice is empty
 * when path has no final component. Neither slice is NUL-terminated. NULL or
 * empty input returns NB_ERR_INVAL. */
NB_EXPORT nb_status nb_path_split(const char *path, nb_str *dir_out,
                                  nb_str *name_out);

/* Writes the directory part of path to out, replacing it only on success.
 * A relative path without a separator yields ".". */
NB_EXPORT nb_status nb_path_dirname(nb_buf *out, const char *path);

/* Replaces the final extension of path and writes the result to out,
 * replacing it only on success. Leading '.' characters in extension are
 * ignored; NULL or an empty extension removes the existing extension. The
 * directory part is copied unchanged. A path whose final component is "." or
 * "..", ends with a separator, or an extension containing a platform
 * separator returns NB_ERR_INVAL. */
NB_EXPORT nb_status nb_path_set_extension(nb_buf *out, const char *path,
                                          const char *extension);

/* Lexically normalizes path into out, replacing it on success.
 *
 * Repeated '/' characters are collapsed, "." segments are removed, and ".."
 * removes the preceding segment. ".." at an absolute root remains "/";
 * leading ".." segments of a relative path are preserved. The result remains
 * absolute or relative according to the input. Trailing '/' characters are
 * removed except for the root "/", and an empty relative result becomes ".".
 *
 * The function does not resolve symbolic links or access the file system.
 * NULL or empty input returns NB_ERR_INVAL; allocation failure returns
 * NB_ERR_NOMEM. */
NB_EXPORT nb_status nb_path_clean(nb_buf *out, const char *path);

NB_EXPORT const char *nb_path_basename(const char *path);
NB_EXPORT const char *nb_path_extension(const char *path);

typedef enum nb_dir_entry_type {
    NB_DIR_ENTRY_UNKNOWN = 0,
    NB_DIR_ENTRY_FILE,
    NB_DIR_ENTRY_DIR,
    NB_DIR_ENTRY_SYMLINK,
    NB_DIR_ENTRY_CHAR,
    NB_DIR_ENTRY_BLOCK,
    NB_DIR_ENTRY_FIFO,
    NB_DIR_ENTRY_SOCKET
} nb_dir_entry_type;

typedef struct nb_dir_entry {
    const char *name; /* borrowed until the next nb_dir_next call */
    nb_dir_entry_type type;
} nb_dir_entry;

typedef struct nb_dir nb_dir;

NB_EXPORT nb_dir *nb_dir_open(const char *path, nb_err *err);
/* Returns the next entry name, or NULL at end of directory or on error.
 * "." and ".." are skipped. entry may be NULL; when non-NULL it receives the
 * entry name and type, and name remains valid until the next call. */
NB_EXPORT const char *nb_dir_next(nb_dir *dir, nb_dir_entry *entry);
NB_EXPORT nb_status nb_dir_status(const nb_dir *dir);
NB_EXPORT void nb_dir_close(nb_dir *dir);

typedef enum nb_dir_scan_flags {
    NB_DIR_SCAN_FILES = 1u << 0,
    NB_DIR_SCAN_DIRS = 1u << 1,
    NB_DIR_SCAN_HIDDEN = 1u << 2,
    NB_DIR_SCAN_FOLLOW_LINKS = 1u << 3,
    NB_DIR_SCAN_SORT = 1u << 4,
    NB_DIR_SCAN_ALL = 1u << 5
} nb_dir_scan_flags;

/* An independent snapshot of names, each held in a native nb_buf. */
typedef struct nb_dir_listing {
    nb_vec names;
    bool initialized;
} nb_dir_listing;

/* out must be empty. Failure preserves it. Entries can disappear during a
 * scan; a failed stat skips that entry. Other enumeration failures propagate. */
NB_EXPORT nb_status nb_dir_scan(const char *path, unsigned flags, nb_dir_listing *out, nb_err *err);
NB_EXPORT void nb_dir_listing_destroy(nb_dir_listing *listing);
NB_EXPORT const char *nb_dir_listing_name(const nb_dir_listing *listing, size_t index);

#ifdef __cplusplus
}
#endif

#endif /* NB_FILE_H */
