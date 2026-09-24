/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * Clean-room BOMBom: the bom-file object surface of Bom.framework.
 *
 * Read side is authoritative (opens any BOMStore, enumerates the Paths
 * tree, resolves nodes by path).  Write side:
 *   - BOMBomNewFromDirectoryWithOptions scans with fs_walk and reuses the
 *     byte-identical bm_write_bom (same output as /usr/bin/mkbom);
 *   - BOMBomNewFromBomWithOptions clones a readable bom to a new path; when
 *     arch/lang filters are supplied it re-emits from decoded rows instead
 *     of copying bytes (no hard-link group trailers; the no-filter path
 *     stays a verbatim copy);
 *   - BOMBomInsertFSObject/RemoveFSObject build a flat in-memory tree
 *     (roots seeded by NewWithSys); BOMBomFree re-emits the archive via
 *     bom_reencode when it was created for writing (commit-on-free).
 * Reference clients pass BOMSys = NULL everywhere. */
#include "bom_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>

#include "fs_walk.h"
#include "bom_writer.h"
#include "bom_reencode.h"

/* ---- pending-tree helpers (NewWithSys write-on-free) ---- */

/* Parent of a "./..." path: "./a/b" -> "./a", "./a" -> ".", "." -> NULL.
 * Returns a malloc'd copy, or NULL when the path has no parent. */
static char *pend_parent(const char *path) {
    const char *slash;
    size_t len;
    char *out;
    if (path == NULL || strcmp(path, ".") == 0)
        return NULL;
    slash = strrchr(path, '/');
    if (slash == NULL)
        return NULL;
    len = (size_t)(slash - path);
    if (len == 2 && path[0] == '.' && path[1] == '/')
        return strdup(".");
    if (len == 0)
        return NULL;
    out = (char *)malloc(len + 1);
    if (out != NULL) {
        memcpy(out, path, len);
        out[len] = '\0';
    }
    return out;
}

/* Raw PathRecord bytes for an implied directory node (root too). */
static uint8_t *pend_dir_pr(size_t *prlen) {
    bom_pathrec d;
    uint8_t *out = NULL;
    size_t outlen = 0;
    memset(&d, 0, sizeof(d));
    d.path_type = BM_PT_DIR;
    d.architecture = 0x000f;
    d.mode = 0040755;
    d.valid = 1;
    if (bom_pr_rebuild(&d, &out, &outlen) != 0)
        return NULL;
    *prlen = outlen;
    return out;
}

/* Find a pending row by its full path. */
static struct bom_pend_row *pend_find(BOMBom *bom, const char *path) {
    size_t i;
    for (i = 0; i < bom->npend; i++)
        if (strcmp(bom->pend[i].path, path) == 0)
            return &bom->pend[i];
    return NULL;
}

/* Grow the pending array; returns a zeroed slot or NULL. */
static struct bom_pend_row *pend_slot(BOMBom *bom) {
    if (bom->npend == bom->pendcap) {
        size_t nc = bom->pendcap ? bom->pendcap * 2 : 8;
        struct bom_pend_row *np = (struct bom_pend_row *)realloc(
            bom->pend, nc * sizeof(*np));
        if (np == NULL)
            return NULL;
        bom->pend = np;
        bom->pendcap = nc;
    }
    memset(&bom->pend[bom->npend], 0, sizeof(bom->pend[0]));
    return &bom->pend[bom->npend];
}

/* Ensure a pending row exists for `path`, creating (and appending, parent
 * before child) any missing ancestors as implied directories.  When `path`
 * does not exist yet it gets the rows' pid from the monotonic allocator and
 * a raw PathRecord built by `bom_pr_rebuild` from its metadata; tasks that
 * build the record themselves pass a pre-built record via pr/prlen.  The
 * root row (".") is created by NewWithSys. */
static struct bom_pend_row *pend_put(BOMBom *bom, const char *path,
                                     const uint8_t *pr, size_t prlen) {
    struct bom_pend_row *row, *prow;
    uint8_t *own_pr = NULL;
    const uint8_t *use_pr;
    const char *leaf;
    const char *slash;
    size_t use_len;
    char *parent;

    row = pend_find(bom, path);
    if (row != NULL) {
        /* The node already exists (e.g. the seeded root, or a caller
         * inserting metadata for a path laid down as an implied dir).
         * Adopt the caller's record when one is supplied; recursive
         * ancestor calls always pass NULL and keep what is there. */
        if (pr != NULL) {
            uint8_t *np = (uint8_t *)malloc(prlen);
            if (np == NULL)
                return NULL;
            memcpy(np, pr, prlen);
            free(row->pr);
            row->pr = np;
            row->prlen = prlen;
        }
        return row;
    }

    /* Recurse to the parent first so rows are always appended parent-before-
     * child and the parent's pid exists before the child is created. */
    parent = pend_parent(path);
    prow = NULL;
    uint32_t parent_pid = 0;
    if (parent != NULL) {
        prow = pend_put(bom, parent, NULL, 0);
        free(parent);
        if (prow == NULL)
            return NULL;
        /* Capture now: pend_slot() below may realloc() the array. */
        parent_pid = prow->pid;
    }

    /* A path that is not the root of this tree has a parent pid; the root
     * row (already seeded by NewWithSys) has parent 0. */
    use_pr = pr;
    use_len = prlen;
    if (use_pr == NULL) {
        own_pr = pend_dir_pr(&use_len);
        if (own_pr == NULL)
            return NULL;
        use_pr = own_pr;
    }

    row = pend_slot(bom);
    if (row == NULL) {
        free(own_pr);
        return NULL;
    }
    row->path = strdup(path);
    row->pr = (uint8_t *)malloc(use_len);
    if (row->path == NULL || row->pr == NULL) {
        free(row->path);
        free(row->pr);
        free(own_pr);
        return NULL;
    }
    memcpy(row->pr, use_pr, use_len);
    row->prlen = use_len;
    if (strcmp(path, ".") == 0) {
        row->parent = 0;
        leaf = ".";
    } else {
        row->parent = parent_pid;
        slash = strrchr(path, '/');
        leaf = slash ? slash + 1 : path;
    }
    row->leaf = strdup(leaf);
    if (row->leaf == NULL) {
        free(row->path);
        free(row->pr);
        free(own_pr);
        return NULL;
    }
    row->pid = bom->next_pid++;
    bom->npend++;
    free(own_pr);
    return row;
}

/* ---- open / new ---- */

BOMBom *BOMBomNewWithSys(const char *path, BOMSys *sys) {
    BOMBom *b;
    (void)sys;
    if (path == NULL)
        return NULL;
    b = (BOMBom *)calloc(1, sizeof(BOMBom));
    if (b == NULL)
        return NULL;
    b->path = strdup(path);
    if (b->path == NULL) {
        free(b);
        return NULL;
    }
    b->for_write = 1;
    b->next_pid = 1; /* root row takes pid 1 */
    /* Seed the root row ("." dir, pid 1) so the pending tree always has a
     * parent-before-child set that bom_reencode can commit on free. */
    if (pend_put(b, ".", NULL, 0) == NULL) {
        free(b->path);
        free(b);
        return NULL;
    }
    return b;
}

static BOMBom *bom_open_common(const char *path) {
    BOMBom *b;
    BOMStorage *s;
    if (path == NULL)
        return NULL;
    s = _bom_storage_open(path);
    if (s == NULL)
        return NULL;
    b = (BOMBom *)calloc(1, sizeof(BOMBom));
    if (b == NULL) {
        _bom_storage_close(s);
        return NULL;
    }
    b->path = strdup(path);
    if (b->path == NULL) {
        _bom_storage_close(s);
        free(b);
        return NULL;
    }
    b->storage = s;
    b->open = 1;
    return b;
}

BOMBom *BOMBomOpen(const char *path, uint32_t flags) {
    (void)flags;
    return bom_open_common(path);
}

BOMBom *BOMBomOpenWithSys(const char *path, uint32_t flags, BOMSys *sys) {
    (void)flags;
    (void)sys;
    return bom_open_common(path);
}

BOMBom *BOMBomNewFromDirectoryWithOptions(const char *bomPath,
                                          const char *dirPath,
                                          uint32_t options,
                                          uint32_t flags) {
    bm_walk w;
    char errbuf[512];
    BOMBom *b;
    (void)options;
    (void)flags;
    if (bomPath == NULL || dirPath == NULL)
        return NULL;
    if (bm_scan(dirPath, &w) != 0)
        return NULL;
    if (bm_write_bom(&w, bomPath, errbuf, sizeof errbuf) != 0) {
        bm_walk_free(&w);
        return NULL;
    }
    bm_walk_free(&w);
    b = bom_open_common(bomPath);
    if (b != NULL)
        b->for_write = 0;
    return b;
}

/* CFArray helpers for the arch/lang filters.  archFilter carries CFNumber
 * cputypes; langFilter carries CFString language codes ("en", "fr", ...). */

static int filter_has_cputype(const void *archFilter, uint32_t cputype) {
    CFArrayRef arr = (CFArrayRef)archFilter;
    CFIndex i, n;
    if (arr == NULL)
        return 1;
    n = CFArrayGetCount(arr);
    for (i = 0; i < n; i++) {
        CFNumberRef nr = (CFNumberRef)CFArrayGetValueAtIndex(arr, i);
        int v = 0;
        if (nr == NULL)
            continue;
        if (CFNumberGetValue(nr, kCFNumberIntType, &v) &&
            (uint32_t)v == cputype)
            return 1;
    }
    return 0;
}

/* A language pack directory keeps its "en.lproj" form only when the code
 * before ".lproj" is in the allowed set. */
static int lang_allowed(const void *langFilter, const char *name,
                        size_t name_len) {
    static const char lproj[] = ".lproj";
    size_t lproj_len = sizeof(lproj) - 1;
    CFArrayRef arr = (CFArrayRef)langFilter;
    CFIndex i, n;
    char code[128];
    size_t clen;
    if (arr == NULL)
        return 1;
    if (name_len <= lproj_len ||
        memcmp(name + name_len - lproj_len, lproj, lproj_len) != 0)
        return 1; /* not a language pack directory */
    clen = name_len - lproj_len;
    if (clen >= sizeof code)
        clen = sizeof code - 1;
    memcpy(code, name, clen);
    code[clen] = '\0';
    n = CFArrayGetCount(arr);
    for (i = 0; i < n; i++) {
        CFStringRef sr = (CFStringRef)CFArrayGetValueAtIndex(arr, i);
        char sbuf[128];
        if (sr == NULL)
            continue;
        if (CFStringGetCString(sr, sbuf, sizeof sbuf, kCFStringEncodingUTF8) &&
            strcmp(sbuf, code) == 0)
            return 1;
    }
    return 0;
}

/* Slices of a Mach-O record survive only when their cputype is allowed. */
static uint32_t keep_slices(uint8_t *dst, const uint8_t *src, uint32_t nslice,
                            const void *archFilter) {
    uint32_t i, kept = 0;
    for (i = 0; i < nslice; i++) {
        uint32_t cp = _bom_r32(src + 16 * i);
        if (filter_has_cputype(archFilter, cp)) {
            memcpy(dst + 16 * kept, src + 16 * i, 16);
            kept++;
        }
    }
    return kept;
}

/* Rebuild the archive at outPath from decoded rows, applying the arch and
 * language filters.  Returns the opened re-encoded bom, or NULL on error. */
static BOMBom *bom_rebuild_with_options(const char *outPath, BOMBom *bom,
                                        const void *archFilter,
                                        const void *langFilter) {
    BOMTree *t;
    struct bom_tree_row *r;
    bom_reenc_row *rows;
    uint8_t **rawpr;
    size_t *rawprlen;
    uint8_t *slicebuf = NULL;
    uint8_t *rebuild = NULL;
    size_t rebuild_len = 0;
    uint32_t i;
    uint8_t *dropped = NULL;
    size_t nkeep = 0, slot = 0;
    uint32_t maxpid = 0;
    char errbuf[512];
    BOMBom *b;
    uint32_t nslice_total = 0;

    t = BOMBomPathsTree(bom);
    if (t == NULL)
        return NULL;

    /* Decide drop for every row first: a path is dropped when its parent is
     * a dropped directory (language pruning propagates down the subtree) or
     * when an arch filter strips a Mach-O file to zero slices. */
    for (i = 0; i < t->nrows; i++) {
        r = &t->rows[i];
        if (r->pid > maxpid)
            maxpid = r->pid;
        nslice_total += r->pr.nslice;
    }
    if (t->nrows > 500) { /* single Paths block: 12 + 8*n <= 0x1000 */
        return NULL;
    }
    dropped = (uint8_t *)calloc((size_t)maxpid + 1, 1);
    rawpr = (uint8_t **)calloc(t->nrows ? t->nrows : 1, sizeof(uint8_t *));
    rawprlen = (size_t *)calloc(t->nrows ? t->nrows : 1, sizeof(size_t));
    rows = (bom_reenc_row *)calloc(t->nrows ? t->nrows : 1,
                                   sizeof(bom_reenc_row));
    if (dropped == NULL || rawpr == NULL || rawprlen == NULL || rows == NULL)
        goto fail;
    slicebuf = (uint8_t *)malloc(16 * (size_t)(nslice_total ? nslice_total : 1));
    if (slicebuf == NULL)
        goto fail;

    for (i = 0; i < t->nrows; i++) {
        uint32_t parent;
        uint32_t nslice_kept;

        r = &t->rows[i];
        /* Language pruning: a dropped parent directory drops this path too. */
        if (r->parent != 0 && r->parent <= maxpid && dropped[r->parent]) {
            dropped[r->pid] = 1;
            continue;
        }
        if (!lang_allowed(langFilter, r->leaf, r->name_len)) {
            dropped[r->pid] = 1;
            continue;
        }
        /* Arch pruning: keep only allowed slices. */
        nslice_kept = r->pr.nslice;
        if (archFilter != NULL && r->pr.nslice > 0) {
            nslice_kept = keep_slices(slicebuf, r->pr.slices,
                                      r->pr.nslice, archFilter);
            if (nslice_kept == 0) {
                dropped[r->pid] = 1;
                continue;
            }
        }
        /* Build the PathRecord bytes this row will emit. */
        if (archFilter == NULL || r->pr.nslice == 0) {
            uint32_t prlen;
            rawpr[slot] = (uint8_t *)bom_block(&bom->storage->bf, r->prblk,
                                               &prlen);
            if (rawpr[slot] == NULL)
                goto fail;
            rawprlen[slot] = prlen;
        } else {
            bom_pathrec pr2;
            pr2 = r->pr;
            pr2.slices = slicebuf;
            pr2.nslice = nslice_kept;
            if (bom_pr_rebuild(&pr2, &rebuild, &rebuild_len) != 0)
                goto fail;
            rawpr[slot] = rebuild;
            rawprlen[slot] = rebuild_len;
            rebuild = NULL;
        }
        parent = r->parent;
        rows[slot].pid = r->pid;
        rows[slot].parent = parent;
        rows[slot].name = r->leaf;
        rows[slot].pr = rawpr[slot];
        rows[slot].prlen = rawprlen[slot];
        slot++;
    }
    nkeep = slot;
    if (nkeep == 0)
        goto fail;

    if (bom_reencode(rows, nkeep, outPath, errbuf, sizeof errbuf) != 0)
        goto fail;

    b = bom_open_common(outPath);
    if (b != NULL)
        b->for_write = 0;
    free(dropped);
    free(rawpr);
    free(rawprlen);
    free(rows);
    free(slicebuf);
    free(rebuild);
    return b;

fail:
    free(dropped);
    free(rawpr);
    free(rawprlen);
    free(rows);
    free(slicebuf);
    free(rebuild);
    return NULL;
}

BOMBom *BOMBomNewFromBomWithOptions(const char *outPath, BOMBom *bom,
                                    uint32_t options,
                                    const void *archFilter,
                                    const void *langFilter) {
    FILE *infile, *outfile;
    uint8_t buf[65536];
    size_t n;
    BOMBom *b;
    (void)options;
    if (outPath == NULL || bom == NULL || !bom->open || bom->path == NULL)
        return NULL;
    /* No filters: verbatim copy (byte parity with the source). */
    if (archFilter == NULL && langFilter == NULL) {
        infile = fopen(bom->path, "rb");
        if (infile == NULL)
            return NULL;
        outfile = fopen(outPath, "wb");
        if (outfile == NULL) {
            fclose(infile);
            return NULL;
        }
        while ((n = fread(buf, 1, sizeof buf, infile)) > 0)
            if (fwrite(buf, 1, n, outfile) != n) {
                fclose(outfile);
                fclose(infile);
                return NULL;
            }
        fclose(outfile);
        fclose(infile);
        b = bom_open_common(outPath);
        if (b != NULL)
            b->for_write = 0;
        return b;
    }
    /* Filters present: rebuild the archive from decoded rows. */
    return bom_rebuild_with_options(outPath, bom, archFilter, langFilter);
}

/* ---- accessors ---- */

BOMTree *BOMBomPathsTree(BOMBom *bom) {
    if (bom == NULL || !bom->open)
        return NULL;
    if (bom->paths == NULL)
        bom->paths = BOMTreeOpenWithName(bom->storage, "Paths", 0);
    return bom->paths;
}

/* Build an FSObject for tree row index i; NULL when out of range. */
static BOMFSObject *row_fso(BOMBom *bom, uint32_t i) {
    BOMTree *t = BOMBomPathsTree(bom);
    if (t == NULL || i >= t->nrows)
        return NULL;
    return _bom_fso_from_row(&t->rows[i]);
}

BOMFSObject *BOMBomGetRootFSObject(BOMBom *bom) {
    BOMTree *t;
    uint32_t i;
    if (bom == NULL || !bom->open)
        return NULL;
    t = BOMBomPathsTree(bom);
    if (t == NULL)
        return NULL;
    for (i = 0; i < t->nrows; i++)
        if (t->rows[i].pid == 1)
            return row_fso(bom, i);
    return NULL;
}

static char *dot_path(const char *path) {
    /* Normalize "path/to/node" to "./path/to/node" for lookup; "." stays ".". */
    size_t n = strlen(path);
    char *out;
    if (path[0] == '.' && (n == 1 || path[1] == '/'))
        return strdup(path);
    out = (char *)malloc(n + 3);
    if (out == NULL)
        return NULL;
    out[0] = '.';
    out[1] = '/';
    memcpy(out + 2, path, n + 1);
    return out;
}

BOMFSObject *BOMBomGetFSObjectAtPath(BOMBom *bom, const char *path) {
    BOMTree *t;
    char *want;
    uint32_t i;
    if (bom == NULL || !bom->open || path == NULL)
        return NULL;
    t = BOMBomPathsTree(bom);
    if (t == NULL)
        return NULL;
    want = dot_path(path);
    if (want == NULL)
        return NULL;
    for (i = 0; i < t->nrows; i++) {
        if (t->rows[i].path != NULL &&
            strcmp(t->rows[i].path, want) == 0) {
            BOMFSObject *o = row_fso(bom, i);
            free(want);
            return o;
        }
    }
    free(want);
    return NULL;
}

/* ---- mutation (flat-tree write-on-free) ---- */

/* Build the raw PathRecord bytes a NewWithSys insert stores, from the
 * object's metadata (mirrors fso/pr-layout of the read side). */
static int obj_pr_bytes(const BOMFSObject *obj, uint8_t **out, size_t *outlen) {
    bom_pathrec pr;
    memset(&pr, 0, sizeof(pr));
    switch (obj->type) {
    case BOM_TYPE_DIR:
        pr.path_type = BM_PT_DIR;
        break;
    case BOM_TYPE_LINK:
        pr.path_type = BM_PT_LINK;
        break;
    default:
        pr.path_type = BM_PT_FILE;
        break;
    }
    pr.architecture = obj->binary ? (obj->nslice > 1 ? 0x200f : 0x100f) : 0x000f;
    pr.mode = (uint16_t)(obj->mode & 0xffff);
    pr.uid = obj->uid;
    pr.gid = obj->gid;
    pr.mtime = obj->mtime;
    pr.size = obj->size;
    pr.checksum = obj->checksum;
    pr.link = (const uint8_t *)obj->link;
    pr.link_len = (uint32_t)obj->link_len;
    pr.slices = obj->slices;
    pr.nslice = obj->nslice;
    pr.valid = 1;
    if (bom_pr_rebuild(&pr, out, outlen) != 0)
        return -1;
    return 0;
}

int BOMBomInsertFSObject(BOMBom *bom, BOMFSObject *obj, uint32_t flags) {
    const char *path;
    char *want;
    uint8_t *pr = NULL;
    size_t prlen = 0;
    int rc = -1;
    (void)flags;
    if (bom == NULL || obj == NULL || !bom->for_write)
        return -1;
    path = BOMFSObjectPathName(obj);
    if (path == NULL)
        return -1;
    want = dot_path(path);
    if (want == NULL)
        return -1;
    if (obj_pr_bytes(obj, &pr, &prlen) != 0) {
        free(want);
        return -1;
    }
    if (pend_put(bom, want, pr, prlen) != NULL)
        rc = 0;
    free(want);
    free(pr);
    return rc;
}

int BOMBomRemoveFSObject(BOMBom *bom, BOMFSObject *obj) {
    const char *path;
    char *want;
    size_t wantlen;
    size_t i, w;
    int removed = 0;
    if (bom == NULL || obj == NULL || !bom->for_write)
        return -1;
    path = BOMFSObjectPathName(obj);
    if (path == NULL)
        return -1;
    want = dot_path(path);
    if (want == NULL)
        return -1;
    wantlen = strlen(want);
    /* Remove the node and its whole subtree (path prefix: self or "path/"). */
    w = 0;
    for (i = 0; i < bom->npend; i++) {
        const char *p = bom->pend[i].path;
        int del = (strcmp(p, want) == 0) ||
                  (strncmp(p, want, wantlen) == 0 && p[wantlen] == '/');
        if (del) {
            free(bom->pend[i].path);
            free(bom->pend[i].leaf);
            free(bom->pend[i].pr);
            removed = 1;
        } else {
            if (w != i)
                bom->pend[w] = bom->pend[i];
            w++;
        }
    }
    bom->npend = w;
    free(want);
    return removed ? 0 : -1;
}

/* ---- enumeration (children of the root) ---- */

BOMBomEnumerator *BOMBomEnumeratorNewWithOptions(BOMBom *bom,
                                                 uint64_t options,
                                                 uint32_t flags) {
    BOMBomEnumerator *e;
    BOMTree *t;
    uint32_t root = 0, i;
    (void)options;
    (void)flags;
    if (bom == NULL || !bom->open)
        return NULL;
    t = BOMBomPathsTree(bom);
    if (t == NULL)
        return NULL;
    for (i = 0; i < t->nrows; i++)
        if (t->rows[i].pid == 1) {
            root = 1;
            break;
        }
    (void)root;
    e = (BOMBomEnumerator *)calloc(1, sizeof(BOMBomEnumerator));
    if (e == NULL)
        return NULL;
    e->bom = bom;
    e->parent = 1; /* enumerate children whose parent id == 1 */
    return e;
}

BOMFSObject *BOMBomEnumeratorNext(BOMBomEnumerator *enumer) {
    BOMTree *t;
    if (enumer == NULL || enumer->bom == NULL)
        return NULL;
    t = BOMBomPathsTree(enumer->bom);
    if (t == NULL)
        return NULL;
    while (enumer->i < t->nrows) {
        uint32_t i = enumer->i++;
        if (t->rows[i].parent == enumer->parent)
            return row_fso(enumer->bom, i);
    }
    return NULL;
}

void BOMBomEnumeratorFree(BOMBomEnumerator *enumer) {
    free(enumer);
}

/* Commit ordering: Apple's reader walks the Paths block in (parent,name)
 * key order, so rows must be emitted parent-then-child sorted by leaf. */
static int pend_cmp(const void *a, const void *b) {
    const struct bom_pend_row *ra = (const struct bom_pend_row *)a;
    const struct bom_pend_row *rb = (const struct bom_pend_row *)b;
    if (ra->parent != rb->parent)
        return ra->parent < rb->parent ? -1 : 1;
    return strcmp(ra->leaf, rb->leaf);
}

void BOMBomFree(BOMBom *bom) {
    size_t i;
    if (bom == NULL)
        return;
    /* Commit-on-free: a NewWithSys tree is emitted when the handle goes
     * away. Rows are parent-before-child by construction. */
    if (bom->for_write && !bom->committed && bom->path != NULL &&
        bom->npend > 0) {
        bom_reenc_row *rows;
        char errbuf[512];
        qsort(bom->pend, bom->npend, sizeof(bom->pend[0]), pend_cmp);
        rows = (bom_reenc_row *)calloc(bom->npend, sizeof(bom_reenc_row));
        if (rows != NULL) {
            for (i = 0; i < bom->npend; i++) {
                rows[i].pid = bom->pend[i].pid;
                rows[i].parent = bom->pend[i].parent;
                rows[i].name = bom->pend[i].leaf;
                rows[i].pr = bom->pend[i].pr;
                rows[i].prlen = bom->pend[i].prlen;
            }
            if (bom_reencode(rows, bom->npend, bom->path, errbuf,
                             sizeof errbuf) != 0)
                fprintf(stderr, "BOMBomFree: commit failed: %s\n", errbuf);
            free(rows);
        }
        bom->committed = 1;
    }
    for (i = 0; i < bom->npend; i++) {
        free(bom->pend[i].path);
        free(bom->pend[i].leaf);
        free(bom->pend[i].pr);
    }
    free(bom->pend);
    BOMTreeFree(bom->paths);
    if (bom->storage != NULL)
        _bom_storage_close(bom->storage);
    free(bom->path);
    free(bom);
}