/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * Clean-room BOMStorage: the BOMStore container surface of Bom.framework
 * (see BOM.h for the derived ABI contract).  Read side only; write/create
 * keeps the file untouched until commit semantics differ from Apple's (our
 * tools write via bom_writer.c, not through this API). */
#include "bom_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t _bom_r32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static BOMStorage *storage_alloc(void) {
    BOMStorage *s = (BOMStorage *)calloc(1, sizeof(BOMStorage));
    return s;
}

BOMStorage *_bom_storage_open(const char *path) {
    BOMStorage *s;
    if (path == NULL)
        return NULL;
    s = storage_alloc();
    if (s == NULL)
        return NULL;
    s->path = strdup(path);
    if (s->path == NULL || bom_file_open(path, &s->bf) != 0) {
        free(s->path);
        free(s);
        return NULL;
    }
    s->open = 1;
    _bom_storage_load_vars(s);
    return s;
}

void _bom_storage_close(BOMStorage *s) {
    uint32_t i;
    if (s == NULL)
        return;
    if (s->open)
        bom_file_close(&s->bf);
    for (i = 0; i < s->nvar; i++)
        free(s->varname[i]);
    free(s->varname);
    free(s->varlen);
    free(s->varblk);
    free(s->path);
    free(s);
}

void _bom_storage_load_vars(BOMStorage *s) {
    uint32_t voff, vlen, count, i;
    const uint8_t *p;
    if (s == NULL || !s->open || s->bf.len < 32)
        return;
    voff = _bom_r32(s->bf.data + 24);
    vlen = _bom_r32(s->bf.data + 28);
    if (voff == 0 || voff > s->bf.len || vlen < 4 || vlen > s->bf.len - voff)
        return;
    count = _bom_r32(s->bf.data + voff);
    if (count == 0 || count > 0xf000)
        return;
    s->varname = (char **)calloc(count, sizeof(char *));
    s->varlen = (uint8_t *)malloc(count);
    s->varblk = (uint32_t *)calloc(count, sizeof(uint32_t));
    if (s->varname == NULL || s->varlen == NULL || s->varblk == NULL) {
        free(s->varname); s->varname = NULL;
        free(s->varlen);  s->varlen = NULL;
        free(s->varblk);  s->varblk = NULL;
        return;
    }
    p = s->bf.data + voff + 4;
    for (i = 0; i < count; i++) {
        uint8_t nl;
        if ((size_t)(p - (s->bf.data + voff)) + 5 > vlen)
            break;
        s->varblk[i] = _bom_r32(p);
        nl = p[4];
        if ((size_t)(p + 5 + nl) > (size_t)(s->bf.data + voff + vlen))
            break;
        s->varname[i] = (char *)malloc((size_t)nl + 1);
        if (s->varname[i] == NULL)
            break;
        memcpy(s->varname[i], p + 5, nl);
        s->varname[i][nl] = '\0';
        s->varlen[i] = nl;
        p += 5 + nl;
        s->nvar++;
    }
}

uint32_t _bom_storage_var_block(const BOMStorage *s, const char *name,
                                size_t namelen) {
    uint32_t i;
    if (s == NULL || name == NULL)
        return 0;
    for (i = 0; i < s->nvar; i++) {
        if (s->varlen[i] == namelen && memcmp(s->varname[i], name, namelen) == 0)
            return s->varblk[i];
    }
    return 0;
}

/* ---- public surface ---- */

int BOMStorageIsStorageFileWithSys(const char *path, BOMSys *sys) {
    BOMStorage *s;
    (void)sys;
    if (path == NULL)
        return 0;
    s = _bom_storage_open(path);
    if (s == NULL)
        return 0;
    _bom_storage_close(s);
    return 1;
}

void BOMStorageDump(BOMStorage *storage, uint32_t flags) {
    uint32_t i;
    (void)flags;
    if (storage == NULL || !storage->open)
        return;
    fprintf(stdout, "BOMStorage: %s\n", storage->path ? storage->path : "?");
    fprintf(stdout, "  version %u  blocks %u  image %u bytes\n",
            _bom_r32(storage->bf.data + 8), storage->bf.num_blocks,
            storage->bf.len);
    fprintf(stdout, "  %u variable(s)\n", storage->nvar);
    for (i = 0; i < storage->nvar; i++)
        fprintf(stdout, "    %s -> block %u\n", storage->varname[i],
                storage->varblk[i]);
}

size_t BOMStorageSizeOfBlock(BOMStorage *storage, uint32_t blockID) {
    uint32_t len;
    if (storage == NULL || !storage->open)
        return 0;
    if (bom_block(&storage->bf, blockID, &len) == NULL)
        return 0;
    return (size_t)len;
}

int BOMStorageCopyFromBlock(BOMStorage *storage, uint32_t blockID,
                            void *buffer) {
    const uint8_t *b;
    uint32_t len;
    if (storage == NULL || !storage->open || buffer == NULL)
        return -1;
    b = bom_block(&storage->bf, blockID, &len);
    if (b == NULL)
        return -1;
    memcpy(buffer, b, len);
    return 0;
}