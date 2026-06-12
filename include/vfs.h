// include/vfs.h — minimal hierarchical in-memory file system.
//
// Single global namespace rooted at "/".  Each node is either a
// directory (containing child nodes) or a regular file (containing a
// byte payload allocated via kmalloc).  All paths are absolute strings
// of the form "/dir/dir/.../file" — no relative paths, no current
// working directory.  No mount points, no symlinks, no hard links.
//
// Storage backend = tmpfs only: everything lives in RAM and is lost
// at reboot.  Persistent backends (FAT32 on the SD card, ext2/ext4
// on the Pi OS rootfs partition) can be added later as alternative
// implementations of the same vfs_node_t interface.

#ifndef XINU_RPI5_VFS_H
#define XINU_RPI5_VFS_H

#define VFS_NAME_MAX  31

typedef enum {
    VFS_DIR,
    VFS_FILE
} vfs_kind_t;

typedef struct vfs_node {
    char           name[VFS_NAME_MAX + 1];
    vfs_kind_t     kind;
    unsigned long  size;       /* bytes (files) or 0 (dirs)         */
    unsigned long  capacity;   /* allocated bytes in `data` buffer  */
    void          *data;       /* file payload (NULL until loaded)  */
    unsigned int   fat_cluster;/* FAT32 first cluster for on-demand load (0 = tmpfs) */
    struct vfs_node *parent;
    struct vfs_node *children; /* head of child list  (dirs only)   */
    struct vfs_node *next;     /* sibling link                       */
} vfs_node_t;

/* Register a loader called by vfs_read when a file has a backing store but no
 * in-RAM payload yet (data==NULL, size>0).  The hook should populate node->data
 * (e.g. via vfs_write) from the backing FS.  Returns 0 on success. */
void vfs_set_load_hook(int (*fn)(vfs_node_t *node));

/* The implicit root "/" node.  Statically allocated; never freed. */
vfs_node_t *vfs_root(void);

/* Create a child directory / file under `parent`.  Returns the new
 * node, or NULL on OOM / name collision / invalid parent. */
vfs_node_t *vfs_mkdir(vfs_node_t *parent, const char *name);
vfs_node_t *vfs_create_file(vfs_node_t *parent, const char *name);

/* Path-based creation with automatic intermediate directories ("mkdir -p").
 * vfs_mkdir_p makes the final component a directory; vfs_create_path makes it
 * a regular file.  Both return the final node (the existing one if already
 * present with the right kind), or NULL on OOM / a file used as a directory. */
vfs_node_t *vfs_mkdir_p(const char *path);
vfs_node_t *vfs_create_path(const char *path);

/* Write/read raw bytes to/from a regular file.  vfs_write replaces
 * the existing contents (the file is grown via kmalloc as needed).
 * vfs_read copies up to `max` bytes from offset 0 into `buf` and
 * returns the number of bytes actually transferred. */
int vfs_write(vfs_node_t *file, const void *buf, unsigned long len);
int vfs_read (vfs_node_t *file, void *buf, unsigned long max);

/* Convenience: write the C string `s` (without trailing NUL) into
 * `file`, replacing existing contents.  Returns 0 / -1. */
int vfs_write_str(vfs_node_t *file, const char *s);

/* Resolve an absolute path "/foo/bar".  Returns the matching node
 * or NULL.  Trailing slash is allowed.  Path "/" is the root. */
vfs_node_t *vfs_lookup(const char *path);

/* Depth-first walk: visit(depth, node, ctx) is called for every
 * reachable node, with `depth` counting the number of ancestors
 * (root = 0).  Useful for the File-Tree window. */
typedef void (*vfs_visit_fn)(int depth, vfs_node_t *node, void *ctx);
void vfs_walk(vfs_node_t *node, int depth, vfs_visit_fn visit, void *ctx);

/* Tally for the Memory window. */
unsigned long vfs_node_count(void);
unsigned long vfs_total_file_bytes(void);

#endif /* XINU_RPI5_VFS_H */
