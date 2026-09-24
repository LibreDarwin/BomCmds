/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * --bom filter: reads a Bill-Of-Materials file produced by mkbom and answers
 * path-inclusion queries during copy.  Walks the same Paths table that
 * lsbom renders (variable "Paths" -> PathInfoIndex -> File blocks). */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "ditto.h"
#include "bom_read.h"

struct prow {
    uint32_t pid, parent;
    char *name;
};

static uint32_t r32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static uint16_t r16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static int find_var_block(const bom_file *bf, const char *want) {
    uint32_t voff, vlen, count, i;
    const uint8_t *p;
    if (bf->len < 32)
        return 0;
    voff = r32le(bf->data + 24);
    vlen = r32le(bf->data + 28);
    if (voff == 0 || (size_t)voff > bf->len || vlen < 4 ||
        (size_t)vlen > bf->len - voff)
        return 0;
    count = r32le(bf->data + voff);
    p = bf->data + voff + 4;
    for (i = 0; i < count; i++) {
        uint32_t b;
        uint8_t nl;
        if ((size_t)(p - (bf->data + voff)) + 5 > vlen)
            return 0;
        b = r32le(p);
        nl = p[4];
        if (p + 5 + nl > bf->data + voff + vlen)
            return 0;
        if (nl == strlen(want) && memcmp(p + 5, want, nl) == 0)
            return b != 0 ? (int)b : 0;
        p += 5 + nl;
    }
    return 0;
}

static void walk_emit(struct ditto_bomf *bf, struct prow *rows, size_t n,
                      struct prow *cur, const char *prefix, size_t prefixlen) {
    size_t i;
    char *path;
    size_t plen;
    if (cur->pid == 1) {
        path = strdup(".");
        plen = 1;
    } else {
        plen = prefixlen + 1 + strlen(cur->name);
        path = malloc(plen + 1);
        if (path == NULL)
            return;
        snprintf(path, plen + 1, "%s/%s", prefix, cur->name);
    }
    bf->paths = realloc(bf->paths, (bf->npaths + 1) * sizeof(char *));
    if (bf->paths == NULL)
        bf->npaths = 0;
    bf->paths[bf->npaths++] = path;
    for (i = 0; i < n; i++)
        if (rows[i].parent == cur->pid)
            walk_emit(bf, rows, n, &rows[i], path, plen);
}

struct ditto_bomf *bomf_open(const char *path) {
    struct ditto_bomf *bf = calloc(1, sizeof *bf);
    bom_file bom;
    struct prow *rows = NULL;
    size_t n = 0, cap = 0;
    int tree_idx;
    const uint8_t *tree, *pb;
    uint32_t tree_len, pb_len;
    size_t qi;

    if (bf == NULL)
        return NULL;
    if (bom_file_open(path, &bom) != 0) {
        free(bf);
        return NULL;
    }

    tree_idx = find_var_block(&bom, "Paths");
    tree = tree_idx > 0 ? bom_block(&bom, (uint32_t)tree_idx, &tree_len) : NULL;
    if (tree != NULL && tree_len >= 12) {
        pb = bom_block(&bom, r32le(tree + 8), &pb_len);
        while (pb != NULL) {
            uint32_t count, next, i;
            int is_pi;
            if (pb_len < 12)
                break;
            is_pi = (int)r16le(pb);
            count = (uint32_t)r16le(pb + 2);
            next = r32le(pb + 4);
            if (is_pi == 1) {
                for (i = 0; i < count; i++) {
                    uint32_t piib, fblk, prl, fl;
                    const uint8_t *pii, *fb;
                    const char *name;
                    size_t nl;
                    struct prow *row;
                    if (12 + 8 * i + 8 > pb_len)
                        goto done;
                    pii = pb + 12 + 8 * i;
                    piib = r32le(pii);
                    fblk = r32le(pii + 4);
                    pii = bom_block(&bom, piib, &prl);
                    fb = bom_block(&bom, fblk, &fl);
                    if (pii == NULL || prl < 8 || fb == NULL || fl < 5)
                        continue;
                    name = (const char *)fb + 4;
                    nl = 0;
                    while (nl < fl - 4 && name[nl] != '\0')
                        nl++;
                    if (name[nl] != '\0')
                        continue;
                    if (n == cap) {
                        size_t ncap = cap ? cap * 2 : 256;
                        struct prow *nr = realloc(rows, ncap * sizeof *nr);
                        if (nr == NULL)
                            goto done;
                        rows = nr;
                        cap = ncap;
                    }
                    row = rows + n++;
                    row->pid = r32le(pii);
                    row->parent = r32le(fb);
                    row->name = strdup(name);
                }
            }
            if (next == 0)
                break;
            pb = bom_block(&bom, next, &pb_len);
        }
    }
done:
    (void)qi;
    for (qi = 0; qi < n; qi++) {
        if (rows[qi].pid == 1) {
            walk_emit(bf, rows, n, rows + qi, "", 0);
            break;
        }
    }
    for (qi = 0; qi < n; qi++)
        free(rows[qi].name);
    free(rows);
    bom_file_close(&bom);
    bf->ok = 1;
    return bf;
}

int bomf_contains(const struct ditto_bomf *bf, const char *rel) {
    size_t i;
    char full[4096];
    if (bf == NULL || !bf->ok)
        return 1; /* no filter: include everything */
    if (rel[0] == '\0')
        return 1; /* the root itself */
    if (rel[0] == '.') {
        snprintf(full, sizeof full, "%s", rel);
    } else {
        snprintf(full, sizeof full, "./%s", rel);
    }
    for (i = 0; i < bf->npaths; i++)
        if (strcmp(bf->paths[i], full) == 0)
            return 1;
    return 0;
}

void bomf_close(struct ditto_bomf *bf) {
    size_t i;
    if (bf == NULL)
        return;
    for (i = 0; i < bf->npaths; i++)
        free(bf->paths[i]);
    free(bf->paths);
    free(bf);
}

