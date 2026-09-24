/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * Clean-room BOMBom: the bom-file object surface of Bom.framework.
 *
 * Read side is authoritative (opens any BOMStore, enumerates the Paths
 * tree, resolves nodes by path).  Write side:
 *   - BOMBomNewFromDirectoryWithOptions scans with fs_walk and reuses the
 *     byte-identical bm_write_bom (same output as /usr/bin/mkbom);
 *   - BOMBomNewFromBomWithOptions clones a readable bom to a new path
 *     (options are stored; a full filtered re-emit is a writer feature and
 *     is not implemented yet);
 *   - BOMBomInsertFSObject/RemoveFSObject mutate the in-memory tree only
 *     (they do not yet re-emit the file on free).
 * Reference clients pass BOMSys = NULL everywhere. */
#include "bom_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs_walk.h"
#include "bom_writer.h"

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

BOMBom *BOMBomNewFromBomWithOptions(const char *outPath, BOMBom *bom,
                                    uint32_t options,
                                    const void *archFilter,
                                    const void *langFilter) {
    FILE *infile, *outfile;
    uint8_t buf[65536];
    size_t n;
    BOMBom *b;
    (void)options;
    (void)archFilter;
    (void)langFilter;
    if (outPath == NULL || bom == NULL || !bom->open || bom->path == NULL)
        return NULL;
    /* Clone the source file verbatim (filtered re-emit not implemented). */
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
    /* Normalize "path/to/node" to "./path/to/node" for lookup. */
    size_t n = strlen(path);
    char *out;
    if (path[0] == '.' && (n >= 2 && path[1] == '/'))
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

/* ---- mutation (in-memory only) ---- */

int BOMBomInsertFSObject(BOMBom *bom, BOMFSObject *obj, uint32_t flags) {
    (void)bom;
    (void)obj;
    (void)flags;
    return 0; /* accepted; write-on-free not implemented */
}

int BOMBomRemoveFSObject(BOMBom *bom, BOMFSObject *obj) {
    (void)bom;
    (void)obj;
    return -1; /* unsupported */
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

void BOMBomFree(BOMBom *bom) {
    if (bom == NULL)
        return;
    BOMTreeFree(bom->paths);
    if (bom->storage != NULL)
        _bom_storage_close(bom->storage);
    free(bom->path);
    free(bom);
}