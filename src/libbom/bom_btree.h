#ifndef BOM_BTREE_H
#define BOM_BTREE_H

#include <stddef.h>
#include <stdint.h>

/* Multi-leaf Paths-tree layout planner.
 *
 * Reproduces Apple's BOM Paths B-tree page layout: rows keyed by
 * (parent_path_id, name) are inserted in a given order into leaves of
 * capacity BM_BTREE_LEAF_CAP; a leaf that would overflow (511 rows) is
 * split at (count+1)/2, and the new right leaf links into the fwd/back
 * chain immediately after the split leaf.  The tree then spans multiple
 * leaves reached through one interior node (children in chain order with a
 * "greatest file block" per subtree), exactly as observed in Apple's files
 * (see tools/prototype/btsim7.py).
 */

#define BM_BTREE_LEAF_CAP 510

/* A row key as stored in the Paths tree. */
typedef struct btree_key {
    uint32_t    parent;    /* pid of the enclosing directory (0 = none) */
    const char *name;      /* leaf name ("." for the root row) */
} btree_key;

/* Leaf partition: leaves have creation ids 0..nleaf-1 (0 is the initial
 * leaf, later ids are created by splits).  Each leaf owns a contiguous run
 * of SORTED ranks [start, start+count), where rank r addresses
 * plan->sorted[r]. */
typedef struct btree_leaf {
    size_t      start;
    size_t      count;
} btree_leaf;

/* One split event: inserting the iord[splits[e].insert]'th key overflowed a
 * leaf, creating newleaf.  Events are in insertion order; the first always
 * has newleaf == 1 and is the point where the (single) interior node is
 * allocated. */
typedef struct btree_split {
    uint32_t    insert;    /* 0-based position in the insertion order */
    uint32_t    newleaf;   /* creation id of the leaf created */
} btree_split;

typedef struct btree_plan {
    size_t          nrows;
    size_t          nleaf;      /* number of leaves (creation ids 0..nleaf-1) */
    uint32_t       *chain;      /* nleaf creation ids in chain (key) order */
    btree_leaf     *leaf;       /* per creation id (nleaf) */
    uint32_t       *leaf_of;    /* per sorted rank (nrows): owning creation id */
    uint32_t       *sorted;     /* rank -> index into the caller's key array */
    btree_split    *splits;     /* nsplits events in insertion order */
    size_t          nsplits;
} btree_plan;

/* Compute the split layout for nrows keys inserted in the order given by
 * insertion_order (nrows indexes into keys; pass NULL for identity, which
 * means keys[] itself is already in insertion order).  On success *out holds
 * freshly allocated arrays released with bom_btree_plan_free.
 * Returns 0 or -1. */
int bom_btree_plan(const btree_key *keys, size_t nrows,
                   const uint32_t *insertion_order, btree_plan *out);

void bom_btree_plan_free(btree_plan *p);

#endif /* BOM_BTREE_H */