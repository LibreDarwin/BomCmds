/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * Clean-room reimplementation of Apple's /usr/bin/ditto.
 *
 * Shared types for the copier, archive reader/writer and filters.
 * Behavior and all user-visible output are byte-identical to the reference
 * tool (verified against macOS /usr/bin/ditto); see FORMAT.md sections
 * 10-15 and local/BomCmds.md. */
#ifndef DITTO_H
#define DITTO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* --------------------------------------------------------------------- */
/* Options (mirrors the long-option surface of the reference tool).      */
/* --------------------------------------------------------------------- */

typedef struct ditto_opts {
    /* flags */
    int help;
    int verbose;              /* -v */
    int very_verbose;         /* -V */
    int no_cross_dev;         /* -X */
    int create;               /* -c */
    int extract;              /* -x */
    int gzip;                 /* -z */
    int bzip2;                /* -j */
    int zip;                  /* -k / --pkzip */
    int keep_parent;          /* --keepParent */
    int bom_filter;           /* --bom */
    int lang_filter;          /* --lang */
    int norsrc;               /* --norsrc */
    int noextattr;            /* --noextattr */
    int noqtn;                /* --noqtn */
    int noacl;                /* --noacl */
    int sequester_rsrc;       /* --sequesterRsrc */
    int zlib_level_set;
    int zlib_level;           /* --zlibCompressionLevel */
    int hfs_compression;      /* --hfsCompression */
    int nohfs_compression;    /* --nohfsCompression */
    int preserve_hfs_comp;    /* --preserveHFSCompression */
    int nopreserve_hfs_comp;  /* --nopreserveHFSCompression */
    int nocache;              /* --nocache */
    int non_atomic;           /* --nonAtomicCopies */
    int segment_large;        /* --segmentLargeFiles */
    int nopersist_rootless;   /* --nopersistRootless */
    int keep_binaries;        /* --keepBinaries */
    int clone;                /* --clone */

    /* value options */
    const char *keep_binaries_list;  /* --keepBinariesList */
    const char *bom_path;            /* --bom */
    const char *out_bom;             /* --outBom */
    char **archs;                    /* --arch (accumulated) */
    size_t narchs;
    char **langs;                    /* --lang (accumulated) */
    size_t nlangs;
} ditto_opts;

/* Resolved copy flags (defaults per the reference tool's "enabled by
 * default" list). */
typedef struct ditto_copy_flags {
    int rsrc;                /* preserve resource forks + HFS metadata */
    int extattr;             /* preserve extended attributes */
    int qtn;                 /* preserve quarantine */
    int acl;                 /* preserve ACLs */
    int persist_rootless;    /* persist SF_RESTRICTED / com.apple.rootless */
    int no_cross_dev;        /* -X: skip directories on other devices */
    int non_atomic;          /* --nonAtomicCopies */
    int preserve_hfs_comp;   /* preserve filesystem compression */
    int hfs_compression;     /* --hfsCompression */
    int clone;               /* --clone */
    int nocache;             /* --nocache */
    int    arch_active;      /* copy only requested CPU architectures */
    int    zlib_level;       /* 6 unless --zlibCompressionLevel */
    int    keep_binaries;
    int    zlib_level_set;   /* option was given (any mode) */
} ditto_copy_flags;

/* --------------------------------------------------------------------- */
/* Error plumbing.  ditto "almost never gives up"; errors are printed to  */
/* stderr and tallied, most per-file failures leave a diagnostic but      */
/* exit 0.                                                                */
/* --------------------------------------------------------------------- */

extern int ditto_nerrors;
extern int ditto_very_verbose;

void ditto_msg(const char *fmt, ...) __attribute__((format(printf,1,2)));

/* Reference-qualified diagnostics. */
extern const char *ditto_arch_unknown; /* "can't get arch info for '%s'" */

/* --------------------------------------------------------------------- */
/* Usage text.  ditto_usage is the byte-exact full text (includes trailing */
/* newline); the short form is what follows the first line.               */
/* --------------------------------------------------------------------- */

extern const char ditto_usage[];
void ditto_usage_short(void);
void ditto_usage_full(void);

/* DITTO_TEST_OPTIONS: prints the resolved-option table on stderr and
 * returns 1 if requested (environment set), 0 otherwise. */
int ditto_testopts_requested(void);
void ditto_dump_testopts(const struct ditto_opts *o,
                         const struct ditto_copy_flags *cf);

/* --------------------------------------------------------------------- */
/* Copy / create / extract engines.  Return 0 or 1 (process exit code).  */
/* --------------------------------------------------------------------- */

int ditto_copy(ditto_opts *o, ditto_copy_flags *cf,
               char *const *srcs, int nsrcs, const char *dst);
int ditto_create(ditto_opts *o, ditto_copy_flags *cf,
                 char *const *srcs, int nsrcs, const char *dst);
int ditto_extract(ditto_opts *o, ditto_copy_flags *cf,
                  char *const *srcs, int nsrcs, const char *dst);

/* Write one source's verbose header ("Copying"/">>> Copying").  The arch
 * list is appended for -V only. */
void ditto_print_header(const ditto_opts *o, const char *src);

/* Sidecar generation from an open fd (path variant).  Returns malloc'd
 * AppleDouble image or NULL when no sidecar is needed. */
struct ad_xattr {
    const char *name;
    const void *value;
    size_t      len;
};
void *ditto_make_sidecar(int fd, int is_dir, size_t *outlen);
void  ditto_gather_xattrs(int fd, struct ad_xattr **out, size_t *outn);

/* --------------------------------------------------------------------- */
/* CPIO odc reader/writer.                                                */
/* --------------------------------------------------------------------- */

typedef enum { CPIO_PLAIN, CPIO_GZIP, CPIO_BZIP2 } cpio_compress;

/* Writer sink.  Writes are routed through zlib/bzip2 as configured.
 * For CPIO_GZIP, `z` points at a struct gz_state declared in cpio.c; we
 * frame gzip ourselves (header XFL=0/OS=3) so bytes match the reference. */
struct cpio_out {
    FILE *f;
    uint64_t ino;               /* next synthetic inode */
    uint64_t written;           /* raw bytes out (for 512 pad) */
    void *z;                    /* struct gz_state* or BZFILE* */
    int   compress;
};
void cpio_out_init(struct cpio_out *co, FILE *f, cpio_compress c);
/* ino: pass (uint32_t)-1 to auto-assign/increment, else use the value
 * (for hard-link sharing).  Returns 0 on success, -1 on write error. */
int cpio_write_header(struct cpio_out *co, const char *name, uint32_t mode,
                      uint32_t uid, uint32_t gid, uint32_t nlink,
                      uint32_t mtime, uint64_t size, uint32_t ino);
int cpio_write_data(struct cpio_out *co, const void *data, uint64_t len);
int cpio_write_trailer(struct cpio_out *co);
int cpio_finish_pad(struct cpio_out *co); /* zero-pad to next 512 */
int cpio_flush(struct cpio_out *co);
int cpio_out_close(struct cpio_out *co, int *io_err);

/* Reader over an in-memory stream. */
struct cpio_entry {
    char  name[1024];
    uint32_t mode, uid, gid, nlink, dev, ino, mtime;
    uint64_t size;
};
struct cpio_in {
    const uint8_t *p, *end;     /* next byte to read */
};
int cpio_next(struct cpio_in *in, struct cpio_entry *ce);
/* After cpio_next, `size` data bytes sit at in->p; caller must advance
 * in->p by ce->size (no per-entry padding in odc). */

/* --------------------------------------------------------------------- */
/* PKZip reader/writer.  zlib is required at link time.                   */
/* --------------------------------------------------------------------- */

/* Writer builds the archive image in memory (offset in central-directory
 * entries can be patched); ditto_create flushes the result to disk. */
struct zip_out {
    uint8_t *b;
    size_t   len, cap;
    int      level;
    uint32_t nentries;
    int      ioerr;
    void    *cd; /* array of struct cent_rec (private to zip.c) */
};
int zip_out_init(struct zip_out *zo, int level);
/* is_dir / is_symlink choose stored-vs-deflated and the entry type bits.
 * payload for symlinks is the target string, for dirs NULL. */
int zip_out_add(struct zip_out *zo, const char *name, uint32_t mode,
                uint32_t uid, uint32_t gid, time_t mtime, time_t atime,
                int is_dir, int is_symlink, const void *payload, size_t plen);
/* Returns the complete image (owned by zo) and length; zo is reset. */
const uint8_t *zip_out_finish(struct zip_out *zo, size_t *len);
void zip_out_free(struct zip_out *zo);

/* Reader over an in-memory zip image. */
struct zip_cent {
    char       *name;
    uint32_t    mode;         /* full mode incl. type (from external attrs) */
    uint16_t    method;
    uint16_t    flags;
    uint32_t    crc, csz, usz;
    time_t      mtime, atime; /* unix seconds or fallback DOS stamp */
    uint32_t    local_off;
    int         is_dir, is_symlink;
};
struct zip_in {
    const uint8_t *p;
    size_t         n;
    struct zip_cent *cen;
    size_t         ncen, icen;
    uint8_t       *data;      /* decompressed current entry (malloc'd) */
    size_t         dlen;
};
/* Loads central directory; returns number of entries or -1. */
int zip_in_open(struct zip_in *zi, const uint8_t *p, size_t n);
/* Parse next local entry in order; fills *ce, decompresses data into
 * zi->data/zi->dlen.  Returns 1 entry, 0 EOF, -1 error. */
int zip_in_next(struct zip_in *zi, struct zip_cent *ce);
void zip_in_close(struct zip_in *zi);

/* --------------------------------------------------------------------- */
/* AppleDouble / ATTR blob writer/reader (._ sidecars for CPIO and zip).  */
/* --------------------------------------------------------------------- */

#define AD_MAGIC 0x00051607u
#define AD_VERSION 0x00020000u
#define AD_ENTRY_ATTR 0x9
#define AD_ENTRY_DATA 0x2

/* Build an AppleDouble file image (header + ATTR blob + optional resource
 * fork).  `n` xattr descriptors (name+value, listxattr order) and `rfork`
 * bytes (may be NULL/0).  Returns malloc'd buffer in *out, length in
 * *outlen, or NULL if nothing to store. */
struct ad_xattr_list {
    struct ad_xattr *x;
    size_t n;
};
void *ad_build(const struct ad_xattr *xattrs, size_t n, const void *rfork,
               size_t rfork_len, size_t *outlen);

/* Decode an AppleDouble image; returns array of xattr descriptors
 * (malloc'd) plus rfork bytes.  0 on success, -1 on malformed. */
int ad_parse(const void *ad, size_t len, struct ad_xattr **xattrs,
             size_t *nx, void **rfork, size_t *rfork_len);

/* --------------------------------------------------------------------- */
/* Mach-O thinning (--arch).                                              */
/* --------------------------------------------------------------------- */

/* Recognize Mach-O enough for `--arch` matching and fat-file slicing. */
struct macho_fat {
    int    is_fat;                 /* MH_MAGIC/MH_CIGAM fat header */
    char   cpu_names[16][16];      /* arch names for slices */
    uint64_t slice_off[16];
    uint32_t slice_len[16];
    uint32_t fat_count;
    int    big_endian;
};
/* Returns 0 and fills mf when the image is a fat Mach-O; 1 if not
 * Mach-O; -1 on malformed. */
int macho_scan(const uint8_t *buf, size_t len, struct macho_fat *mf);
/* Returns 1 if `buf` is a thin Mach-O matching `arch`, else 0. */
int macho_is_thin_for(const uint8_t *buf, size_t len, const char *arch);
/* Map an --arch name to {"canonical" name, }.  0 on success, -1 unknown. */
int macho_arch_lookup(const char *name, uint32_t *cputype_val,
                      int *match_everything);
/* Produce the thinned image of a Mach-O file for the requested arch list:
 * 0 -> *out malloc'd thin image; 1 -> file does not match (skip); -1 error. */
int ditto_thin_build(const uint8_t *src, size_t len,
                     const char *const *archs, size_t narchs,
                     uint8_t **out, size_t *outlen);

/* --------------------------------------------------------------------- */
/* BOM filter (--bom).                                                    */
/* --------------------------------------------------------------------- */

struct ditto_bomf {
    char  **paths;              /* matched paths ("./..." form) */
    size_t  npaths;
    int     ok;
};
struct ditto_bomf *bomf_open(const char *path);
int  bomf_contains(const struct ditto_bomf *bf, const char *rel);
void bomf_close(struct ditto_bomf *bf);

#endif /* DITTO_H */