/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * Directory walker + hard-link group detection for mkbom dir mode. */
#include "fs_walk.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static uint8_t classify(mode_t m) {
    switch (m & S_IFMT) {
    case S_IFREG:
        return BM_TYPE_REG;
    case S_IFDIR:
        return BM_TYPE_DIR;
    case S_IFLNK:
        return BM_TYPE_LNK;
    default:
        return BM_TYPE_SPC;
    }
}

static int paths_grow(bm_path **a, size_t *cap) {
    size_t nc = (*cap ? *cap * 2 : 256);
    bm_path *np = (bm_path *)realloc(*a, nc * sizeof(bm_path));
    if (np == NULL)
        return -1;
    *a = np;
    *cap = nc;
    return 0;
}

static void scan_dir(const char *path, uint32_t parent_pid,
                     bm_path **paths, size_t *n, size_t *cap) {
    DIR *dp = opendir(path);
    if (dp == NULL)
        return; /* unreadable subtree: skipped, like mkbom's walk */
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        const char *nm = de->d_name;
        if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
            continue;
        size_t pl = strlen(path), nl = strlen(nm);
        char *full = (char *)malloc(pl + 1 + nl + 1);
        if (full == NULL)
            continue;
        memcpy(full, path, pl);
        full[pl] = '/';
        memcpy(full + pl + 1, nm, nl + 1);

        struct stat st;
        if (lstat(full, &st) != 0) {
            free(full);
            continue;
        }
        if (*n == *cap && paths_grow(paths, cap) != 0) {
            free(full);
            continue;
        }
        bm_path *node = &(*paths)[*n];
        node->pid = (uint32_t)(*n) + 1;
        node->parent = parent_pid;
        node->path = full;
        node->name = strdup(nm);
        if (node->name == NULL) {
            free(full);
            continue;
        }
        node->st = st;
        node->type = classify(st.st_mode);
        node->group = -1;
        node->rank = 0;
        node->cksum = 0;
        node->link = NULL;
        (*n)++;
        if (node->type == BM_TYPE_DIR)
            scan_dir(full, node->pid, paths, n, cap);
    }
    closedir(dp);
}

/* ---- hard-link group detection: hash table over (st_dev, st_ino) ---- */

typedef struct ino_rec {
    dev_t     dev;
    ino_t     ino;
    uint32_t  first_pid;
    uint32_t *members; /* pids in encounter order */
    size_t    nmem;
    size_t    cmem;
} ino_rec;

typedef struct ino_tab {
    struct slot {
        uint64_t key;
        int      rec;
    } *slots;
    size_t  nslots;
    size_t  used;
    ino_rec *recs;
    size_t  nrecs;
    size_t  crecs;
} ino_tab;

static uint64_t ino_hash(dev_t dev, ino_t ino) {
    uint64_t h = 14695981039346656037ULL;
    h = (h ^ (uint64_t)(uint32_t)dev) * 1099511628211ULL;
    h = (h ^ (uint64_t)ino) * 1099511628211ULL;
    return h;
}

static int tab_resize(ino_tab *t) {
    size_t ns = t->nslots ? t->nslots * 2 : 64;
    struct slot *sl = (struct slot *)calloc(ns, sizeof(*sl));
    if (sl == NULL)
        return -1;
    size_t i;
    for (i = 0; i < t->nslots; i++) {
        if (!t->slots[i].rec)
            continue;
        size_t j = t->slots[i].key & (ns - 1);
        while (sl[j].rec)
            j = (j + 1) & (ns - 1);
        sl[j] = t->slots[i];
    }
    free(t->slots);
    t->slots = sl;
    t->nslots = ns;
    return 0;
}

static int tab_add(ino_tab *t, dev_t dev, ino_t ino, uint32_t pid,
                   int *recidx) {
    uint64_t h = ino_hash(dev, ino);
    if (t->used * 2 >= t->nslots && tab_resize(t) != 0)
        return -1;
    size_t j = h & (t->nslots - 1);
    while (t->slots[j].rec) {
        ino_rec *r = &t->recs[t->slots[j].rec - 1];
        if (r->dev == dev && r->ino == ino) {
            if (r->nmem == r->cmem) {
                size_t nc = r->cmem ? r->cmem * 2 : 4;
                uint32_t *np =
                    (uint32_t *)realloc(r->members, nc * sizeof(uint32_t));
                if (np == NULL)
                    return -1;
                r->members = np;
                r->cmem = nc;
            }
            r->members[r->nmem++] = pid;
            *recidx = t->slots[j].rec - 1;
            return 0;
        }
        j = (j + 1) & (t->nslots - 1);
    }
    if (t->nrecs == t->crecs) {
        size_t nc = t->crecs ? t->crecs * 2 : 16;
        ino_rec *nr = (ino_rec *)realloc(t->recs, nc * sizeof(ino_rec));
        if (nr == NULL)
            return -1;
        t->recs = nr;
        t->crecs = nc;
    }
    size_t ri = t->nrecs++;
    t->recs[ri].dev = dev;
    t->recs[ri].ino = ino;
    t->recs[ri].first_pid = pid;
    t->recs[ri].members = NULL;
    t->recs[ri].nmem = 0;
    t->recs[ri].cmem = 0;
    t->slots[j].key = h;
    t->slots[j].rec = (int)ri + 1;
    t->used++;
    *recidx = (int)ri;
    return 0;
}

static void tab_free(ino_tab *t) {
    size_t i;
    for (i = 0; i < t->nrecs; i++)
        free(t->recs[i].members);
    free(t->recs);
    free(t->slots);
}

static void bm_walk_free_parts(bm_path *paths, size_t n, bm_group *groups,
                               size_t ng) {
    size_t i;
    for (i = 0; i < n; i++) {
        free(paths[i].path);
        free(paths[i].name);
        free(paths[i].link);
    }
    free(paths);
    for (i = 0; i < ng; i++)
        free(groups[i].members);
    free(groups);
}

/* ---- public API ---- */

static void bm_walk_free_parts(bm_path *paths, size_t n, bm_group *groups,
                               size_t ng);

int bm_scan(const char *root, bm_walk *out) {
    memset(out, 0, sizeof(*out));
    struct stat st;
    if (lstat(root, &st) != 0)
        return -1;
    if ((st.st_mode & S_IFMT) != S_IFDIR) {
        errno = ENOTDIR;
        return -1;
    }

    size_t n = 0, cap = 0;
    bm_path *paths = NULL;
    if (paths_grow(&paths, &cap) != 0)
        return -1;
    bm_path *r = &paths[0];
    r->pid = 1;
    r->parent = 0;
    r->path = strdup(root);
    r->name = strdup(".");
    if (r->path == NULL || r->name == NULL) {
        free(r->path);
        free(r->name);
        free(paths);
        return -1;
    }
    r->st = st;
    r->type = BM_TYPE_DIR;
    r->group = -1;
    r->rank = 0;
    r->cksum = 0;
    r->link = NULL;
    n = 1;
    scan_dir(root, 1, &paths, &n, &cap);

    ino_tab tab = {0};
    size_t i;
    for (i = 0; i < n; i++) {
        const bm_path *p = &paths[i];
        if (p->type != BM_TYPE_REG && p->type != BM_TYPE_LNK)
            continue;
        int ri;
        tab_add(&tab, p->st.st_dev, p->st.st_ino, p->pid, &ri);
    }

    bm_group *groups = NULL;
    size_t ng = 0, capg = 0;
    for (i = 0; i < tab.nrecs; i++) {
        ino_rec *rec = &tab.recs[i];
        if (rec->nmem < 2)
            continue;
        if (ng == capg) {
            size_t nc = capg ? capg * 2 : 8;
            bm_group *np = (bm_group *)realloc(groups, nc * sizeof(bm_group));
            if (np == NULL)
                goto oom;
            groups = np;
            capg = nc;
        }
        bm_group *g = &groups[ng];
        g->first_pid = rec->first_pid;
        g->nmem = rec->nmem;
        g->members =
            (uint32_t *)malloc(rec->nmem * sizeof(uint32_t));
        if (g->members == NULL)
            goto oom;
        memcpy(g->members, rec->members, rec->nmem * sizeof(uint32_t));
        size_t k;
        for (k = 0; k < g->nmem; k++) {
            bm_path *mp = &paths[g->members[k] - 1];
            mp->group = (int)ng;
            mp->rank = (int)k + 1;
        }
        ng++;
    }
    tab_free(&tab);
    out->paths = paths;
    out->npaths = n;
    out->groups = groups;
    out->ngroups = ng;
    out->mode = BM_MODE_DIR;
    return 0;

oom:
    tab_free(&tab);
    bm_walk_free_parts(paths, n, groups, ng);
    return -1;
}

void bm_walk_free(bm_walk *w) {
    bm_walk_free_parts(w->paths, w->npaths, w->groups, w->ngroups);
    memset(w, 0, sizeof(*w));
}

char *bm_relpath(const bm_walk *w, uint32_t pid) {
    size_t cap = 8, n = 0;
    const char **names = (const char **)malloc(cap * sizeof(char *));
    if (names == NULL)
        return NULL;
    uint32_t cur = pid;
    size_t len = 2; /* "./" */
    while (cur > 1) {
        const bm_path *p = &w->paths[cur - 1];
        if (n == cap) {
            cap *= 2;
            const char **nn =
                (const char **)realloc(names, cap * sizeof(char *));
            if (nn == NULL) {
                free(names);
                return NULL;
            }
            names = nn;
        }
        names[n++] = p->name;
        len += strlen(p->name) + 1;
        cur = p->parent;
    }
    if (n > 0)
        len -= 1; /* no trailing slash */
    char *out = (char *)malloc(len + 1);
    if (out == NULL) {
        free(names);
        return NULL;
    }
    char *ptr = out;
    *ptr++ = '.';
    *ptr++ = '/';
    size_t i;
    for (i = 0; i < n; i++) {
        if (i > 0)
            *ptr++ = '/';
        size_t l = strlen(names[i]);
        memcpy(ptr, names[i], l);
        ptr += l;
    }
    *ptr = '\0';
    free(names);
    return out;
}