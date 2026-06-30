// include/fat32.h — read-only FAT32 reader on top of sd_read_block.
//
// Single-partition mode: we expect the SD card's first partition to
// start at the LBA the MBR points to (typically LBA 8192 on a Pi OS
// card), and that partition to be FAT32 formatted.  We parse the
// BIOS Parameter Block in the first sector of the partition, then
// walk the root directory's cluster chain to enumerate every entry.
//
// All directory entries are reported to a visitor callback; the
// caller (loader/main.c) inserts them into the VFS under /sd/.
// Long-filename (LFN) entries are skipped, so users see the 8.3
// short name (e.g. CONFIG~1.TXT) — that's a deliberate trade-off
// for keeping this reader small.  Most Pi boot files have 8.3 names
// natively.

#ifndef XINU_RPI5_FAT32_H
#define XINU_RPI5_FAT32_H

/* Block-device hooks: fat32_mount() binds the on-board microSD; fat32_mount_dev()
 * lets a caller bind any 512-byte block device (e.g. a USB mass-storage stick),
 * so the same reader serves both. */
typedef int (*fat32_rdfn)(unsigned long lba, void *buf);
typedef int (*fat32_wrfn)(unsigned long lba, const void *buf);

/* Returned by fat32_mount() — opaque to callers but the struct is
 * exposed so they can keep a stack-allocated instance. */
typedef struct {
    unsigned int  bytes_per_sector;
    unsigned int  sectors_per_cluster;
    unsigned int  reserved_sectors;
    unsigned int  num_fats;
    unsigned int  sectors_per_fat;
    unsigned int  root_cluster;

    /* Derived: absolute LBA bases. */
    unsigned long part_lba;          /* first sector of the partition  */
    unsigned long fat_lba;            /* first FAT entry sector         */
    unsigned long data_lba;           /* cluster 2's first sector       */

    /* Bound block device (set by fat32_mount / fat32_mount_dev). */
    fat32_rdfn    rd;
    fat32_wrfn    wr;
} fat32_t;

/* Read MBR to find the first partition, read its boot sector and
 * populate `fs`.  Returns 0 on success, -1 if anything is unexpected
 * (no MBR signature, partition isn't FAT32, etc.). */
int fat32_mount(fat32_t *fs);

/* Mount a FAT32 volume on an arbitrary 512-byte block device (e.g. USB MSD).
 * `rd`/`wr` read/write one block; `wr` may be NULL for read-only media.
 * Returns 0 on success, -1 if not a FAT32 volume. */
int fat32_mount_dev(fat32_t *fs, fat32_rdfn rd, fat32_wrfn wr);

/* Visitor callback signature: called once per non-LFN, non-deleted
 * directory entry.  `is_dir` is non-zero if the entry is a sub-
 * directory.  `first_cluster` lets the caller recurse into a sub-
 * directory by calling fat32_walk_dir() again. */
typedef void (*fat32_visit_fn)(const char  *name,
                               int          is_dir,
                               unsigned long size,
                               unsigned int first_cluster,
                               int          depth,
                               void        *ctx);

/* Walk the directory whose first cluster is `cluster`, calling
 * `visit` for each entry.  `depth` is passed through verbatim so
 * the caller can produce indented output; for a top-level walk
 * pass 0.  Returns 0 on success. */
int fat32_walk_dir(fat32_t *fs, unsigned int cluster, int depth,
                   fat32_visit_fn visit, void *ctx);

/* Read a file's contents (cluster-chain walk) into `buf`; copies up to
 * min(size, maxlen) bytes.  Returns bytes copied, or -1 on error. */
int fat32_read_file(fat32_t *fs, unsigned int first_cluster,
                    unsigned long size, void *buf, unsigned long maxlen);

/* Write a small file (<= one cluster) named `name` (8.3) into the directory
 * whose first cluster is `dir_cluster`, allocating one free cluster and adding/
 * overwriting the directory entry.  Returns 0 on success, -1 on error. */
int fat32_write_file(fat32_t *fs, unsigned int dir_cluster, const char *name,
                     const void *buf, unsigned long len);

#endif /* XINU_RPI5_FAT32_H */
