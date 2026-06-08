// fs/fat32.c — read-only FAT32 reader.

#include "fat32.h"
#include "sd.h"

/* Little-endian unpackers — FAT32 metadata is always LE on disk. */
static unsigned int le16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}
static unsigned int le32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

/* One sector worth of scratch — re-used by directory and FAT walks
 * to keep stack pressure low. */
static unsigned char scratch[SD_BLOCK_SIZE];

int fat32_mount(fat32_t *fs)
{
    unsigned char mbr[SD_BLOCK_SIZE];
    if (sd_read_block(0, mbr) != 0)             return -1;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA)   return -1;

    /* First partition table entry starts at MBR offset 0x1BE.
     *   +0x04 = partition type byte (0x0B / 0x0C for FAT32)
     *   +0x08 = LBA of first sector (LE u32) */
    unsigned char ptype = mbr[0x1BE + 0x04];
    if (ptype != 0x0B && ptype != 0x0C)         return -1;

    fs->part_lba = le32(&mbr[0x1BE + 0x08]);

    /* Read the partition's boot sector to get the BPB. */
    if (sd_read_block(fs->part_lba, scratch) != 0) return -1;
    if (scratch[510] != 0x55 || scratch[511] != 0xAA) return -1;

    fs->bytes_per_sector   = le16(&scratch[0x0B]);
    fs->sectors_per_cluster= scratch[0x0D];
    fs->reserved_sectors   = le16(&scratch[0x0E]);
    fs->num_fats           = scratch[0x10];
    fs->sectors_per_fat    = le32(&scratch[0x24]);
    fs->root_cluster       = le32(&scratch[0x2C]);

    if (fs->bytes_per_sector  != SD_BLOCK_SIZE) return -1;
    if (fs->sectors_per_cluster == 0)           return -1;
    if (fs->root_cluster < 2)                    return -1;

    fs->fat_lba  = fs->part_lba + fs->reserved_sectors;
    fs->data_lba = fs->fat_lba
                 + (unsigned long)fs->num_fats * fs->sectors_per_fat;

    return 0;
}

/* Translate a cluster number to the LBA of its first sector. */
static unsigned long cluster_to_lba(const fat32_t *fs, unsigned int cluster)
{
    return fs->data_lba
         + (unsigned long)(cluster - 2) * fs->sectors_per_cluster;
}

/* Read the FAT entry for `cluster`.  Returns the next-cluster value;
 * >= 0x0FFFFFF8 means end of chain.  Returns 0x0FFFFFFF on read
 * error so the caller terminates the walk. */
static unsigned int fat32_next_cluster(fat32_t *fs, unsigned int cluster)
{
    unsigned long fat_byte = (unsigned long)cluster * 4UL;
    unsigned long sec = fs->fat_lba + (fat_byte / SD_BLOCK_SIZE);
    unsigned int  off = (unsigned int)(fat_byte % SD_BLOCK_SIZE);

    if (sd_read_block(sec, scratch) != 0) return 0x0FFFFFFFu;
    unsigned int v = le32(&scratch[off]) & 0x0FFFFFFFu;
    return v;
}

/* Format an 8.3 directory entry name into a NUL-terminated C string.
 * Trailing spaces are trimmed; a `.` is inserted between base and
 * extension if the extension is non-empty. */
static void format_8_3(const unsigned char *raw, char *out, int outlen)
{
    int o = 0;
    /* 8-char base name */
    for (int i = 0; i < 8 && o < outlen - 1; i++) {
        if (raw[i] == ' ') break;
        out[o++] = (char)raw[i];
    }
    /* extension */
    int ext_present = 0;
    for (int i = 8; i < 11; i++) {
        if (raw[i] != ' ') { ext_present = 1; break; }
    }
    if (ext_present && o < outlen - 1) out[o++] = '.';
    for (int i = 8; i < 11 && o < outlen - 1; i++) {
        if (raw[i] == ' ') break;
        out[o++] = (char)raw[i];
    }
    out[o] = 0;
}

/* Read a file's contents by walking its cluster chain.  Copies up to
 * min(size, maxlen) bytes into `buf`.  Returns bytes copied, or -1 on error. */
int fat32_read_file(fat32_t *fs, unsigned int first_cluster,
                    unsigned long size, void *buf, unsigned long maxlen)
{
    if (first_cluster < 2) return 0;
    unsigned long want = size < maxlen ? size : maxlen;
    unsigned long done = 0;
    unsigned char *out = (unsigned char *)buf;
    unsigned int   cur = first_cluster;
    int            safety = 1 << 20;
    unsigned long  cluster_bytes = (unsigned long)fs->sectors_per_cluster * SD_BLOCK_SIZE;

    while (cur >= 2 && cur < 0x0FFFFFF8u && done < want && safety-- > 0) {
        unsigned long base = cluster_to_lba(fs, cur);
        for (unsigned int s = 0; s < fs->sectors_per_cluster && done < want; s++) {
            if (sd_read_block(base + s, scratch) != 0) return (int)done;
            unsigned long n = want - done;
            if (n > SD_BLOCK_SIZE) n = SD_BLOCK_SIZE;
            for (unsigned long i = 0; i < n; i++) out[done + i] = scratch[i];
            done += n;
        }
        (void)cluster_bytes;
        cur = fat32_next_cluster(fs, cur);
    }
    return (int)done;
}

/* ---- write support (single-cluster small files into the root dir) ----
 * Allocates ONE free cluster, writes up to one cluster of data, links it as a
 * 1-cluster chain (EOC) in every FAT copy, and adds/overwrites an 8.3 directory
 * entry in `dir_cluster`.  Returns 0 on success, -1 on error (incl. file larger
 * than one cluster, or no free cluster / dir slot).  Deliberately minimal — the
 * microSD is the safe scratch target and most config/text files fit one
 * cluster (32 KB at 64 sectors/cluster). */

/* Write a 32-bit LE value into a FAT entry for `cluster`, in all FAT copies. */
static int fat_set_entry(fat32_t *fs, unsigned int cluster, unsigned int value)
{
    unsigned long fat_byte = (unsigned long)cluster * 4UL;
    unsigned int  off = (unsigned int)(fat_byte % SD_BLOCK_SIZE);
    for (unsigned int f = 0; f < fs->num_fats; f++) {
        unsigned long sec = fs->fat_lba + (unsigned long)f * fs->sectors_per_fat
                          + (fat_byte / SD_BLOCK_SIZE);
        if (sd_read_block(sec, scratch) != 0) return -1;
        scratch[off+0] = value & 0xFF;        scratch[off+1] = (value >> 8) & 0xFF;
        scratch[off+2] = (value >> 16) & 0xFF; scratch[off+3] = (value >> 24) & 0x0F
                                              | (scratch[off+3] & 0xF0);  /* top nibble reserved */
        if (sd_write_block(sec, scratch) != 0) return -1;
    }
    return 0;
}

/* Scan the FAT for the first free cluster (entry == 0), starting at cluster 2. */
static unsigned int fat_find_free(fat32_t *fs)
{
    unsigned long total = (unsigned long)fs->sectors_per_fat * (SD_BLOCK_SIZE / 4);
    for (unsigned int c = 2; c < total; c++) {
        if ((fat32_next_cluster(fs, c) & 0x0FFFFFFFu) == 0) return c;
    }
    return 0;
}

/* Format a C string name into 8.3 raw form (11 bytes, space-padded, upper). */
static void name_to_8_3(const char *name, unsigned char raw[11])
{
    for (int i = 0; i < 11; i++) raw[i] = ' ';
    int i = 0, o = 0;
    while (name[i] && name[i] != '.' && o < 8) {
        char c = name[i++]; if (c>='a'&&c<='z') c -= 32; raw[o++] = (unsigned char)c;
    }
    while (name[i] && name[i] != '.') i++;      /* skip rest of base */
    if (name[i] == '.') { i++; o = 8;
        while (name[i] && o < 11) { char c=name[i++]; if(c>='a'&&c<='z') c-=32; raw[o++]=(unsigned char)c; } }
}

int fat32_write_file(fat32_t *fs, unsigned int dir_cluster, const char *name,
                     const void *buf, unsigned long len)
{
    unsigned long cluster_bytes = (unsigned long)fs->sectors_per_cluster * SD_BLOCK_SIZE;
    if (len > cluster_bytes) return -1;          /* single-cluster only (for now) */

    unsigned int newc = fat_find_free(fs);
    if (newc < 2) return -1;
    /* Mark the cluster end-of-chain FIRST so a crash can't leave a dangling alloc. */
    if (fat_set_entry(fs, newc, 0x0FFFFFFFu) != 0) return -1;

    /* Write the data: fill cluster sectors, zero-padding the tail. */
    unsigned long base = cluster_to_lba(fs, newc);
    const unsigned char *src = (const unsigned char *)buf;
    unsigned long done = 0;
    for (unsigned int s = 0; s < fs->sectors_per_cluster; s++) {
        for (int i = 0; i < SD_BLOCK_SIZE; i++) {
            scratch[i] = (done < len) ? src[done] : 0;
            if (done < len) done++;
        }
        if (sd_write_block(base + s, scratch) != 0) return -1;
        if (done >= len && s+1 < fs->sectors_per_cluster) {
            /* remaining sectors stay as-is on disk; we only needed to cover data */
        }
    }

    /* Add / overwrite the 8.3 directory entry in dir_cluster. */
    unsigned char raw[11]; name_to_8_3(name, raw);
    unsigned int cur = dir_cluster; int safety = 1024;
    while (cur >= 2 && cur < 0x0FFFFFF8u && safety-- > 0) {
        unsigned long dbase = cluster_to_lba(fs, cur);
        for (unsigned int s = 0; s < fs->sectors_per_cluster; s++) {
            unsigned long sec = dbase + s;
            if (sd_read_block(sec, scratch) != 0) return -1;
            for (unsigned int off = 0; off + 32 <= SD_BLOCK_SIZE; off += 32) {
                unsigned char *e = &scratch[off];
                int match = 1;
                for (int i = 0; i < 11; i++) if (e[i] != raw[i]) { match = 0; break; }
                if (e[0] == 0x00 || e[0] == 0xE5 || match) {
                    for (int i = 0; i < 32; i++) e[i] = 0;
                    for (int i = 0; i < 11; i++) e[i] = raw[i];
                    e[11] = 0x20;                                   /* archive attr */
                    e[20] = (newc >> 16) & 0xFF; e[21] = (newc >> 24) & 0xFF;  /* hi */
                    e[26] = newc & 0xFF;         e[27] = (newc >> 8) & 0xFF;   /* lo */
                    e[28] = len & 0xFF;          e[29] = (len >> 8) & 0xFF;
                    e[30] = (len >> 16) & 0xFF;  e[31] = (len >> 24) & 0xFF;
                    if (sd_write_block(sec, scratch) != 0) return -1;
                    return 0;
                }
            }
        }
        cur = fat32_next_cluster(fs, cur);
    }
    return -1;                                    /* no free directory slot */
}

int fat32_walk_dir(fat32_t *fs, unsigned int cluster, int depth,
                   fat32_visit_fn visit, void *ctx)
{
    unsigned char dirsec[SD_BLOCK_SIZE];
    unsigned int  cur = cluster;
    int           safety = 1024;  /* bound the cluster-chain walk    */

    while (cur >= 2 && cur < 0x0FFFFFF8u && safety-- > 0) {
        unsigned long base = cluster_to_lba(fs, cur);
        for (unsigned int s = 0; s < fs->sectors_per_cluster; s++) {
            if (sd_read_block(base + s, dirsec) != 0) return -1;

            for (unsigned int off = 0; off + 32 <= SD_BLOCK_SIZE; off += 32) {
                unsigned char *e = &dirsec[off];

                if (e[0] == 0x00) return 0;   /* end-of-directory marker */
                if (e[0] == 0xE5) continue;   /* deleted entry           */
                if ((e[11] & 0x0F) == 0x0F) continue;  /* LFN chunk      */
                if (e[11] & 0x08) continue;   /* volume label            */

                char name[16];
                format_8_3(e, name, sizeof name);

                /* Skip "." and ".." pseudo-entries to avoid loops. */
                if (name[0] == '.' && (name[1] == 0
                    || (name[1] == '.' && name[2] == 0))) continue;

                int           is_dir = (e[11] & 0x10) ? 1 : 0;
                unsigned long size   = le32(&e[28]);
                unsigned int  first  = (le16(&e[20]) << 16) | le16(&e[26]);

                visit(name, is_dir, size, first, depth, ctx);
            }
        }
        cur = fat32_next_cluster(fs, cur);
    }
    return 0;
}
