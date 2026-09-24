#ifndef BOM_CKSUM_H
#define BOM_CKSUM_H

#include <stddef.h>
#include <stdint.h>

/* Clean-room reimplementation of the BSD "cksum" CRC-32 used for path
 * record checksums (Apple's libstuff crc32.c algorithm: MSB-first table
 * over poly 0x04c11db7, start 0, byte length folded low-byte-first, then
 * complemented).  Verified against /usr/bin/cksum. */

typedef struct bm_cksum_ctx {
    uint32_t crc;
} bm_cksum_ctx;

void     bm_cksum_init(bm_cksum_ctx *ctx);
void     bm_cksum_feed(bm_cksum_ctx *ctx, const void *data, size_t len);
uint32_t bm_cksum_finish(bm_cksum_ctx *ctx, size_t data_len);

uint32_t bm_cksum(const void *data, size_t len);

#endif /* BOM_CKSUM_H */