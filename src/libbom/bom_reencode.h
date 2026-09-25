#ifndef BOM_REENCODE_H
#define BOM_REENCODE_H

#include <stddef.h>
#include <stdint.h>

#include "bom_read.h"
#include "bom_api.h"

/* Clean-room records-based BOM relocator.  Rebuilds a BOMStore file from a
 * decoded set of tree rows (no filesystem access): rows keep their ORIGINAL
 * (possibly sparse) pids so the (parent,name)-sorted key order the tree's
 * interior nodes assume is preserved, every row gets its own (PathRecord,
 * File, PathInfoIndex) triplet (no hard-link group trailers), and block 1
 * BomInfo is recomputed from the records' own slice tables.  The emitted
 * layout is the ngroups=0 shape of bm_write_bom (fixed blocks 1-10, triplets
 * from block 11), so the result is readable by our reader and lsbom output
 * matches the rows that were passed in. */

/* One row to re-emit.  `pid`/`parent` are the ORIGINAL path ids from the
 * source tree and must be preserved verbatim; the re-encoder never
 * renumbers them (Apple's own archives use sparse ids, and renumbering here
 * would break the (parent,name) key sort).  `pr` points at the raw
 * PathRecord bytes to emit (owned by the caller), `name` is the leaf name
 * (root row: "."). */
typedef struct bom_reenc_row {
    uint32_t      pid;
    uint32_t      parent;
    const char   *name;
    const uint8_t *pr;
    size_t        prlen;
} bom_reenc_row;

/* Rebuilds the exact PathRecord bytes a bm_write_bom walk would store for a
 * decoded record (mirrors build_pr's byte layout, including the Mach-O
 * slice-table variant).  Caller frees *out.  Returns 0 or -1. */
LIBBOM_HIDDEN int bom_pr_rebuild(const bom_pathrec *pr, uint8_t **out,
                                 size_t *outlen);

/* Emit `out_path` from `nrows` rows.  Rows must be supplied parent-before-
 * child in (parent,name)-sorted key order (the source tree's storage order
 * satisfies this after subtree pruning; BOMBomFree's pend_cmp qsort also
 * produces it).  Returns 0 on success, -1 on error with a message in
 * errbuf. */
LIBBOM_HIDDEN int bom_reencode(const bom_reenc_row *rows, size_t nrows,
                               const char *out_path, char *errbuf,
                               size_t errbufsz);

#endif /* BOM_REENCODE_H */