/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * Clean-room BOMTree (FORMAT.md 4.3/4.4): a BOMStore variable whose Tree
 * root block points (via block_paths_index) at a chain of Paths blocks.
 *
 * The main "Paths" tree stores one entry per path: each PathsEntry pair is
 * (PathInfoIndex block, File block), sorted by (parent path id, leaf name
 * bytes).  BOMTreeIterator keys are the reconstructed "./..." paths; values
 * point at the 8-byte PathInfoIndex record. */
#include "bom_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TREE_MAGIC "tree"

/* ---- row decode ---- */

static uint32_t r16be(const uint8_t *p) {
    return (uint32_t)((uint16_t)((p[0] << 8) | p[1]));
}

static int tree_decode_row(BOMTree *t, uint32_t pii_blk, uint32_t file_blk,
                           struct bom_tree_row *out) {
    const uint8_t *pii, *fb, *prb;
    uint32_t pii_len, fb_len, pr_len, nl, pid, parent, prblk;
    size_t i;
    (void)i;
    memset(out, 0, sizeof *out);
    pii = bom_block(&t->storage->bf, pii_blk, &pii_len);
    fb = bom_block(&t->storage->bf, file_blk, &fb_len);
    if (pii == NULL || pii_len < 8 || fb == NULL || fb_len < 5)
        return -1;
    pid = _bom_r32(pii);
    parent = _bom_r32(fb);
    for (nl = 0; nl + 4 < fb_len && fb[4 + nl] != '\0'; nl++)
        ;
    if (nl == 0 || fb[4 + nl] != '\0')
        return -1;
    prblk = _bom_r32(pii + 4);
    prb = bom_block(&t->storage->bf, prblk, &pr_len);
    if (prb == NULL || bom_pathrec_decode(prb, pr_len, &out->pr) != 0)
        return -1;
    out->pid = pid;
    out->parent = parent;
    out->prblk = prblk;
    memcpy(out->pii, pii, 8);
    out->leaf = (char *)malloc(nl + 1);
    if (out->leaf == NULL)
        return -1;
    memcpy(out->leaf, fb + 4, nl);
    out->leaf[nl] = '\0';
    out->name_len = (uint16_t)nl;
    return 0;
}

/* ---- path reconstruction ---- */

/* Find a row by path id. */
static struct bom_tree_row *find_pid(BOMTree *t, uint32_t pid) {
    uint32_t i;
    for (i = 0; i < t->nrows; i++)
        if (t->rows[i].pid == pid)
            return &t->rows[i];
    return NULL;
}

/* Rebuild the full "./a/b/c" path for row `r` by walking the parent chain
 * up to the root (pid 1).  Returns 0 and sets *out on success. */
static int row_path(BOMTree *t, struct bom_tree_row *r, char **out) {
    static uint32_t chain[16384];
    uint32_t nchain = 0, pid = r->pid;
    size_t total, off;
    char *path;

    if (pid == 1) {
        path = (char *)malloc(2);
        if (path == NULL)
            return -1;
        path[0] = '.';
        path[1] = '\0';
        *out = path;
        return 0;
    }
    while (pid != 1) {
        struct bom_tree_row *p = find_pid(t, pid);
        if (p == NULL)
            return -1;
        if (nchain >= 16384)
            return -1;
        chain[nchain++] = pid;
        pid = p->parent;
        if (pid == 0) /* detached node / bad parent chain */
            return -1;
    }
    total = 1; /* "." */
    {
        uint32_t i;
        for (i = 0; i < nchain; i++) {
            struct bom_tree_row *p = find_pid(t, chain[nchain - 1 - i]);
            if (p == NULL || p->name_len == 0)
                return -1;
            total += 1 + p->name_len;
        }
    }
    path = (char *)malloc(total + 1);
    if (path == NULL)
        return -1;
    off = 0;
    path[off++] = '.';
    {
        uint32_t i;
        for (i = 0; i < nchain; i++) {
            struct bom_tree_row *p = find_pid(t, chain[nchain - 1 - i]);
            path[off++] = '/';
            memcpy(path + off, p->leaf, p->name_len);
            off += p->name_len;
        }
    }
    path[off] = '\0';
    *out = path;
    return 0;
}

/* ---- public surface ---- */

BOMStorage *BOMTreeStorage(BOMTree *tree) {
    if (tree == NULL)
        return NULL;
    return tree->storage;
}

BOMTree *BOMTreeOpenWithName(BOMStorage *storage, const char *name,
                             uint32_t flags) {
    BOMTree *t;
    const uint8_t *root, *pb;
    uint32_t root_len, idx, pb_len;
    const char *varname;
    uint32_t nalloc = 0;
    (void)flags;

    if (storage == NULL || !storage->open)
        return NULL;
    varname = (name != NULL && name[0] != '\0') ? name : "Paths";
    idx = _bom_storage_var_block(storage, varname, strlen(varname));
    if (idx == 0)
        return NULL;
    root = bom_block(&storage->bf, idx, &root_len);
    if (root == NULL || root_len < 21 || memcmp(root, TREE_MAGIC, 4) != 0)
        return NULL;

    t = (BOMTree *)calloc(1, sizeof(BOMTree));
    if (t == NULL)
        return NULL;
    t->storage = storage;
    t->name = strdup(varname);
    if (t->name == NULL) {
        free(t);
        return NULL;
    }
    t->root = idx;
    t->bpi = _bom_r32(root + 8);
    t->bsize = _bom_r32(root + 12);
    t->count = _bom_r32(root + 16);
    if (t->count > 0) {
        t->rows = (struct bom_tree_row *)calloc(t->count, sizeof *t->rows);
        if (t->rows == NULL) {
            free(t->name);
            free(t);
            return NULL;
        }
        nalloc = t->count;
    }

    /* Walk the Paths chain via next_paths_block_index. */
    pb = bom_block(&storage->bf, t->bpi, &pb_len);
    while (pb != NULL && pb_len >= 12) {
        uint32_t is_pi, count, next, i;
        is_pi = r16be(pb);
        count = r16be(pb + 2);
        next = _bom_r32(pb + 4);
        if (is_pi != 1)
            break;
        for (i = 0; i < count && t->nrows < nalloc; i++) {
            uint32_t pii_blk, file_blk;
            struct bom_tree_row *r;
            if (12 + 8 * i + 8 > pb_len)
                break;
            pii_blk = _bom_r32(pb + 12 + 8 * i);
            file_blk = _bom_r32(pb + 12 + 8 * i + 4);
            r = &t->rows[t->nrows];
            if (tree_decode_row(t, pii_blk, file_blk, r) != 0)
                continue;
            t->nrows++;
        }
        if (next == 0)
            break;
        pb = bom_block(&storage->bf, next, &pb_len);
    }

    /* Reconstruct full paths.  Rows are stored in (parent, name) order, so
     * a parent always precedes its children; but path reconstruction needs
     * the whole set, run after the pass above. */
    {
        uint32_t i;
        for (i = 0; i < t->nrows; i++) {
            if (row_path(t, &t->rows[i], &t->rows[i].path) != 0) {
                t->rows[i].path = strdup(".");
                if (t->rows[i].path == NULL) {
                    BOMTreeFree(t);
                    return NULL;
                }
            }
        }
    }
    return t;
}

uint32_t BOMTreeCount(BOMTree *tree) {
    if (tree == NULL)
        return 0;
    return tree->nrows;
}

BOMTreeIterator *BOMTreeIteratorNew(BOMTree *tree, const void *startKey,
                                    const void *stopKey, uint32_t flags) {
    BOMTreeIterator *it;
    (void)flags;
    (void)stopKey;
    if (tree == NULL)
        return NULL;
    it = (BOMTreeIterator *)calloc(1, sizeof(BOMTreeIterator));
    if (it == NULL)
        return NULL;
    it->tree = tree;
    if (startKey != NULL) {
        while (it->i < tree->nrows &&
               strcmp(tree->rows[it->i].path ? tree->rows[it->i].path : ".",
                      (const char *)startKey) < 0)
            it->i++;
    }
    return it;
}

int BOMTreeIteratorIsAtEnd(BOMTreeIterator *iter) {
    if (iter == NULL || iter->tree == NULL)
        return 1;
    return iter->i >= iter->tree->nrows;
}

void *BOMTreeIteratorKey(BOMTreeIterator *iter) {
    if (iter == NULL || iter->tree == NULL || iter->i >= iter->tree->nrows)
        return NULL;
    return iter->tree->rows[iter->i].path;
}

void *BOMTreeIteratorValue(BOMTreeIterator *iter) {
    if (iter == NULL || iter->tree == NULL || iter->i >= iter->tree->nrows)
        return NULL;
    return iter->tree->rows[iter->i].pii;
}

void BOMTreeIteratorNext(BOMTreeIterator *iter) {
    if (iter == NULL || iter->tree == NULL)
        return;
    iter->i++;
}

void BOMTreeIteratorFree(BOMTreeIterator *iter) {
    free(iter);
}

void BOMTreeFree(BOMTree *tree) {
    uint32_t i;
    if (tree == NULL)
        return;
    if (tree->rows != NULL) {
        for (i = 0; i < tree->nrows; i++) {
            free(tree->rows[i].path);
            free(tree->rows[i].leaf);
        }
    }
    free(tree->rows);
    free(tree->name);
    free(tree);
}