/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * mkbom: build a bill of materials for a directory tree, optionally as a
 * path-only bom (-s), or from an lsbom(8)-format file listing (-i).
 * Clean-room reimplementation, byte-identical to Apple's mkbom. */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libbom/bom_writer.h"
#include "libbom/fs_walk.h"

static void usage(void) {
    fprintf(stderr,
            "Usage: mkbom [-h] [-s] directory bom\n"
            "       mkbom [-s] -i filelist bom\n"
            "\n"
            "    -h              print full usage\n"
            "    -s              create a path-only bom\n"
            "    -i filelist     a file listing in lsbom(8) format used to "
            "create the bom file\n");
}

/* ---- file-list walk building (mkbom -i) ---- */

/* Tree table: key -> pid.  Keys are relpaths without the leading "./"
 * (the root is the key ".").  Pids are assigned in listing order. */
typedef struct {
    char   *key;
    uint32_t pid;
} path_row;

static uint32_t tab_find(const path_row *tab, size_t n, const char *key) {
    size_t i;
    for (i = 0; i < n; i++)
        if (strcmp(tab[i].key, key) == 0)
            return tab[i].pid;
    return 0;
}

/* Key for a listing path: "." for the root, else the path with a leading
 * "./" stripped.  Absolute paths are kept as-is.  Caller frees. */
static char *path_key(const char *rel) {
    if (strcmp(rel, ".") == 0 || strcmp(rel, "./") == 0)
        return strdup(".");
    if (strncmp(rel, "./", 2) == 0)
        return strdup(rel + 2);
    return strdup(rel);
}

/* Key of the parent directory, or NULL if rel names the root itself.
 * A top-level entry's parent is the root key ".".  Caller frees. */
static char *parent_key(const char *rel) {
    char *k, *slash;
    if (strcmp(rel, ".") == 0 || strcmp(rel, "./") == 0)
        return NULL;
    k = path_key(rel);
    slash = strrchr(k, '/');
    if (slash == NULL) { /* top level: parent is the root */
        free(k);
        return strdup(".");
    }
    *slash = '\0';
    if (*k == '\0') {
        free(k);
        return strdup(".");
    }
    return k;
}

/* Parent path as Apple prints it in "parent directory %s does not exist". */
static void parent_disp(char *buf, size_t sz, const char *parent_key_) {
    if (strcmp(parent_key_, ".") == 0)
        snprintf(buf, sz, ".");
    else
        snprintf(buf, sz, "./%s", parent_key_);
}

static char *leaf_of(const char *key) {
    const char *s;
    if (strcmp(key, ".") == 0)
        return strdup(".");
    s = strrchr(key, '/');
    return strdup(s != NULL ? s + 1 : key);
}

static int all_decimal(const char *p) {
    if (*p == '\0')
        return 0;
    for (; *p; p++)
        if (!isdigit((unsigned char)*p))
            return 0;
    return 1;
}

static int all_octal(const char *p) {
    if (*p == '\0')
        return 0;
    for (; *p; p++)
        if (*p < '0' || *p > '7')
            return 0;
    return 1;
}

/* uid/gid field: "uid" or "uid/gid" (both decimal). */
static int uidgid_ok(const char *p) {
    if (*p == '\0')
        return 0;
    for (; *p && *p != '/'; p++)
        if (!isdigit((unsigned char)*p))
            return 0;
    if (*p == '/') {
        const char *g = ++p;
        if (*g == '\0')
            return 0;
        for (; *g; g++)
            if (!isdigit((unsigned char)*g))
                return 0;
    }
    return 1;
}

typedef struct {
    uint8_t   type; /* BM_TYPE_* */
    mode_t    mode; /* full mode incl. type bits (octal) */
    uint32_t  uid, gid;
    off_t     size;
    uint32_t  cksum;
    char     *link; /* symlink target; NULL for non-symlinks */
} listing_ent;

/* Parse one lsbom(8) summary line into ent.  Returns 0 and fills *ent on
 * success; on failure prints the matching Apple diagnostic and returns -1. */
static int parse_line(listing_ent *ent, const char *line) {
    char *copy, *tok[8];
    size_t ntok = 0;
    char *p;
    char *end;

    copy = strdup(line);
    if (copy == NULL)
        return -1;
    if ((p = strchr(copy, '\n')) != NULL)
        *p = '\0';
    if ((p = strchr(copy, '\r')) != NULL)
        *p = '\0';

    p = copy;
    while (ntok < 8) {
        tok[ntok++] = p;
        p = strchr(p, '\t');
        if (p == NULL)
            break;
        *p++ = '\0';
        if (*p == '\0') { /* trailing tab: empty field */
            ntok++;
            break;
        }
    }

    {   /* "., 40755 ..." has no tab: bug in lsbom-default handling? no: a
         * malformed (tab-free) line. */
        int empty = 0;
        size_t i;
        for (i = 0; i < ntok; i++)
            if (tok[i][0] == '\0')
                empty = 1;
        if (empty) {
            fprintf(stderr, "mkbom: Empty field in summary\n");
            fprintf(stderr, "mkbom: Can't insert (null)\n");
            free(copy);
            return -1;
        }
    }

    if (ntok < 3) {
        fprintf(stderr, "mkbom: Not enough fields\n");
        fprintf(stderr, "mkbom: Can't insert (null)\n");
        free(copy);
        return -1;
    }

    /* dir lines carry exactly path, mode, uid/gid. */
    if (!all_octal(tok[1])) {
        fprintf(stderr, "mkbom: Improperly formatted field\n");
        fprintf(stderr, "mkbom: Can't insert (null)\n");
        free(copy);
        return -1;
    }
    mode_t mode = (mode_t)strtoul(tok[1], NULL, 8);

    /* uid/gid: "uid" or "uid/gid" */
    {
        const char *u = tok[2];
        if (!uidgid_ok(u)) {
            fprintf(stderr, "mkbom: Improperly formatted field\n");
            fprintf(stderr, "mkbom: Can't insert (null)\n");
            free(copy);
            return -1;
        }
    }

    memset(ent, 0, sizeof(*ent));
    ent->mode = mode;
    ent->uid = (uint32_t)strtoul(tok[2], NULL, 10);
    end = strchr(tok[2], '/');
    ent->gid = (end != NULL) ? (uint32_t)strtoul(end + 1, NULL, 10) : 0;

    switch (mode & S_IFMT) {
    case S_IFDIR:
        ent->type = BM_TYPE_DIR;
        if (ntok > 3) {
            fprintf(stderr, "mkbom: Improperly formatted field\n");
            fprintf(stderr, "mkbom: Can't insert (null)\n");
            free(copy);
            return -1;
        }
        break;
    case S_IFREG:
        ent->type = BM_TYPE_REG;
        goto needs_data;
    case S_IFLNK:
        ent->type = BM_TYPE_LNK;
        goto needs_data;
    default:
        fprintf(stderr, "mkbom: Improperly formatted field\n");
        fprintf(stderr, "mkbom: Can't insert (null)\n");
        free(copy);
        return -1;
    }
    goto have_all;

needs_data:
    if (ntok < 5 || ntok > 6) {
        fprintf(stderr, "mkbom: Improperly formatted field\n");
        fprintf(stderr, "mkbom: Can't insert (null)\n");
        free(copy);
        return -1;
    }
    if (!all_decimal(tok[3]) || !all_decimal(tok[4])) {
        fprintf(stderr, "mkbom: Improperly formatted field\n");
        fprintf(stderr, "mkbom: Can't insert (null)\n");
        free(copy);
        return -1;
    }
    ent->size = (off_t)strtoll(tok[3], NULL, 10);
    ent->cksum = (uint32_t)(strtoull(tok[4], NULL, 10) & 0xffffffffu);
    if (ntok == 6) {
        if (ent->type != BM_TYPE_LNK) {
            fprintf(stderr, "mkbom: Improperly formatted field\n");
            fprintf(stderr, "mkbom: Can't insert (null)\n");
            free(copy);
            return -1;
        }
        ent->link = strdup(tok[5]);
        if (ent->link == NULL) {
            free(copy);
            return -1;
        }
    } else if (ent->type == BM_TYPE_LNK) {
        ent->link = strdup("");
        if (ent->link == NULL) {
            free(copy);
            return -1;
        }
    }

have_all:
    free(copy);
    return 0;
}

/* Append a path entry to the walk and record its key in the pid table. */
static int walk_insert(bm_path **paths, size_t *n, size_t *cap,
                       path_row **tab, size_t *nt, size_t *ct,
                       const char *raw, const listing_ent *ent) {
    char *key = path_key(raw);
    char *pkey = parent_key(raw);
    char *name;
    uint32_t parent = 0;

    if (key == NULL || (pkey == NULL && strcmp(key, ".") != 0)) {
        free(key);
        free(pkey);
        return -1;
    }

    if (pkey != NULL) {
        parent = tab_find(*tab, *nt, pkey);
        if (parent == 0) {
            char dbuf[512];
            parent_disp(dbuf, sizeof(dbuf), pkey);
            fprintf(stderr, "parent directory %s does not exist\n", dbuf);
            fprintf(stderr, "mkbom: Can't insert %s\n", raw);
            free(key);
            free(pkey);
            return 1; /* line rejected (not a hard error) */
        }
    }

    name = leaf_of(key);
    if (name == NULL) {
        free(key);
        free(pkey);
        return -1;
    }

    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 256;
        bm_path *np = (bm_path *)realloc(*paths, nc * sizeof(bm_path));
        if (np == NULL) {
            free(key);
            free(pkey);
            free(name);
            return -1;
        }
        *paths = np;
        *cap = nc;
    }
    if (*nt == *ct) {
        size_t nc = *ct ? *ct * 2 : 64;
        path_row *ntab = (path_row *)realloc(*tab, nc * sizeof(path_row));
        if (ntab == NULL) {
            free(key);
            free(pkey);
            free(name);
            return -1;
        }
        *tab = ntab;
        *ct = nc;
    }

    bm_path *node = &(*paths)[*n];
    memset(node, 0, sizeof(*node));
    node->pid = (uint32_t)(*n) + 1;
    node->parent = parent;
    node->path = strdup(raw);
    node->name = name;
    node->st.st_mode = ent->mode;
    node->st.st_uid = ent->uid;
    node->st.st_gid = ent->gid;
    node->st.st_size = ent->size;
    node->type = ent->type;
    node->group = -1;
    node->rank = 0;
    node->cksum = ent->cksum;
    if (ent->link != NULL) {
        node->link = strdup(ent->link);
        if (node->link == NULL) {
            free(node->path);
            free(key);
            free(pkey);
            return -1;
        }
    } else {
        node->link = NULL;
    }
    if (node->path == NULL) {
        free(node->path);
        free(key);
        free(pkey);
        return -1;
    }

    (*tab)[*nt].key = key;
    (*tab)[*nt].pid = node->pid;
    (*nt)++;
    (*n)++;
    free(pkey);
    return 0;
}

/* Parse a filelist (lsbom(8) summary lines) or, with simplified, a plain
 * path list, into a BM_MODE_FILELIST / BM_MODE_PATHONLY walk. */
static int build_from_list(FILE *fp, int simplified, bm_walk *walk) {
    bm_path *paths = NULL;
    size_t n = 0, cap = 0;
    path_row *tab = NULL;
    size_t nt = 0, ct = 0;
    char *line = NULL;
    size_t lsz = 0;
    size_t ln = 0;
    int rc = -1;

    while (getline(&line, &lsz, fp) >= 0) {
        char *s = line;
        size_t len = strlen(s);
        int newline = (len > 0 && s[len - 1] == '\n');
        ln++;

        if (!newline) { /* Apple: unterminated last line */
            fprintf(stderr, "mkbom: Malformed line %zu has invalid "
                    "information\n", ln);
            continue;
        }
        if (len > 0 && s[len - 1] == '\n')
            s[--len] = '\0';
        if (*s == '\0')
            continue; /* empty line: skipped */

        if (simplified) {
            if (strchr(s, '\t') != NULL || strchr(s, ' ') != NULL) {
                fprintf(stderr, "mkbom: Malformed line %zu has invalid "
                        "information\n", ln);
                continue;
            }
            /* path-only listing: one path per line */
            listing_ent ent;
            memset(&ent, 0, sizeof(ent));
            if (strcmp(s, ".") == 0 || strcmp(s, "./") == 0) {
                ent.type = BM_TYPE_DIR;
                ent.mode = S_IFDIR | 0777;
            } else {
                ent.type = BM_TYPE_REG;
                ent.mode = S_IFREG | 0644;
            }
            int r = walk_insert(&paths, &n, &cap, &tab, &nt, &ct, s, &ent);
            if (r < 0)
                goto out;
            continue;
        }

        listing_ent ent;
        ent.link = NULL;
        if (parse_line(&ent, line) != 0)
            continue; /* diagnostic already printed; line skipped */
        char *tabc = strchr(s, '\t');
        if (tabc != NULL)
            *tabc = '\0'; /* s now holds the raw path field */
        int r = walk_insert(&paths, &n, &cap, &tab, &nt, &ct, s, &ent);
        free(ent.link);
        if (r < 0)
            goto out;
    }

    /* Always root-less when no "." line was inserted: Apple leaves the bom
     * with just the fixed blocks (npaths == 0). */
    walk->paths = paths;
    walk->npaths = n;
    walk->groups = NULL;
    walk->ngroups = 0;
    walk->mode = simplified ? BM_MODE_PATHONLY : BM_MODE_FILELIST;
    rc = 0;

out:
    free(line);
    {
        size_t i;
        for (i = 0; i < nt; i++)
            free(tab[i].key);
    }
    free(tab);
    if (rc != 0) {
        size_t i;
        for (i = 0; i < n; i++) {
            free(paths[i].path);
            free(paths[i].name);
            free(paths[i].link);
        }
        free(paths);
    }
    return rc;
}
int main(int argc, char **argv) {
    int ch;
    int simplified = 0;
    const char *filelist = NULL;

    while ((ch = getopt(argc, argv, "hsi:")) != -1) {
        switch (ch) {
        case 'h':
            usage();
            return 1;
        case 's':
            simplified = 1;
            break;
        case 'i':
            filelist = optarg;
            break;
        default:
            usage();
            return 1;
        }
    }
    argc -= optind;
    argv += optind;

    if (filelist != NULL) {
        /* mkbom [-s] -i filelist bom */
        FILE *fp = fopen(filelist, "r");
        if (fp == NULL) {
            fprintf(stderr, "mkbom: Can't open \"%s\": %s\n", filelist,
                    strerror(errno));
            return 1;
        }
        if (argc != 1) {
            fclose(fp);
            usage();
            return 1;
        }
        bm_walk walk;
        memset(&walk, 0, sizeof(walk));
        if (build_from_list(fp, simplified, &walk) != 0) {
            fclose(fp);
            fprintf(stderr, "mkbom: %s\n", strerror(errno));
            return 1;
        }
        fclose(fp);
        char errbuf[256];
        int rc = bm_write_bom(&walk, argv[0], errbuf, sizeof(errbuf));
        bm_walk_free(&walk);
        if (rc != 0) {
            fprintf(stderr, "mkbom: %s: %s\n", argv[1],
                    errbuf[0] ? errbuf : strerror(errno));
            return 1;
        }
        return 0;
    }

    if (argc != 2) {
        usage();
        return 1;
    }

    bm_walk walk;
    if (bm_scan(argv[0], &walk) != 0) {
        fprintf(stderr, "mkbom: %s: %s\n", argv[0], strerror(errno));
        return 1;
    }
    if (simplified)
        walk.mode = BM_MODE_PATHONLY;

    char errbuf[256];
    int rc = bm_write_bom(&walk, argv[1], errbuf, sizeof(errbuf));
    bm_walk_free(&walk);
    if (rc != 0) {
        fprintf(stderr, "mkbom: %s: %s\n", argv[1],
                errbuf[0] ? errbuf : strerror(errno));
        return 1;
    }
    return 0;
}
