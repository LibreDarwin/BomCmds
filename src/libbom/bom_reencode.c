/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * Clean-room records-based BOM re-encoder (see bom_reencode.h).
 *
 * Rebuilds a BOMStore from already-decoded tree rows: no filesystem access.
 * The emitted file matches the ngroups=0 layout of bm_write_bom (fixed
 * blocks 1-10, one (PathRecord, File, PathInfoIndex) triplet per row from
 * block 11, no hard-link group trailers).  Rows carry the raw PathRecord
 * bytes to emit (built by the caller, e.g. via bom_pr_rebuild), so they are
 * reproduced verbatim; block 1 BomInfo is recomputed from the decoded
 * records' own slice tables.  Rows must be supplied parent-before-child,
 * which the source tree's (parent,name)-sorted storage order satisfies. */
#include "bom_reencode.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BM_POOL 2730
#define BM_FIXED_START 0x200
#define BM_TREE_PAD 0x1000

/* ---- endian helpers ---- */

static void w16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void w32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t r32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ---- blob store (mirrors bom_writer.c) ---- */

typedef struct blob_store {
    uint8_t  **data;
    size_t    *len;
    uint8_t   *present;
    int        nob;
} blob_store;

static int blobs_init(blob_store *bs, int nob) {
    memset(bs, 0, sizeof(*bs));
    bs->data = (uint8_t **)calloc((size_t)nob + 1, sizeof(uint8_t *));
    bs->len = (size_t *)calloc((size_t)nob + 1, sizeof(size_t));
    bs->present = (uint8_t *)calloc((size_t)nob + 1, sizeof(uint8_t));
    if (bs->data == NULL || bs->len == NULL || bs->present == NULL) {
        free(bs->data);
        free(bs->len);
        free(bs->present);
        return -1;
    }
    bs->nob = nob;
    return 0;
}

static void blobs_free(blob_store *bs) {
    int i;
    if (bs->data == NULL)
        return;
    for (i = 1; i <= bs->nob; i++)
        free(bs->data[i]);
    free(bs->data);
    free(bs->len);
    free(bs->present);
    memset(bs, 0, sizeof(*bs));
}

static int put(blob_store *bs, int idx, const void *data, size_t len) {
    uint8_t *copy;
    if (idx < 1 || idx > bs->nob)
        return -1;
    copy = (uint8_t *)malloc(len ? len : 1);
    if (copy == NULL)
        return -1;
    if (len)
        memcpy(copy, data, len);
    free(bs->data[idx]);
    bs->data[idx] = copy;
    bs->len[idx] = len;
    bs->present[idx] = 1;
    return 0;
}

/* ---- PathRecord rebuild (mirrors bom_writer.c build_pr) ---- */

int bom_pr_rebuild(const bom_pathrec *pr, uint8_t **out, size_t *outlen) {
    uint8_t base[31];
    uint8_t *rec;
    size_t reclen;
    size_t i;

    if (pr == NULL || out == NULL || outlen == NULL)
        return -1;
    memset(base, 0, sizeof(base));
    base[0] = pr->path_type;
    base[1] = 1;
    w16(base + 2, pr->architecture);
    w16(base + 4, pr->mode);
    w32(base + 6, pr->uid);
    w32(base + 10, pr->gid);
    w32(base + 14, pr->mtime);
    w32(base + 18, pr->size);
    base[22] = 1;
    w32(base + 23, pr->checksum);

    if (pr->nslice > 0) { /* Mach-O: arch table variant */
        /* Each slice entry is the 16-byte {cputype, subtype, size, cksum}
         * the writer stored; copy verbatim.  The record ends with an 8-byte
         * tail holding link name length + (rarely) the start of the link. */
        uint32_t tail = 32 + 16 * (uint32_t)pr->nslice;
        reclen = (size_t)tail + 8;
        *out = (uint8_t *)calloc(reclen, 1);
        if (*out == NULL)
            return -1;
        rec = *out;
        memcpy(rec, base, 27); /* bytes 0..26 */
        rec[27] = 1;           /* arch table flag */
        w32(rec + 28, pr->nslice);
        for (i = 0; i < (size_t)pr->nslice; i++)
            memcpy(rec + 32 + 16 * i, pr->slices + 16 * i, 16);
        if (pr->link_len > 0 && pr->link != NULL &&
            pr->link_len <= 4) {
            w32(rec + tail, pr->link_len);
            memcpy(rec + tail + 4, pr->link, pr->link_len);
        }
        *outlen = reclen;
        return 0;
    }

    if (pr->path_type == BM_PT_LINK) {
        uint32_t lnklen = pr->link_len ? pr->link_len : 1;
        reclen = 31 + (size_t)lnklen + 8; /* lnklen + NUL + 8 pad */
        *out = (uint8_t *)calloc(reclen, 1);
        if (*out == NULL)
            return -1;
        rec = *out;
        w32(base + 27, lnklen);
        memcpy(rec, base, 31);
        if (pr->link != NULL && pr->link_len > 0)
            memcpy(rec + 31, pr->link, pr->link_len);
        *outlen = reclen;
        return 0;
    }

    if (pr->path_type == BM_PT_DIR) { /* 31 bytes, no tail */
        *out = (uint8_t *)malloc(31);
        if (*out == NULL)
            return -1;
        memcpy(*out, base, 31);
        *outlen = 31;
        return 0;
    }

    /* regular file (non-Mach-O): 31 + 4 pad = 35 bytes */
    reclen = 31 + 4;
    *out = (uint8_t *)calloc(reclen, 1);
    if (*out == NULL)
        return -1;
    rec = *out;
    memcpy(rec, base, 31);
    *outlen = reclen;
    return 0;
}

/* ---- pid renumbering ---- */

static int renumber(const bom_reenc_row *rows, size_t nrows, uint32_t **map,
                    uint32_t **parents) {
    /* new pid for row i is i+1; build old-pid -> new-pid lookup. */
    uint32_t maxpid = 0;
    size_t i;
    uint32_t *np;
    uint32_t *pp;

    for (i = 0; i < nrows; i++)
        if (rows[i].pid > maxpid)
            maxpid = rows[i].pid;
    np = (uint32_t *)calloc((size_t)maxpid + 1, sizeof(uint32_t));
    pp = (uint32_t *)malloc(nrows * sizeof(uint32_t));
    if (np == NULL || pp == NULL) {
        free(np);
        free(pp);
        return -1;
    }
    for (i = 0; i < nrows; i++)
        np[rows[i].pid] = (uint32_t)i + 1; /* row 0 must be the root */
    for (i = 0; i < nrows; i++) {
        uint32_t p = rows[i].parent;
        if (p == 0) {
            if (rows[i].pid != 1) {
                free(pp);
                free(np);
                errno = EINVAL;
                return -1;
            }
            pp[i] = 0;
        } else {
            if (p > maxpid || np[p] == 0) {
                free(pp);
                free(np);
                errno = EINVAL;
                return -1;
            }
            pp[i] = np[p];
        }
    }
    *parents = pp;
    *map = np;
    return 0;
}

/* ---- BomInfo from records ---- */

typedef struct bominfo {
    uint32_t plain;
    uint32_t arcp[64];
    uint64_t arsz[64];
    size_t   narch;
    int      has_file;
} bominfo;

static void bominfo_from_rows(const bom_reenc_row *rows, size_t nrows,
                              bominfo *bi) {
    size_t i;
    memset(bi, 0, sizeof(*bi));
    for (i = 0; i < nrows; i++) {
        bom_pathrec pr;
        size_t j, a;
        if (bom_pathrec_decode(rows[i].pr, (uint32_t)rows[i].prlen, &pr) != 0 ||
            !pr.valid)
            continue;
        if (pr.path_type != BM_PT_DIR)
            bi->has_file = 1;
        if (pr.path_type == BM_PT_DIR)
            continue;
        if (pr.nslice > 0) { /* Mach-O: tally per-slice cputype sizes */
            for (j = 0; j < (size_t)pr.nslice; j++) {
                const uint8_t *s = pr.slices + 16 * j;
                uint32_t cp = r32(s);
                uint32_t sz = r32(s + 8);
                for (a = 0; a < bi->narch; a++)
                    if (bi->arcp[a] == cp)
                        break;
                if (a == bi->narch) {
                    if (bi->narch >= 64)
                        continue;
                    bi->arcp[bi->narch] = cp;
                    bi->arsz[bi->narch] = 0;
                    a = bi->narch;
                    bi->narch++;
                }
                bi->arsz[a] += (uint64_t)sz;
            }
        } else {
            bi->plain += (uint64_t)pr.size;
        }
    }
}

/* ---- file assembly ---- */

static int write_bom_file(const char *out_path, const blob_store *bs, int nob,
                          char *errbuf, size_t errbufsz);

/* ---- main entry ---- */

int bom_reencode(const bom_reenc_row *rows, size_t nrows,
                 const char *out_path, char *errbuf, size_t errbufsz) {
    blob_store bs;
    size_t ncount = nrows;
    size_t i;
    uint32_t *new_of_old = NULL;
    uint32_t *new_parent = NULL;
    int *pridx = NULL, *fidx = NULL, *piidx = NULL;
    int nob = 0, idx, rc = -1;
    bominfo bi;

    if (errbuf && errbufsz)
        errbuf[0] = '\0';

    if (rows == NULL || out_path == NULL || nrows == 0 ||
        rows[0].pid != 1 || rows[0].parent != 0) {
        if (errbuf && errbufsz)
            snprintf(errbuf, errbufsz, "invalid row set");
        return -1;
    }
    /* Single Paths block: 12-byte header + 8 bytes per entry must fit. */
    if (nrows > 500) {
        if (errbuf && errbufsz)
            snprintf(errbuf, errbufsz,
                     "too many rows for a single Paths block");
        return -1;
    }

    if (renumber(rows, nrows, &new_of_old, &new_parent) != 0) {
        if (errbuf && errbufsz)
            snprintf(errbuf, errbufsz, "invalid pid/parent numbering");
        return -1;
    }
    (void)new_of_old;

    bominfo_from_rows(rows, nrows, &bi);
    {
        int ninfo = bi.has_file ? (int)(1 + bi.narch) : 0;
        uint8_t b1head[12];
        w32(b1head, 1);
        w32(b1head + 4, (uint32_t)(nrows + 1));
        w32(b1head + 8, (uint32_t)ninfo);

        /* block-index simulation: one triplet per row, from block 11 */
        pridx = (int *)malloc((nrows + 1) * sizeof(int));
        fidx = (int *)malloc((nrows + 1) * sizeof(int));
        piidx = (int *)malloc((nrows + 1) * sizeof(int));
        if (pridx == NULL || fidx == NULL || piidx == NULL) {
            if (errbuf && errbufsz)
                snprintf(errbuf, errbufsz, "out of memory");
            goto out_early;
        }
        idx = 11;
        for (i = 0; i < ncount; i++) {
            uint32_t pid = (uint32_t)i + 1;
            pridx[pid] = idx;
            fidx[pid] = idx + 1;
            piidx[pid] = idx + 2;
            idx += 3;
        }
        nob = idx - 1;
        if (blobs_init(&bs, nob) != 0) {
            if (errbuf && errbufsz)
                snprintf(errbuf, errbufsz, "out of memory");
            goto out_early;
        }

        /* ---- content emission (mirrors bm_write_bom) ---- */
        for (i = 0; i < ncount; i++) {
            uint32_t pid = (uint32_t)i + 1;
            size_t nl = strlen(rows[i].name);
            uint8_t *f = (uint8_t *)malloc(4 + nl + 1);
            uint8_t pii[8];
            if (f == NULL) {
                if (errbuf && errbufsz)
                    snprintf(errbuf, errbufsz, "out of memory");
                goto out;
            }
            w32(f, new_parent[i]);
            memcpy(f + 4, rows[i].name, nl + 1);
            if (put(&bs, fidx[pid], f, 4 + nl + 1) != 0) {
                free(f);
                goto out;
            }
            free(f);
            w32(pii, pid);
            w32(pii + 4, 0);
            if (put(&bs, piidx[pid], pii, 8) != 0)
                goto out;
            if (put(&bs, pridx[pid], rows[i].pr, rows[i].prlen) != 0)
                goto out;
        }
        /* second pass: patch each PII with its own PR index (no groups) */
        for (i = 0; i < ncount; i++) {
            uint32_t pid = (uint32_t)i + 1;
            uint8_t pii[8];
            w32(pii, pid);
            w32(pii + 4, (uint32_t)pridx[pid]);
            if (put(&bs, piidx[pid], pii, 8) != 0)
                goto out;
        }

        /* block 1: header + entry0 + per-cputype size entries */
        if (ninfo) {
            size_t blen = 12 + 16 * (size_t)ninfo;
            uint8_t *b1 = (uint8_t *)calloc(blen, 1);
            size_t a;
            if (b1 == NULL) {
                if (errbuf && errbufsz)
                    snprintf(errbuf, errbufsz, "out of memory");
                goto out;
            }
            memcpy(b1, b1head, 12);
            w32(b1 + 12, 0);      /* entry0: cpu 0 */
            w32(b1 + 16, 0);      /* entry0: subtype 0 */
            w32(b1 + 20, (uint32_t)bi.plain);
            w32(b1 + 24, 0);
            for (a = 0; a < bi.narch; a++) {
                w32(b1 + 28 + 16 * a, bi.arcp[a]);
                w32(b1 + 32 + 16 * a, 0);
                w32(b1 + 36 + 16 * a, (uint32_t)bi.arsz[a]);
                w32(b1 + 40 + 16 * a, 0);
            }
            if (put(&bs, 1, b1, blen) != 0) {
                free(b1);
                goto out;
            }
            free(b1);
        } else {
            if (put(&bs, 1, b1head, sizeof(b1head)) != 0)
                goto out;
        }

        /* blocks 2,3: Paths tree header + main Paths {@pii,@file} */
        {
            uint8_t b2[21];
            memset(b2, 0, sizeof(b2));
            memcpy(b2, "tree", 4);
            w32(b2 + 4, 1);
            w32(b2 + 8, 3);
            w32(b2 + 12, BM_TREE_PAD);
            w32(b2 + 16, (uint32_t)ncount);
            if (put(&bs, 2, b2, sizeof(b2)) != 0)
                goto out;
        }
        {
            uint8_t *b3 = (uint8_t *)calloc(BM_TREE_PAD, 1);
            if (b3 == NULL) {
                if (errbuf && errbufsz)
                    snprintf(errbuf, errbufsz, "out of memory");
                goto out;
            }
            w16(b3, 1);
            w16(b3 + 2, (uint16_t)ncount);
            for (i = 0; i < ncount; i++) {
                uint32_t pid = (uint32_t)i + 1;
                w32(b3 + 12 + 8 * i, (uint32_t)piidx[pid]);
                w32(b3 + 16 + 8 * i, (uint32_t)fidx[pid]);
            }
            if (put(&bs, 3, b3, BM_TREE_PAD) != 0) {
                free(b3);
                goto out;
            }
            free(b3);
        }

        /* blocks 4,5: HLIndex (empty: no hard-link groups) */
        {
            uint8_t b4[21];
            memset(b4, 0, sizeof(b4));
            memcpy(b4, "tree", 4);
            w32(b4 + 4, 1);
            w32(b4 + 8, 5);
            w32(b4 + 12, BM_TREE_PAD);
            w32(b4 + 16, 0);
            if (put(&bs, 4, b4, sizeof(b4)) != 0)
                goto out;
        }
        {
            uint8_t *b5 = (uint8_t *)calloc(BM_TREE_PAD, 1);
            if (b5 == NULL) {
                if (errbuf && errbufsz)
                    snprintf(errbuf, errbufsz, "out of memory");
                goto out;
            }
            w16(b5, 1);
            w16(b5 + 2, 0);
            if (put(&bs, 5, b5, BM_TREE_PAD) != 0) {
                free(b5);
                goto out;
            }
            free(b5);
        }

        /* blocks 6..10: VIndex + Size64 (both empty) */
        {
            uint8_t b6[13];
            memset(b6, 0, sizeof(b6));
            w32(b6, 1);
            w32(b6 + 4, 7);
            w32(b6 + 8, 0);
            if (put(&bs, 6, b6, sizeof(b6)) != 0)
                goto out;
        }
        {
            uint8_t b7[21];
            memset(b7, 0, sizeof(b7));
            memcpy(b7, "tree", 4);
            w32(b7 + 4, 1);
            w32(b7 + 8, 8);
            w32(b7 + 12, 0x80);
            w32(b7 + 16, 0);
            if (put(&bs, 7, b7, sizeof(b7)) != 0)
                goto out;
        }
        {
            uint8_t b8[0x80];
            memset(b8, 0, sizeof(b8));
            w16(b8, 1);
            w16(b8 + 2, 0);
            if (put(&bs, 8, b8, sizeof(b8)) != 0)
                goto out;
        }
        {
            uint8_t b9[21];
            memset(b9, 0, sizeof(b9));
            memcpy(b9, "tree", 4);
            w32(b9 + 4, 1);
            w32(b9 + 8, 10);
            w32(b9 + 12, BM_TREE_PAD);
            w32(b9 + 16, 0);
            if (put(&bs, 9, b9, sizeof(b9)) != 0)
                goto out;
        }
        {
            uint8_t *b10 = (uint8_t *)calloc(BM_TREE_PAD, 1);
            if (b10 == NULL) {
                if (errbuf && errbufsz)
                    snprintf(errbuf, errbufsz, "out of memory");
                goto out;
            }
            w16(b10, 1);
            w16(b10 + 2, 0);
            if (put(&bs, 10, b10, BM_TREE_PAD) != 0) {
                free(b10);
                goto out;
            }
            free(b10);
        }

        /* ---- assemble file ---- */
        if (write_bom_file(out_path, &bs, nob, errbuf, errbufsz) != 0)
            goto out;
        rc = 0;
    }

out:
    blobs_free(&bs);
out_early:
    free(new_of_old);
    free(new_parent);
    free(pridx);
    free(fidx);
    free(piidx);
    return rc;
}

static int write_bom_file(const char *out_path, const blob_store *bs, int nob,
                          char *errbuf, size_t errbufsz) {
    uint64_t off = BM_FIXED_START;
    uint64_t *offsets;
    uint64_t *lens;
    uint8_t *index, *vars, *header;
    FILE *fp = NULL;
    int i;
    size_t indexlen = 4 + (size_t)BM_POOL * 8 + 4;
    size_t varslen = 4 + 12 + 10 + 12 + 11 + 11;
    uint8_t pad[BM_FIXED_START - 32];
    int rc = -1;

    offsets = (uint64_t *)calloc((size_t)nob + 1, sizeof(uint64_t));
    lens = (uint64_t *)calloc((size_t)nob + 1, sizeof(uint64_t));
    index = (uint8_t *)calloc(indexlen, 1);
    vars = (uint8_t *)calloc(varslen, 1);
    header = (uint8_t *)calloc(32, 1);
    if (offsets == NULL || lens == NULL || index == NULL || vars == NULL ||
        header == NULL) {
        if (errbuf && errbufsz)
            snprintf(errbuf, errbufsz, "out of memory");
        goto done;
    }

    for (i = 1; i <= nob; i++)
        if (bs->present[i]) {
            offsets[i] = off;
            lens[i] = (uint64_t)bs->len[i];
            off += bs->len[i];
        }

    /* index: BM_POOL free-list slots, all present, none free */
    w32(index, (uint32_t)BM_POOL);
    for (i = 1; i <= nob; i++) {
        w32(index + 4 + 8 * i, (uint32_t)(offsets[i] & 0xffffffffu));
        w32(index + 8 + 8 * i, (uint32_t)(lens[i] & 0xffffffffu));
    }

    /* vars: 5 variables, same layout as bm_write_bom */
    w32(vars, 5);
    {
        static const struct {
            const char *name;
            uint32_t blk;
        } vlist[5] = {{"BomInfo", 1}, {"Paths", 2}, {"HLIndex", 4},
                      {"VIndex", 6}, {"Size64", 9}};
        size_t o = 4;
        int v;
        for (v = 0; v < 5; v++) {
            size_t nl = strlen(vlist[v].name);
            w32(vars + o, vlist[v].blk);
            vars[o + 4] = (uint8_t)nl;
            memcpy(vars + o + 5, vlist[v].name, nl);
            o += 5 + nl;
        }
    }

    memcpy(header, "BOMStore", 8);
    w32(header + 8, 1);
    w32(header + 12, (uint32_t)nob);
    w32(header + 16, (uint32_t)(off & 0xffffffffu));
    w32(header + 20, (uint32_t)indexlen);
    w32(header + 24, (uint32_t)((off + indexlen) & 0xffffffffu));
    w32(header + 28, (uint32_t)varslen);

    fp = fopen(out_path, "wb");
    if (fp == NULL) {
        if (errbuf && errbufsz)
            snprintf(errbuf, errbufsz, "%s", strerror(errno));
        goto done;
    }
    if (fwrite(header, 1, 32, fp) != 32 || fwrite(pad, 1, sizeof(pad), fp) != sizeof(pad))
        goto werr;
    for (i = 1; i <= nob; i++)
        if (bs->present[i] && bs->len[i] &&
            fwrite(bs->data[i], 1, bs->len[i], fp) != bs->len[i])
            goto werr;
    if (fwrite(index, 1, indexlen, fp) != indexlen ||
        fwrite(vars, 1, varslen, fp) != varslen)
        goto werr;
    rc = 0;
    if (fclose(fp) != 0) {
        rc = -1;
        fp = NULL;
        if (errbuf && errbufsz)
            snprintf(errbuf, errbufsz, "%s", strerror(errno));
    }
    goto done;
werr:
    if (errbuf && errbufsz)
        snprintf(errbuf, errbufsz, "write failed");
done:
    if (fp != NULL)
        fclose(fp);
    free(offsets);
    free(lens);
    free(index);
    free(vars);
    free(header);
    return rc;
}