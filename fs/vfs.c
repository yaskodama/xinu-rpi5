// fs/vfs.c — tmpfs-backed hierarchical filesystem.

#include "vfs.h"
#include "kmalloc.h"

static vfs_node_t g_root = {
    .name     = "",
    .kind     = VFS_DIR,
    .size     = 0,
    .capacity = 0,
    .data     = 0,
    .parent   = 0,
    .children = 0,
    .next     = 0,
};

static unsigned long g_node_count = 1;  /* root counts */

vfs_node_t *vfs_root(void) { return &g_root; }

unsigned long vfs_node_count(void) { return g_node_count; }

unsigned long vfs_total_file_bytes(void)
{
    /* Walk the tree summing every file's size.  Recursive; tree
     * depth in practice is small (< 8 in our demo). */
    unsigned long total = 0;

    /* Manual stack-based DFS would avoid recursion, but with tens of
     * nodes the C stack handles this easily.  */
    vfs_node_t *stack[64];
    int top = 0;
    stack[top++] = &g_root;
    while (top > 0) {
        vfs_node_t *n = stack[--top];
        if (n->kind == VFS_FILE) total += n->size;
        for (vfs_node_t *c = n->children; c && top < 64; c = c->next) {
            stack[top++] = c;
        }
    }
    return total;
}

/* ---------- helpers ---------- */

static int str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (*a == 0 && *b == 0);
}

static int str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    return i;
}

static vfs_node_t *find_child(vfs_node_t *parent, const char *name)
{
    if (parent == 0 || parent->kind != VFS_DIR) return 0;
    for (vfs_node_t *c = parent->children; c; c = c->next) {
        if (str_eq(c->name, name)) return c;
    }
    return 0;
}

static vfs_node_t *new_node(vfs_kind_t kind, const char *name,
                            vfs_node_t *parent)
{
    vfs_node_t *n = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (n == 0) return 0;
    str_copy(n->name, name, VFS_NAME_MAX + 1);
    n->kind     = kind;
    n->size     = 0;
    n->capacity = 0;
    n->data     = 0;
    n->fat_cluster = 0;
    n->parent   = parent;
    n->children = 0;
    n->next     = 0;

    /* Tail-insert into parent's child list for stable iteration order. */
    if (parent) {
        if (parent->children == 0) {
            parent->children = n;
        } else {
            vfs_node_t *t = parent->children;
            while (t->next) t = t->next;
            t->next = n;
        }
    }
    g_node_count++;
    return n;
}

vfs_node_t *vfs_mkdir(vfs_node_t *parent, const char *name)
{
    if (parent == 0 || parent->kind != VFS_DIR) return 0;
    if (find_child(parent, name)) return 0;            /* collision */
    return new_node(VFS_DIR, name, parent);
}

vfs_node_t *vfs_create_file(vfs_node_t *parent, const char *name)
{
    if (parent == 0 || parent->kind != VFS_DIR) return 0;
    if (find_child(parent, name)) return 0;
    return new_node(VFS_FILE, name, parent);
}

int vfs_write(vfs_node_t *file, const void *buf, unsigned long len)
{
    if (file == 0 || file->kind != VFS_FILE) return -1;

    /* Grow buffer if necessary; we don't shrink. */
    if (len > file->capacity) {
        void *nb = kmalloc(len);
        if (nb == 0) return -1;
        if (file->data) kfree(file->data);
        file->data     = nb;
        file->capacity = len;
    }

    const unsigned char *s = (const unsigned char *)buf;
    unsigned char       *d = (unsigned char *)file->data;
    for (unsigned long i = 0; i < len; i++) d[i] = s[i];
    file->size = len;
    return 0;
}

static int (*g_load_hook)(vfs_node_t *);
void vfs_set_load_hook(int (*fn)(vfs_node_t *)) { g_load_hook = fn; }

int vfs_read(vfs_node_t *file, void *buf, unsigned long max)
{
    if (file == 0 || file->kind != VFS_FILE) return -1;
    /* On-demand backing load (e.g. FAT32 file content): the directory walk
     * records size but leaves data NULL; populate it on first read. */
    if (file->data == 0 && file->size > 0 && g_load_hook) g_load_hook(file);
    unsigned long n = file->size < max ? file->size : max;
    const unsigned char *s = (const unsigned char *)file->data;
    unsigned char       *d = (unsigned char *)buf;
    for (unsigned long i = 0; i < n; i++) d[i] = s[i];
    return (int)n;
}

int vfs_write_str(vfs_node_t *file, const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return vfs_write(file, s, n);
}

/* Walk an absolute path creating any missing components.  Intermediate
 * components are always directories; the final one is a file when
 * `last_is_file` is set, else a directory.  Returns the final node, or
 * NULL on OOM / a component that already exists with the wrong kind
 * (e.g. a file used as a directory).  Idempotent: an existing final node
 * of the right kind is returned as-is. */
static vfs_node_t *resolve_create(const char *path, int last_is_file)
{
    if (path == 0 || path[0] != '/') return 0;
    vfs_node_t *cur = &g_root;
    const char *p = path + 1;
    while (*p) {
        char name[VFS_NAME_MAX + 1];
        int i = 0;
        while (*p && *p != '/' && i < VFS_NAME_MAX) name[i++] = *p++;
        name[i] = 0;
        /* last component? (nothing but slashes remain) */
        int is_last = 1;
        for (const char *q = p; *q; q++) if (*q != '/') { is_last = 0; break; }
        if (i > 0) {
            vfs_node_t *child = find_child(cur, name);
            if (child == 0) {
                vfs_kind_t k = (is_last && last_is_file) ? VFS_FILE : VFS_DIR;
                child = new_node(k, name, cur);
                if (child == 0) return 0;
            } else if (!is_last && child->kind != VFS_DIR) {
                return 0;                 /* path goes through a file */
            }
            cur = child;
        }
        if (*p == '/') p++;
    }
    return cur;
}

vfs_node_t *vfs_mkdir_p(const char *path)     { return resolve_create(path, 0); }
vfs_node_t *vfs_create_path(const char *path) { return resolve_create(path, 1); }

vfs_node_t *vfs_lookup(const char *path)
{
    if (path == 0 || path[0] != '/') return 0;
    vfs_node_t *cur = &g_root;
    const char *p = path + 1;
    while (*p) {
        char name[VFS_NAME_MAX + 1];
        int i = 0;
        while (*p && *p != '/' && i < VFS_NAME_MAX) { name[i++] = *p++; }
        name[i] = 0;
        if (i > 0) {
            cur = find_child(cur, name);
            if (cur == 0) return 0;
        }
        if (*p == '/') p++;
    }
    return cur;
}

void vfs_walk(vfs_node_t *node, int depth,
              vfs_visit_fn visit, void *ctx)
{
    if (node == 0) return;
    visit(depth, node, ctx);
    for (vfs_node_t *c = node->children; c; c = c->next) {
        vfs_walk(c, depth + 1, visit, ctx);
    }
}
