/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * Clean-room reimplementation of BSD "cksum" CRC-32 (see bom_cksum.h). */
#include "bom_cksum.h"

static uint32_t tab[256];

static void tab_init(void) {
    if (tab[1] != 0)
        return;
    uint32_t c;
    int i, b;
    for (i = 1; i < 256; i++) {
        c = (uint32_t)i << 24;
        for (b = 0; b < 8; b++)
            c = (c & 0x80000000u) ? ((c << 1) ^ 0x04c11db7u) : (c << 1);
        tab[i] = c;
    }
}

void bm_cksum_init(bm_cksum_ctx *ctx) {
    tab_init();
    ctx->crc = 0;
}

void bm_cksum_feed(bm_cksum_ctx *ctx, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    uint32_t crc = ctx->crc;
    size_t i;
    for (i = 0; i < len; i++)
        crc = (crc << 8) ^ tab[((crc >> 24) ^ p[i]) & 0xff];
    ctx->crc = crc;
}

uint32_t bm_cksum_finish(bm_cksum_ctx *ctx, size_t data_len) {
    uint32_t crc = ctx->crc;
    while (data_len) {
        crc = (crc << 8) ^ tab[((crc >> 24) ^ (data_len & 0xff)) & 0xff];
        data_len >>= 8;
    }
    return ~crc;
}

uint32_t bm_cksum(const void *data, size_t len) {
    bm_cksum_ctx ctx;
    bm_cksum_init(&ctx);
    bm_cksum_feed(&ctx, data, len);
    return bm_cksum_finish(&ctx, len);
}