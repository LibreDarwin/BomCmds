/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * Clean-room dir-mode BOM writer (see bom_writer.h). */
#include "bom_writer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bom_cksum.h"

#define BM_POOL 2730
#define BM_FIXED_START 0x200
#define BM_TREE_PAD 0x1000
#define BM_TRAILER_PAD 64

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

#define BM_MAXSLICE 64

/* Per-arch slice table entry for a Mach-O file (matches Apple's mkbom). */
struct bm_mslice {
    uint32_t cputype;
    uint32_t subtype;
    uint64_t off;   /* byte offset of the slice in the file */
    uint32_t len;   /* slice size */
    uint32_t cksum;
};

/* Probe an open regular file for a Mach-O image.  Mirrors what Apple's
 * mkbom recognizes: the four thin-Mach-O magics (either byte order) and a
 * big-endian 32-bit universal/fat header (0xcafebabe).  Fat64 and swapped-
 * endian fat headers are NOT treated as Mach-O by Apple's writer.
 *
 * 0  -> not a recognized Mach-O
 * 1, *fat=0 -> thin Mach-O (single slice; sl[0] sized to the whole file)
 * >0, *fat=1 -> fat Mach-O (per-slice entries, sizes from the fat header)
 */
static int macho_slices(const uint8_t *hdr, size_t hlen, uint64_t fsize,
                        struct bm_mslice *sl, int maxsl, int *is_fat) {
    uint32_t magic;
    if (hlen < 4)
        return 0;
    *is_fat = 0;
    magic = (((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
             ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3]);
    switch (magic) {
    case 0xcefaedfe: /* MH_MAGIC (32-bit, little endian on disk) */
    case 0xcffaedfe: /* MH_MAGIC_64 (64-bit, little endian on disk) */
    case 0xfeedface: /* MH_CIGAM (32-bit, big endian on disk) */
    case 0xfeedfacf: /* MH_CIGAM_64 (64-bit, big endian on disk) */
        if (hlen < 12 || maxsl < 1)
            return 0;
        if (magic == 0xcefaedfe || magic == 0xcffaedfe) {
            sl[0].cputype = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
                            ((uint32_t)hdr[6] << 16) |
                            ((uint32_t)hdr[7] << 24);
            sl[0].subtype = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) |
                            ((uint32_t)hdr[10] << 16) |
                            ((uint32_t)hdr[11] << 24);
        } else {
            sl[0].cputype = r32(hdr + 4);
            sl[0].subtype = r32(hdr + 8);
        }
        sl[0].off = 0;
        sl[0].len = (uint32_t)fsize;
        sl[0].cksum = 0;
        return 1;
    case 0xcafebabe: /* FAT_MAGIC */
        if (hlen < 8 || maxsl < 1)
            return 0;
        {
            uint32_t n = r32(hdr + 4);
            size_t need;
            uint32_t i;
            if (n == 0 || n > (uint32_t)maxsl)
                return 0;
            need = 8 + 20 * (size_t)n;
            if (need > hlen)
                return 0;
            *is_fat = 1;
            for (i = 0; i < n; i++) {
                const uint8_t *fe = hdr + 8 + 20 * i;
                uint64_t off = r32(fe + 8);
                uint32_t len = r32(fe + 12);
                if (off > fsize || len > fsize - off)
                    return 0; /* malformed: slices must lie inside the file */
                sl[i].cputype = r32(fe + 0);
                sl[i].subtype = r32(fe + 4);
                sl[i].off = off;
                sl[i].len = len;
                sl[i].cksum = 0;
            }
            return (int)n;
        }
    default:
        return 0;
    }
}

/* ---- blob store: blocks[nob+1], 1-based index ---- */

typedef struct blob_store {
    uint8_t **data;     /* present block contents (may be 0-length) */
    size_t   *len;
    uint8_t  *present;  /* 1 if block was assigned */
    int       nob;
} blob_store;

static int blobs_init(blob_store *bs, int nob) {
    bs->data = (uint8_t **)calloc((size_t)nob + 1, sizeof(uint8_t *));
    bs->len = (size_t *)calloc((size_t)nob + 1, sizeof(size_t));
    bs->present = (uint8_t *)calloc((size_t)nob + 1, sizeof(uint8_t));
    bs->nob = nob;
    return (bs->data && bs->len && bs->present) ? 0 : -1;
}

static void blobs_free(blob_store *bs) {
    int i;
    for (i = 1; i <= bs->nob; i++)
        free(bs->data[i]);
    free(bs->data);
    free(bs->len);
    free(bs->present);
}

static int put(blob_store *bs, int idx, const void *data, size_t len) {
    if (idx < 1 || idx > bs->nob)
        return -1;
    uint8_t *copy = (uint8_t *)malloc(len ? len : 1);
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

/* ---- per-type PathRecord ---- */

static int build_pr(const bm_walk *w, uint32_t pid, uint8_t **out,
                    size_t *outlen) {
    const bm_path *n = &w->paths[pid - 1];
    uint8_t type;
    uint32_t ck = 0, lnklen = 0;
    uint8_t *tail = NULL;
    size_t taillen = 0;

    if (n->type == BM_TYPE_REG) {
        type = 1;
        FILE *f = fopen(n->path, "rb");
        if (f == NULL)
            return -1;
        bm_cksum_ctx ctx;
        bm_cksum_init(&ctx);
        uint8_t hdr[4096];
        size_t hlen = fread(hdr, 1, sizeof(hdr), f);
        if (hlen != 0)
            bm_cksum_feed(&ctx, hdr, hlen);
        uint8_t buf[65536];
        size_t total = hlen, got;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
            bm_cksum_feed(&ctx, buf, got);
            total += got;
        }
        int err = ferror(f);
        fclose(f);
        if (err)
            return -1;
        ck = bm_cksum_finish(&ctx, total);

        struct bm_mslice slvec[BM_MAXSLICE];
        int is_fat = 0;
        int nsl = macho_slices(hdr, hlen, (uint64_t)n->st.st_size, slvec,
                               BM_MAXSLICE, &is_fat);

        if (nsl > 0) { /* Mach-O: arch table appended to the record */
            size_t i, pos;
            if (is_fat) {
                /* per-slice checksum over each slice's own bytes */
                f = fopen(n->path, "rb");
                if (f == NULL)
                    return -1;
                for (i = 0; i < (size_t)nsl; i++) {
                    bm_cksum_ctx sctx;
                    bm_cksum_init(&sctx);
                    uint32_t sgot = 0;
                    if (fseeko(f, (off_t)slvec[i].off, SEEK_SET) == 0) {
                        while (sgot < slvec[i].len) {
                            size_t want = sizeof(buf);
                            if (want > slvec[i].len - sgot)
                                want = slvec[i].len - sgot;
                            size_t r = fread(buf, 1, want, f);
                            if (r == 0)
                                break;
                            bm_cksum_feed(&sctx, buf, r);
                            sgot += (uint32_t)r;
                        }
                    }
                    slvec[i].cksum = bm_cksum_finish(&sctx, sgot);
                }
                fclose(f);
            } else { /* thin: the whole file is the single slice */
                slvec[0].off = 0;
                slvec[0].len = (uint32_t)n->st.st_size;
                slvec[0].cksum = ck;
            }
            size_t reclen = 32 + 16 * (size_t)nsl + 8;
            uint8_t *rec = (uint8_t *)calloc(reclen, 1);
            if (rec == NULL)
                return -1;
            rec[0] = type;
            rec[1] = 1;
            w16(rec + 2, is_fat ? 0x200f : 0x100f);
            w16(rec + 4, (uint16_t)(n->st.st_mode & 0xffff));
            w32(rec + 6, (uint32_t)n->st.st_uid);
            w32(rec + 10, (uint32_t)n->st.st_gid);
            w32(rec + 14, (uint32_t)n->st.st_mtime);
            w32(rec + 18, (uint32_t)n->st.st_size);
            rec[22] = 1;
            w32(rec + 23, ck);
            rec[27] = 1; /* arch table flag */
            w32(rec + 28, (uint32_t)nsl);
            for (i = 0, pos = 32; i < (size_t)nsl; i++, pos += 16) {
                w32(rec + pos + 0, slvec[i].cputype);
                w32(rec + pos + 4, slvec[i].subtype);
                w32(rec + pos + 8, slvec[i].len);
                w32(rec + pos + 12, slvec[i].cksum);
            }
            /* rec[32+16n .. 39+16n]: linklen + pad (zeros) */
            *out = rec;
            *outlen = reclen;
            return 0;
        }

        tail = calloc(4, 1); /* 4 padding zero bytes: record is 35 bytes */
        if (tail == NULL)
            return -1;
        taillen = 4;
    } else if (n->type == BM_TYPE_LNK) {
        type = 3;
        size_t tlen = (size_t)n->st.st_size; /* symlink st_size == target len */
        char *target = (char *)malloc(tlen + 1);
        if (target == NULL)
            return -1;
        ssize_t l = readlink(n->path, target, tlen + 1);
        if (l < 0) {
            free(target);
            return -1;
        }
        target[l] = '\0';
        ck = bm_cksum(target, (size_t)l);
        lnklen = (uint32_t)l + 1;
        size_t tailn = (size_t)l + 1 + 8;
        tail = (uint8_t *)calloc(tailn, 1);
        if (tail == NULL) {
            free(target);
            return -1;
        }
        memcpy(tail, target, (size_t)l);
        tail[(size_t)l] = '\0';
        taillen = tailn;
        free(target);
    } else { /* dir */
        type = 2;
    }

    uint8_t base[31];
    base[0] = type;
    base[1] = 1;
    w16(base + 2, 0x000f);
    w16(base + 4, (uint16_t)(n->st.st_mode & 0xffff));
    w32(base + 6, (uint32_t)n->st.st_uid);
    w32(base + 10, (uint32_t)n->st.st_gid);
    w32(base + 14, (uint32_t)n->st.st_mtime);
    w32(base + 18, (uint32_t)n->st.st_size);
    base[22] = 1;
    w32(base + 23, ck);
    w32(base + 27, lnklen);

    *out = (uint8_t *)malloc(31 + taillen);
    if (*out == NULL) {
        free(tail);
        return -1;
    }
    memcpy(*out, base, 31);
    if (taillen)
        memcpy(*out + 31, tail, taillen);
    free(tail);
    *outlen = 31 + taillen;
    return 0;
}

/* ---- sort helpers ---- */

typedef struct main_entry {
    uint32_t pii;
    uint32_t file;
    uint32_t parent;
    const char *name;
} main_entry;

static int cmp_main(const void *a, const void *b) {
    const main_entry *x = (const main_entry *)a;
    const main_entry *y = (const main_entry *)b;
    if (x->parent != y->parent)
        return x->parent < y->parent ? -1 : 1;
    return strcmp(x->name, y->name);
}

typedef struct tr_entry {
    uint32_t empty;
    uint32_t str;
    char *rel;
} tr_entry;

static int cmp_rel(const void *a, const void *b) {
    const tr_entry *x = (const tr_entry *)a;
    const tr_entry *y = (const tr_entry *)b;
    return strcmp(x->rel, y->rel);
}

typedef struct gp_order {
    int g;
    uint32_t first_pid;
} gp_order;

static int cmp_gp(const void *a, const void *b) {
    const gp_order *x = (const gp_order *)a;
    const gp_order *y = (const gp_order *)b;
    return x->first_pid < y->first_pid ? -1 : x->first_pid > y->first_pid ? 1
                                                                          : 0;
}

static void bm_free_write(int *pr, int *file, int *pii, int *strb, int *empty,
                          int *t_tree, int *t_paths, int *t_prptr,
                          int *t_treeptr) {
    free(pr);
    free(file);
    free(pii);
    free(strb);
    free(empty);
    free(t_tree);
    free(t_paths);
    free(t_prptr);
    free(t_treeptr);
}

static void bm_seterr(char *errbuf, size_t errbufsz, const char *msg) {
    if (errbuf && errbufsz)
        snprintf(errbuf, errbufsz, "%s", msg);
}

int bm_write_bom(const bm_walk *w, const char *out_path,
                 char *errbuf, size_t errbufsz) {
    size_t npaths = w->npaths;
    size_t ngroups = w->ngroups;

    /* ---- simulation: assign block indices in pid (readdir DFS) order ---- */
    int *pr = (int *)malloc((npaths + 1) * sizeof(int));
    int *file = (int *)malloc((npaths + 1) * sizeof(int));
    int *pii = (int *)malloc((npaths + 1) * sizeof(int));
    int *strb = (int *)malloc((npaths + 1) * sizeof(int));
    int *empty = (int *)malloc((npaths + 1) * sizeof(int));
    int *t_tree = (int *)malloc((ngroups ? ngroups : 1) * sizeof(int));
    int *t_paths = (int *)malloc((ngroups ? ngroups : 1) * sizeof(int));
    int *t_prptr = (int *)malloc((ngroups ? ngroups : 1) * sizeof(int));
    int *t_treeptr = (int *)malloc((ngroups ? ngroups : 1) * sizeof(int));
    if (pr == NULL || file == NULL || pii == NULL || strb == NULL ||
        empty == NULL || t_tree == NULL || t_paths == NULL ||
        t_prptr == NULL || t_treeptr == NULL) {
        bm_free_write(pr, file, pii, strb, empty, t_tree, t_paths, t_prptr,
                      t_treeptr);
        bm_seterr(errbuf, errbufsz, "out of memory");
        return -1;
    }
    size_t i;
    for (i = 0; i <= npaths; i++)
        pr[i] = file[i] = pii[i] = strb[i] = empty[i] = -1;
    for (i = 0; i < (ngroups ? ngroups : 1); i++)
        t_tree[i] = t_paths[i] = t_prptr[i] = t_treeptr[i] = -1;

    int idx = 11;
    for (i = 0; i < npaths; i++) {
        const bm_path *n = &w->paths[i];
        uint32_t pid = n->pid;
        if (n->type == BM_TYPE_SPC)
            continue;
        int r = n->rank;
        if (r == 0 || r == 1) {
            pr[pid] = idx;
            file[pid] = idx + 1;
            pii[pid] = idx + 2;
            idx += 3;
        } else if (r == 2) {
            file[pid] = idx;
            pii[pid] = idx + 1;
            idx += 2;
            int g = n->group;
            t_tree[g] = idx;
            t_paths[g] = idx + 1;
            t_prptr[g] = idx + 2;
            t_treeptr[g] = idx + 3;
            idx += 4;
            const bm_group *gr = &w->groups[g];
            size_t k;
            for (k = 0; k < gr->nmem; k++) {
                uint32_t mp = gr->members[k];
                if (w->paths[mp - 1].rank <= 2) {
                    strb[mp] = idx;
                    empty[mp] = idx + 1;
                    idx += 2;
                }
            }
        } else { /* r >= 3 */
            file[pid] = idx;
            pii[pid] = idx + 1;
            idx += 2;
            strb[pid] = idx;
            empty[pid] = idx + 1;
            idx += 2;
        }
    }
    int nob = idx - 1;

    blob_store bs;
    if (blobs_init(&bs, nob) != 0) {
        bm_free_write(pr, file, pii, strb, empty, t_tree, t_paths, t_prptr,
                      t_treeptr);
        bm_seterr(errbuf, errbufsz, "out of memory");
        return -1;
    }
    FILE *fp = NULL;
    uint8_t *index = NULL;
    uint8_t *vars = NULL;

    /* ---- content emission ---- */
    for (i = 0; i < npaths; i++) {
        const bm_path *n = &w->paths[i];
        uint32_t pid = n->pid;
        if (n->type == BM_TYPE_SPC)
            continue;
        if (pr[pid] >= 0) {
            uint8_t *buf;
            size_t blen;
            if (build_pr(w, pid, &buf, &blen) != 0)
                goto write_err;
            put(&bs, pr[pid], buf, blen);
            free(buf);
        }
        size_t nl = strlen(n->name);
        uint8_t *f = (uint8_t *)malloc(4 + nl + 1);
        if (f == NULL)
            goto write_err;
        w32(f, n->parent);
        memcpy(f + 4, n->name, nl + 1);
        put(&bs, file[pid], f, 4 + nl + 1);
        free(f);
        uint8_t pii8[8];
        w32(pii8, pid);
        w32(pii8 + 4, 0); /* PR index patched below */
        put(&bs, pii[pid], pii8, 8);
    }

    for (i = 0; i < npaths; i++) {
        const bm_path *n = &w->paths[i];
        if (n->type == BM_TYPE_SPC)
            continue;
        uint32_t pid = n->pid;
        int pridx;
        if (pr[pid] >= 0) {
            pridx = pr[pid];
        } else {
            int g = n->group;
            pridx = pr[w->groups[g].members[0]];
        }
        uint8_t pii8[8];
        w32(pii8, pid);
        w32(pii8 + 4, (uint32_t)pridx);
        put(&bs, pii[pid], pii8, 8);
    }

    /* hard-link trailers */
    for (i = 0; i < ngroups; i++) {
        const bm_group *gr = &w->groups[i];
        int g = (int)i;
        int shared = pr[gr->members[0]];
        tr_entry *entries = (tr_entry *)malloc(gr->nmem * sizeof(tr_entry));
        if (entries == NULL)
            goto write_err;
        size_t k;
        for (k = 0; k < gr->nmem; k++) {
            uint32_t mp = gr->members[k];
            entries[k].empty = (uint32_t)empty[mp];
            entries[k].str = (uint32_t)strb[mp];
            entries[k].rel = bm_relpath(w, mp);
            if (entries[k].rel == NULL) {
                free(entries);
                goto write_err;
            }
        }
        qsort(entries, gr->nmem, sizeof(tr_entry), cmp_rel);

        size_t pathslen = 12 + 8 * gr->nmem;
        uint8_t *paths = (uint8_t *)calloc(pathslen, 1);
        if (paths == NULL) {
            for (k = 0; k < gr->nmem; k++)
                free(entries[k].rel);
            free(entries);
            goto write_err;
        }
        w16(paths, 1);
        w16(paths + 2, (uint16_t)gr->nmem);
        /* offsets 4..11 stay zero */
        for (k = 0; k < gr->nmem; k++) {
            w32(paths + 12 + 8 * k, entries[k].empty);
            w32(paths + 16 + 8 * k, entries[k].str);
        }

        uint8_t treehdr[21];
        memset(treehdr, 0, sizeof(treehdr));
        memcpy(treehdr, "tree", 4);
        w32(treehdr + 4, 1);
        w32(treehdr + 8, (uint32_t)t_paths[g]);
        w32(treehdr + 12, BM_TRAILER_PAD);
        w32(treehdr + 16, (uint32_t)gr->nmem);
        put(&bs, t_tree[g], treehdr, sizeof(treehdr));

        size_t psz = 12 + 8 * gr->nmem;
        size_t padded = psz > BM_TRAILER_PAD ? psz : BM_TRAILER_PAD;
        uint8_t *paths_pad = (uint8_t *)malloc(padded);
        if (paths_pad == NULL) {
            free(paths);
            for (k = 0; k < gr->nmem; k++)
                free(entries[k].rel);
            free(entries);
            goto write_err;
        }
        memset(paths_pad, 0, padded);
        memcpy(paths_pad, paths, pathslen);
        put(&bs, t_paths[g], paths_pad, padded);
        free(paths_pad);
        free(paths);

        uint8_t pr8[4];
        w32(pr8, (uint32_t)shared);
        put(&bs, t_prptr[g], pr8, 4);

        uint8_t tr8[4];
        w32(tr8, (uint32_t)t_tree[g]);
        put(&bs, t_treeptr[g], tr8, 4);

        for (k = 0; k < gr->nmem; k++) {
            uint32_t mp = gr->members[k];
            if (strb[mp] < 0)
                continue;
            char *rp = bm_relpath(w, mp);
            if (rp == NULL) {
                for (size_t j = 0; j < gr->nmem; j++)
                    free(entries[j].rel);
                free(entries);
                goto write_err;
            }
            put(&bs, strb[mp], rp, strlen(rp) + 1);
            free(rp);
            put(&bs, empty[mp], NULL, 0);
        }
        for (k = 0; k < gr->nmem; k++)
            free(entries[k].rel);
        free(entries);
    }

    /* ---- block 1 (BomInfo): header + per-cputype size entries ----
     * Matching Apple's mkbom:
     *   - entry 0 is (0, 0, <sum of non-Mach-O file sizes>, 0);
     *   - one (cputype, 0, <sum of slice sizes>, 0) entry per distinct
     *     cputype observed, subtype dropped, sizes summed across every
     *     Mach-O slice (thin files count their whole size, fat slices
     *     their fat_arch size);
     *   - entries are emitted in readdir (pre-order path scan) order of
     *     first occurrence, and each inode is only counted once.
     */
    size_t ncount = 0; /* non-special count */
    int has_file = 0;
    for (i = 0; i < npaths; i++) {
        const bm_path *n = &w->paths[i];
        if (n->type == BM_TYPE_SPC)
            continue;
        ncount++;
        if (n->type != BM_TYPE_DIR)
            has_file = 1;
    }
    uint64_t plain = 0;
    uint32_t arcp[BM_MAXSLICE];
    uint64_t arsz[BM_MAXSLICE];
    size_t narch = 0;
    uint64_t *keys = (uint64_t *)malloc((ncount ? ncount : 1) *
                                        sizeof(uint64_t));
    size_t nk = 0;
    if (keys == NULL)
        goto write_err;
    for (i = 0; i < npaths; i++) {
        const bm_path *n = &w->paths[i];
        if (n->type == BM_TYPE_SPC || n->type == BM_TYPE_DIR)
            continue;
        uint64_t k = ((uint64_t)(uint32_t)n->st.st_dev << 32) ^
                     (uint64_t)n->st.st_ino;
        size_t j;
        int seen = 0;
        for (j = 0; j < nk; j++)
            if (keys[j] == k) {
                seen = 1;
                break;
            }
        if (seen)
            continue;
        keys[nk++] = k;

        FILE *f = fopen(n->path, "rb");
        if (f == NULL) {
            plain += (uint64_t)n->st.st_size;
            continue;
        }
        uint8_t hdr[4096];
        size_t hl = fread(hdr, 1, sizeof(hdr), f);
        fclose(f);
        struct bm_mslice sl[BM_MAXSLICE];
        int is_fat = 0;
        int nsl = macho_slices(hdr, hl, (uint64_t)n->st.st_size, sl,
                               BM_MAXSLICE, &is_fat);
        if (nsl <= 0) {
            plain += (uint64_t)n->st.st_size;
            continue;
        }
        for (j = 0; j < (size_t)nsl; j++) {
            uint32_t cp = sl[j].cputype;
            size_t a;
            int found = 0;
            for (a = 0; a < narch; a++)
                if (arcp[a] == cp) {
                    found = 1;
                    break;
                }
            if (!found) {
                if (narch >= BM_MAXSLICE)
                    break;
                arcp[narch] = cp;
                arsz[narch] = 0;
                a = narch;
                narch++;
            }
            arsz[a] += (uint64_t)sl[j].len;
        }
    }
    free(keys);

    int ninfo = has_file ? (int)(1 + narch) : 0;
    uint8_t b1head[12];
    w32(b1head, 1);
    w32(b1head + 4, (uint32_t)(npaths + 1));
    w32(b1head + 8, (uint32_t)ninfo);
    if (ninfo) {
        size_t blen = 12 + 16 * (size_t)ninfo;
        uint8_t *b1 = (uint8_t *)malloc(blen);
        if (b1 == NULL)
            goto write_err;
        memcpy(b1, b1head, 12);
        w32(b1 + 12, 0);
        w32(b1 + 16, 0);
        w32(b1 + 20, (uint32_t)plain);
        w32(b1 + 24, 0);
        for (i = 0; i < (int)narch; i++) {
            w32(b1 + 28 + 16 * (size_t)i, arcp[i]);
            w32(b1 + 32 + 16 * (size_t)i, 0);
            w32(b1 + 36 + 16 * (size_t)i, (uint32_t)arsz[i]);
            w32(b1 + 40 + 16 * (size_t)i, 0);
        }
        put(&bs, 1, b1, blen);
        free(b1);
    } else {
        put(&bs, 1, b1head, 12);
    }

    /* blocks 2,3: Paths tree header + main Paths sorted by (parent,name) */
    main_entry *mains =
        (main_entry *)malloc((ncount ? ncount : 1) * sizeof(main_entry));
    if (mains == NULL)
        goto write_err;
    size_t mi = 0;
    for (i = 0; i < npaths; i++) {
        const bm_path *n = &w->paths[i];
        if (n->type == BM_TYPE_SPC)
            continue;
        mains[mi].pii = (uint32_t)pii[n->pid];
        mains[mi].file = (uint32_t)file[n->pid];
        mains[mi].parent = n->parent;
        mains[mi].name = n->name;
        mi++;
    }
    qsort(mains, ncount, sizeof(main_entry), cmp_main);

    uint8_t b3[BM_TREE_PAD];
    memset(b3, 0, sizeof(b3));
    w16(b3, 1);
    w16(b3 + 2, (uint16_t)ncount);
    for (i = 0; i < ncount; i++) {
        w32(b3 + 12 + 8 * i, mains[i].pii);
        w32(b3 + 16 + 8 * i, mains[i].file);
    }
    free(mains);

    {
        uint8_t b2[21];
        memset(b2, 0, sizeof(b2));
        memcpy(b2, "tree", 4);
        w32(b2 + 4, 1);
        w32(b2 + 8, 3);
        w32(b2 + 12, BM_TREE_PAD);
        w32(b2 + 16, (uint32_t)ncount);
        put(&bs, 2, b2, sizeof(b2));
    }
    put(&bs, 3, b3, BM_TREE_PAD);

    /* blocks 4,5: HLIndex tree header + HLPaths sorted by first member pid */
    gp_order *gps = (gp_order *)malloc((ngroups ? ngroups : 1) * sizeof(*gps));
    if (gps == NULL)
        goto write_err;
    for (i = 0; i < ngroups; i++) {
        gps[i].g = (int)i;
        gps[i].first_pid = w->groups[i].first_pid;
    }
    qsort(gps, ngroups, sizeof(gp_order), cmp_gp);

    uint8_t b5[BM_TREE_PAD];
    memset(b5, 0, sizeof(b5));
    w16(b5, 1);
    w16(b5 + 2, (uint16_t)ngroups);
    for (i = 0; i < ngroups; i++) {
        int g = gps[i].g;
        w32(b5 + 12 + 8 * i, (uint32_t)t_treeptr[g]);
        w32(b5 + 16 + 8 * i, (uint32_t)t_prptr[g]);
    }
    free(gps);

    {
        uint8_t b4[21];
        memset(b4, 0, sizeof(b4));
        memcpy(b4, "tree", 4);
        w32(b4 + 4, 1);
        w32(b4 + 8, 5);
        w32(b4 + 12, BM_TREE_PAD);
        w32(b4 + 16, (uint32_t)ngroups);
        put(&bs, 4, b4, sizeof(b4));
    }
    put(&bs, 5, b5, BM_TREE_PAD);

    /* blocks 6..10: VIndex + Size64 (both empty) */
    {
        uint8_t b6[13];
        memset(b6, 0, sizeof(b6));
        w32(b6, 1);
        w32(b6 + 4, 7);
        w32(b6 + 8, 0);
        put(&bs, 6, b6, sizeof(b6));
    }
    {
        uint8_t b7[21];
        memset(b7, 0, sizeof(b7));
        memcpy(b7, "tree", 4);
        w32(b7 + 4, 1);
        w32(b7 + 8, 8);
        w32(b7 + 12, 0x80);
        w32(b7 + 16, 0);
        put(&bs, 7, b7, sizeof(b7));
    }
    {
        uint8_t b8[0x80];
        memset(b8, 0, sizeof(b8));
        w16(b8, 1);
        w16(b8 + 2, 0);
        put(&bs, 8, b8, sizeof(b8));
    }
    {
        uint8_t b9[21];
        memset(b9, 0, sizeof(b9));
        memcpy(b9, "tree", 4);
        w32(b9 + 4, 1);
        w32(b9 + 8, 10);
        w32(b9 + 12, BM_TREE_PAD);
        w32(b9 + 16, 0);
        put(&bs, 9, b9, sizeof(b9));
    }

    uint8_t b10[BM_TREE_PAD];
    memset(b10, 0, sizeof(b10));
    w16(b10, 1);
    w16(b10 + 2, 0);
    put(&bs, 10, b10, sizeof(b10));

    /* ---- assemble file ---- */
    uint64_t off = BM_FIXED_START;
    uint64_t *offsets = (uint64_t *)calloc((size_t)nob + 1, sizeof(uint64_t));
    uint64_t *lens = (uint64_t *)calloc((size_t)nob + 1, sizeof(uint64_t));
    if (offsets == NULL || lens == NULL) {
        free(offsets);
        free(lens);
        goto write_err;
    }
    for (i = 1; i <= (size_t)nob; i++) {
        if (bs.present[i]) {
            offsets[i] = off;
            lens[i] = (uint64_t)bs.len[i];
            off += bs.len[i];
        } else {
            offsets[i] = 0;
            lens[i] = 0;
        }
    }
    uint64_t idxoff = off;

    size_t indexlen = 4 + (size_t)BM_POOL * 8 + 4;
    index = (uint8_t *)calloc(indexlen, 1);
    if (index == NULL) {
        free(offsets);
        free(lens);
        goto write_err;
    }
    w32(index, (uint32_t)BM_POOL);
    /* slot 0: (0,0) */
    for (i = 1; i <= (size_t)nob; i++) {
        w32(index + 4 + 8 * i, (uint32_t)(offsets[i] & 0xffffffffu));
        w32(index + 8 + 8 * i, (uint32_t)(lens[i] & 0xffffffffu));
    }
    /* remaining pool slots (nob+1 .. POOL-1) stay (0,0); final free count 0 */
    free(offsets);
    free(lens);

    vars = (uint8_t *)malloc(4 + 12 + 10 + 12 + 11 + 11);
    if (vars == NULL) {
        free(index);
        goto write_err;
    }
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
    size_t varslen = 4 + 12 + 10 + 12 + 11 + 11;

    uint8_t header[32];
    memcpy(header, "BOMStore", 8);
    w32(header + 8, 1);
    w32(header + 12, (uint32_t)nob);
    w32(header + 16, (uint32_t)(idxoff & 0xffffffffu));
    w32(header + 20, (uint32_t)indexlen);
    w32(header + 24, (uint32_t)((idxoff + indexlen) & 0xffffffffu));
    w32(header + 28, (uint32_t)varslen);

    fp = fopen(out_path, "wb");
    if (fp == NULL)
        goto write_err;
    if (fwrite(header, 1, sizeof(header), fp) != sizeof(header))
        goto ferr;
    {
        uint8_t pad[BM_FIXED_START - sizeof(header)];
        memset(pad, 0, sizeof(pad));
        if (fwrite(pad, 1, sizeof(pad), fp) != sizeof(pad))
            goto ferr;
    }
    for (i = 1; i <= (size_t)nob; i++) {
        if (!bs.present[i])
            continue;
        if (bs.len[i] &&
            fwrite(bs.data[i], 1, bs.len[i], fp) != bs.len[i])
            goto ferr;
    }
    if (fwrite(index, 1, indexlen, fp) != indexlen)
        goto ferr;
    if (fwrite(vars, 1, varslen, fp) != varslen)
        goto ferr;
    if (fclose(fp) != 0)
        fp = NULL;
    free(index);
    free(vars);
    blobs_free(&bs);
    bm_free_write(pr, file, pii, strb, empty, t_tree, t_paths, t_prptr,
                  t_treeptr);
    return 0;

ferr:
    if (fp != NULL)
        fclose(fp);
    free(index);
    free(vars);
    blobs_free(&bs);
    bm_free_write(pr, file, pii, strb, empty, t_tree, t_paths, t_prptr,
                  t_treeptr);
    bm_seterr(errbuf, errbufsz, strerror(errno));
    return -1;

write_err:
    free(index);
    free(vars);
    blobs_free(&bs);
    bm_free_write(pr, file, pii, strb, empty, t_tree, t_paths, t_prptr,
                  t_treeptr);
    bm_seterr(errbuf, errbufsz, strerror(errno));
    return -1;
}