#ifndef BOM_FS_WALK_H
#define BOM_FS_WALK_H

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

/* Directory scan in readdir order, DFS pre-order, mirroring mkbom's dir
 * mode: paths get consecutive 1-based ids, parent is the enclosing dir.
 *
 * The walk also carries one of three emission modes that the writer honors:
 *   BM_MODE_DIR (default, from bm_scan): full records with contents checksums
 *     and Mach-O arch tables;
 *   BM_MODE_PATHONLY (mkbom -s): 4-byte typed PathRecords, BomInfo size 0;
 *   BM_MODE_FILELIST (mkbom -i): records built from an lsbom(8) listing
 *     (per-path cksum/link carried in bm_path, mtime 0, arch 3, no groups). */

enum bm_mode {
    BM_MODE_DIR = 0,
    BM_MODE_PATHONLY,
    BM_MODE_FILELIST
};

#define BM_TYPE_REG 1      /* PathRecord path_type 1 */
#define BM_TYPE_DIR 2      /* PathRecord path_type 2 */
#define BM_TYPE_LNK 3      /* PathRecord path_type 3 */
#define BM_TYPE_SPC 0      /* fifo/socket/etc.: id + Paths entry, no record */

typedef struct bm_path {
    uint32_t     pid;
    uint32_t     parent;    /* pid of enclosing directory (0 = none) */
    char        *path;      /* full path, strdup'd */
    char        *name;      /* leaf name, strdup'd */
    struct stat  st;        /* lstat result (filelist mode: from listing) */
    uint8_t      type;      /* BM_TYPE_* */
    int          group;     /* index into bm_walk.groups, or -1 */
    int          rank;      /* 1-based ordinal of this member in its group */
    uint32_t     cksum;     /* BOM checksum (filelist mode, from listing) */
    char        *link;      /* symlink target (filelist mode, from listing) */
} bm_path;

typedef struct bm_group {
    uint32_t first_pid;     /* pid of the group's first-encountered member */
    uint32_t *members;      /* member pids in encounter order (pid order) */
    size_t    nmem;
} bm_group;

typedef struct bm_walk {
    bm_path  *paths;        /* paths[pid-1]; index 0 == root '.', pid 1 */
    size_t    npaths;       /* number of entries (npaths = max pid) */
    bm_group *groups;       /* hard-link groups (nmembers >= 2) */
    size_t    ngroups;
    int       mode;         /* enum bm_mode */
} bm_walk;

/* Returns 0 on success; -1 with errno on failure (root unreadable).
 * Produces a BM_MODE_DIR walk (groups detected, cksum/link unset). */
int bm_scan(const char *root, bm_walk *out);

void bm_walk_free(bm_walk *w);

/* "./..." form path string for a pid; caller frees. */
char *bm_relpath(const bm_walk *w, uint32_t pid);

#endif /* BOM_FS_WALK_H */