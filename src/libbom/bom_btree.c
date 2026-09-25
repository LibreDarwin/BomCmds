/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * Split-aware multi-leaf Paths-tree layout planner (see bom_btree.h). */
#include "bom_btree.h"

#include <stdlib.h>
#include <string.h>

typedef struct btree_keyref {
    uint32_t    idx;       /* index into the caller's keys[] */
    uint32_t    parent;
    const char *name;
} btree_keyref;

static int cmp_keyref(const void *a, const void *b) {
    const btree_keyref *x = (const btree_keyref *)a;
    const btree_keyref *y = (const btree_keyref *)b;
    if (x->parent != y->parent)
        return x->parent < y->parent ? -1 : 1;
    return strcmp(x->name, y->name);
}

/* In-progress leaf: sorted-rank values, ascending.  Leaf keys are inserted
 * in their final sorted position, so the rank array stays sorted. */
typedef struct btree_leafsim {
    uint32_t *ranks;
    size_t    count;
    size_t    cap;
} btree_leafsim;

static int leafsim_push_sorted(btree_leafsim *L, uint32_t r) {
    uint32_t *nr;
    size_t pos;
    if (L->count == L->cap) {
        size_t ncap = L->cap ? L->cap * 2 : 16;
        nr = (uint32_t *)realloc(L->ranks, ncap * sizeof(uint32_t));
        if (nr == NULL)
            return -1;
        L->ranks = nr;
        L->cap = ncap;
    }
    /* upper bound: first rank > r */
    pos = L->count;
    while (pos > 0 && L->ranks[pos - 1] > r)
        pos--;
    memmove(&L->ranks[pos + 1], &L->ranks[pos],
            (L->count - pos) * sizeof(uint32_t));
    L->ranks[pos] = r;
    L->count++;
    return 0;
}

int bom_btree_plan(const btree_key *keys, size_t nrows,
                   const uint32_t *insertion_order, btree_plan *out) {
    btree_keyref *keyref = NULL;
    uint32_t *rinv = NULL;
    btree_leafsim *leaves = NULL;
    uint32_t *chain = NULL;
    btree_split *splits = NULL;
    uint32_t *sorted = NULL, *leaf_of = NULL;
    btree_leaf *leaf = NULL;
    size_t n = nrows;
    size_t nleaf = 1;
    size_t nsplits = 0;
    size_t space = n ? n : 1;
    size_t i, j;
    int rc = -1;

    if (out == NULL || (n != 0 && keys == NULL))
        return -1;
    memset(out, 0, sizeof(*out));

    keyref = (btree_keyref *)malloc(space * sizeof(btree_keyref));
    rinv = (uint32_t *)malloc(space * sizeof(uint32_t));
    leaves = (btree_leafsim *)calloc(space, sizeof(btree_leafsim));
    chain = (uint32_t *)malloc(space * sizeof(uint32_t));
    splits = (btree_split *)malloc(space * sizeof(btree_split));
    sorted = (uint32_t *)malloc(space * sizeof(uint32_t));
    leaf_of = (uint32_t *)malloc(space * sizeof(uint32_t));
    leaf = (btree_leaf *)malloc(space * sizeof(btree_leaf));
    if (keyref == NULL || rinv == NULL || leaves == NULL || chain == NULL ||
        splits == NULL || sorted == NULL || leaf_of == NULL || leaf == NULL)
        goto out;

    for (i = 0; i < n; i++) {
        keyref[i].idx = (uint32_t)i;
        keyref[i].parent = keys[i].parent;
        keyref[i].name = keys[i].name;
    }
    qsort(keyref, n, sizeof(btree_keyref), cmp_keyref);
    for (i = 0; i < n; i++) {
        sorted[i] = keyref[i].idx;
        rinv[keyref[i].idx] = (uint32_t)i;
    }

    chain[0] = 0;
    for (j = 0; j < n; j++) {
        uint32_t idx = insertion_order ? insertion_order[j] : (uint32_t)j;
        uint32_t r = rinv[idx];
        size_t li;
        for (li = 0; li < nleaf; li++) {
            uint32_t cid = chain[li];
            size_t cnt = leaves[cid].count;
            if (cnt == 0 || leaves[cid].ranks[cnt - 1] >= r)
                break;
        }
        if (li == nleaf)
            li = nleaf - 1;
        {
            uint32_t cid = chain[li];
            btree_leafsim *L = &leaves[cid];
            if (leafsim_push_sorted(L, r) != 0)
                goto out;
            if (L->count > BM_BTREE_LEAF_CAP) {
                size_t mid = (L->count + 1) / 2;
                size_t nright = L->count - mid;
                uint32_t cid_new = (uint32_t)nleaf;
                btree_leafsim *R;
                size_t k;
                nleaf++;
                R = &leaves[cid_new];
                if (R->cap < nright) {
                    size_t ncap = nright;
                    uint32_t *nr =
                        (uint32_t *)realloc(R->ranks, ncap * sizeof(uint32_t));
                    if (nr == NULL)
                        goto out;
                    R->ranks = nr;
                    R->cap = ncap;
                }
                memcpy(R->ranks, &L->ranks[mid], nright * sizeof(uint32_t));
                R->count = nright;
                L->count = mid;
                for (k = nleaf - 1; k > (li + 1); k--)
                    chain[k] = chain[k - 1];
                chain[li + 1] = cid_new;
                splits[nsplits].insert = (uint32_t)j;
                splits[nsplits].newleaf = cid_new;
                nsplits++;
            }
        }
    }

    /* Final partitions in creation-id order. */
    for (i = 0; i < nleaf; i++) {
        leaf[i].start = leaves[i].count ? leaves[i].ranks[0] : 0;
        leaf[i].count = leaves[i].count;
    }
    /* leaf_of per sorted rank, walking the chain in key order. */
    for (i = 0; i < nleaf; i++) {
        uint32_t cid = chain[i];
        size_t start = leaf[cid].start;
        size_t count = leaf[cid].count;
        size_t k;
        for (k = 0; k < count; k++)
            leaf_of[start + k] = cid;
    }

    if (nleaf < 1 || (nsplits != nleaf - 1))
        goto out;

    {
        uint32_t *tc = (uint32_t *)realloc(chain,
                                           (nleaf ? nleaf : 1) *
                                               sizeof(uint32_t));
        btree_leaf *tl = (btree_leaf *)realloc(leaf,
                                               (nleaf ? nleaf : 1) *
                                                   sizeof(btree_leaf));
        btree_split *ts = (btree_split *)realloc(splits,
                                                 (nsplits ? nsplits : 1) *
                                                     sizeof(btree_split));
        if (tc) chain = tc;
        if (tl) leaf = tl;
        if (ts) splits = ts;
    }
    out->nrows = n;
    out->nleaf = nleaf;
    out->chain = chain;
    chain = NULL;
    out->leaf = leaf;
    leaf = NULL;
    out->leaf_of = leaf_of;
    leaf_of = NULL;
    out->sorted = sorted;
    sorted = NULL;
    out->splits = splits;
    splits = NULL;
    out->nsplits = nsplits;
    rc = 0;

out:
    free(keyref);
    free(rinv);
    for (i = 0; i < (leaves ? space : 0); i++)
        free(leaves[i].ranks);
    free(leaves);
    free(chain);
    free(splits);
    free(sorted);
    free(leaf_of);
    free(leaf);
    if (rc != 0)
        return -1;
    return 0;
}

void bom_btree_plan_free(btree_plan *p) {
    if (p == NULL)
        return;
    free(p->chain);
    free(p->leaf);
    free(p->leaf_of);
    free(p->sorted);
    free(p->splits);
    memset(p, 0, sizeof(*p));
}