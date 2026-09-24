#ifndef BOM_READ_H
#define BOM_READ_H

#include <stdint.h>
#include <stddef.h>

/* Clean-room BOM reader. Mirrors the subset of the format that lsbom
 * exercises: whole-file image, block index, variable lookup, PathRecord
 * decode. See FORMAT.md for the empirical format description. */

/* PathRecord path_type codes. */
#define BM_PT_FILE    1  /* regular file */
#define BM_PT_DIR     2  /* directory */
#define BM_PT_LINK    3  /* symbolic link */
#define BM_PT_BLOCK   4  /* block device */
#define BM_PT_CHAR    5  /* character device */
#define BM_PT_FIFO    6  /* named pipe (invalid to lsbom) */
#define BM_PT_SOCKET  7  /* socket (invalid to lsbom) */

typedef struct bom_file {
    uint8_t  *data;      /* whole file image (owned) */
    uint32_t  len;       /* image length */
    uint32_t  num_blocks;/* header: number of populated blocks */
    uint32_t *block_off; /* [num_blocks+1]; index 0 unused */
    uint32_t *block_len;
    int       open_failed; /* 1 = open() itself failed */
} bom_file;

/* Decoded PathRecord (FORMAT.md 4.6). `link` points into the block if
 * link_len > 0, else NULL.
 *
 * Mach-O files carry a per-architecture slice table after the 27-byte base:
 * byte 27 = 0x01 flag, bytes 28..31 = BE slice count, then count entries of
 * 16 bytes each {cputype, subtype, size, checksum} (all BE), then the usual
 * link-name tail.  nslice is 0 for non-Mach-O records; `slices` points into
 * the PathRecord block. */
typedef struct bom_pathrec {
    uint8_t      path_type;
    uint16_t     architecture;
    uint16_t     mode;
    uint32_t     uid, gid, mtime, size, checksum;
    const uint8_t *link;
    uint32_t     link_len;
    uint32_t     nslice;
    const uint8_t *slices;
    int          valid;
} bom_pathrec;

/* Reads the whole file and validates the BOMStore header and block index.
 * Returns 0 on success; -1 on failure with errno set (open/read errors keep
 * their errno; short reads and header/volume mismatches set EACCES, which
 * matches lsbom's "Operation not permitted" message). */
int bom_file_open(const char *path, bom_file *bf);

void bom_file_close(bom_file *bf);

/* Returns pointer to block `index` (1-based) and stores its length, or NULL
 * for the null block / out of range / zero-length block (len set to 0). */
const uint8_t *bom_block(const bom_file *bf, uint32_t index, uint32_t *len_out);

/* Decodes a PathRecord from `len` bytes at `b`. `pr->valid` is 0 when the
 * block is too short to be a PathRecord. */
int bom_pathrec_decode(const uint8_t *b, uint32_t len, bom_pathrec *pr);

#endif /* BOM_READ_H */