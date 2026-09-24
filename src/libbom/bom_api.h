/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * Internal state shared by the clean-room BOM.framework API modules
 * (BOMStorage/Tree/FSObject/Bom/Copier).  The public ABI is BOM.h; these
 * struct layouts are private to the framework and never exposed. */
#ifndef LIBBOM_BOM_API_H
#define LIBBOM_BOM_API_H

#include "bom_read.h"
#include "BOM.h"
#include <stddef.h>
#include <stdint.h>

/* ---- BOMStorage: the BOMStore container ---- */

struct BOMStorage {
    bom_file bf;         /* read image (owned once open) */
    char    *path;       /* strdup'd open path, or NULL */
    int      open;       /* 1 = image loaded */
    int      for_write;  /* 1 = created via NewWithSys (commit-on-free) */

    /* Variables index (FORMAT.md 3): name -> block, in file order. */
    uint32_t nvar;
    char   **varname;    /* [nvar], NUL-terminated */
    uint8_t *varlen;     /* [nvar], stored name_length */
    uint32_t *varblk;    /* [nvar], block index referenced */
};

/* ---- BOMTree: a Tree variable (FORMAT.md 4.3) ---- */

struct bom_tree_row {
    char       *path;    /* full "./..." path (owned) */
    uint32_t    pid;     /* path id (from PathInfoIndex) */
    uint32_t    parent;  /* parent path id (from File block) */
    uint32_t    prblk;   /* path_record_index (from PathInfoIndex) */
    uint8_t     pii[8];  /* raw PathInfoIndex bytes (iterator value) */
    bom_pathrec pr;      /* decoded PathRecord */
    char       *leaf;    /* leaf name bytes (owned) */
    uint16_t    name_len;/* leaf name length */
};

struct BOMTree {
    BOMStorage *storage;  /* borrowed; not owned */
    char       *name;     /* variable name, strdup'd */
    uint32_t    root;     /* tree root block index */
    uint32_t    bpi;      /* block_paths_index (first Paths block) */
    uint32_t    bsize;    /* block_size */
    uint32_t    count;    /* path_count */
    struct bom_tree_row *rows; /* decoded rows, storage order */
    uint32_t    nrows;    /* rows actually decoded */
};

struct BOMTreeIterator {
    BOMTree *tree;        /* borrowed */
    uint32_t i;           /* next row index */
};

/* ---- BOMFSObject: filesystem-object node ---- */

/* Internal helpers shared by the framework modules.  They carry hidden
 * visibility: the drop-in dylib must not export anything beyond the 56-symbol
 * union in plus BOM.h's `BOM*` API. */
#if defined(__GNUC__)
#define LIBBOM_HIDDEN __attribute__((visibility("hidden")))
#else
#define LIBBOM_HIDDEN
#endif

struct BOMFSObject {
    uint32_t type;       /* path_type (BOM_TYPE_*) */
    uint32_t mode;       /* st_mode incl. S_IFMT */
    uint32_t uid, gid;
    uint32_t mtime;
    uint32_t size;
    uint32_t checksum;
    char    *pathname;   /* full "./..." path (owned) */
    char    *shortname;  /* leaf name (owned) */
    char    *link;       /* symlink target (owned) */
    size_t   link_len;
    uint32_t nslice;
    uint8_t *slices;     /* owned; 16-byte {cpu,sub,size,cksum} BE entries */
    uint8_t *opaque;     /* owned; raw PathRecord block bytes */
    size_t   opaque_len;
    uint32_t flags;      /* B_ALLDATA / B_PATHONLY */
    int      own_path;   /* 1 = pathname owned */
    int      own_short;  /* 1 = shortname owned */
    int      binary;     /* 1 = carries a Mach-O slice table */
};

/* Build an FSObject tree node from a decoded PathRecord plus full path and
 * leaf name (borrowed); copies its own state. */
LIBBOM_HIDDEN BOMFSObject *_bom_fso_from_row(const struct bom_tree_row *row);

/* Fill an FSObject's metadata + slices + link from a decoded record. */
LIBBOM_HIDDEN void _bom_fso_from_pathrec(struct BOMFSObject *o,
                                         const bom_pathrec *pr);

LIBBOM_HIDDEN uint32_t _bom_r32(const uint8_t *p);

/* Open a file as a BOMStorage (read side); returns NULL on failure. */
LIBBOM_HIDDEN BOMStorage *_bom_storage_open(const char *path);

LIBBOM_HIDDEN void _bom_storage_close(BOMStorage *s);

/* Parse the vars index (header 0x18 = offset, 0x1c = length; FORMAT.md 3). */
LIBBOM_HIDDEN void _bom_storage_load_vars(BOMStorage *s);

/* Look up a variable name; returns its block index or 0. */
LIBBOM_HIDDEN uint32_t _bom_storage_var_block(const BOMStorage *s,
                                              const char *name,
                                              size_t namelen);

/* ---- BOMBom: a bom file object ---- */

/* One row of a NewWithSys tree that has not been committed yet: the raw
 * PathRecord bytes to emit plus the path bookkeeping.  `pid`/`parent` are
 * the ORIGINAL path ids the row is holding (never reused, so removal leaves
 * no dangling references); bom_reencode renumbers from them at commit. */
struct bom_pend_row {
    char       *path;      /* full "./..." path (owned) */
    char       *leaf;      /* leaf name (owned); root row: "." */
    uint8_t    *pr;        /* raw PathRecord bytes (owned) */
    size_t      prlen;
    uint32_t    pid;
    uint32_t    parent;
};

struct BOMBom {
    char       *path;      /* file path (owned) */
    BOMStorage *storage;   /* read image (owned) */
    BOMTree    *paths;     /* "Paths" tree (owned) */
    int         open;      /* 1 = readable image loaded */
    int         for_write; /* 1 = created via NewWithSys */

    /* Pending mutation rows (write-on-free).  Parent rows always precede
     * children, so commit hands bom_reencode a parent-before-child set. */
    struct bom_pend_row *pend;   /* owned array */
    size_t            npend;     /* rows in use */
    size_t            pendcap;   /* allocated */
    uint32_t          next_pid;  /* monotonic pid allocator (root = 1) */
    int               committed; /* 1 = file already emitted on free */
};

struct BOMBomEnumerator {
    BOMBom   *bom;         /* borrowed */
    uint32_t  parent;      /* path id whose children are enumerated */
    uint32_t  i;           /* next candidate row index */
};

/* ---- BOMCopier ---- */

struct BOMCopier {
    BOMCopierFatalErrorHandler       fatal;
    BOMCopierFatalFileErrorHandler   fatal_file;
    BOMCopierFileErrorHandler        file_err;
    BOMCopierPKZipPasswordRequester  passwd;
    BOMCopierCopyFileStartedHandler  started;
    BOMCopierCopyFileFinishedHandler finished;
    BOMCopierCopyFileUpdateHandler   update;
};

#endif /* LIBBOM_BOM_API_H */