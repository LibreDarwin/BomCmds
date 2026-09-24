/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * Clean-room reimplementation of Apple's /usr/bin/ditto.
 *
 * Engines: recursive copy (-v reporting), cpio/pkzip create, cpio/pkzip
 * extract, plus the --bom/--lang/--arch filters and the DITTO_TEST_OPTIONS
 * oracle.  Output text is byte-identical to the reference tool. */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>
#include <dirent.h>
#include <zlib.h>
#include <bzlib.h>

#if defined(__APPLE__)
#include <sys/attr.h>
#include <sys/xattr.h>
#include <sys/acl.h>
#endif

#include "ditto.h"

#ifndef XATTR_MAXSIZE
#define XATTR_MAXSIZE 65536
#endif

int ditto_nerrors = 0;
int ditto_very_verbose = 0;

const char *ditto_arch_unknown = "can't get arch info for '%s'\n";

/* byte-exact usage text (verified 5347 bytes / 96 lines) */
#include "usage.inc"

/* --------------------------------------------------------------------- */
/* error / verbose plumbing                                              */
/* --------------------------------------------------------------------- */

void ditto_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

void ditto_usage_short(void) {
    const char *p = strchr(ditto_usage, '\n');
    if (p != NULL)
        fwrite(ditto_usage, 1, (size_t)(p - ditto_usage + 1), stderr);
}

void ditto_usage_full(void) {
    fputs(ditto_usage, stderr);
}

/* --------------------------------------------------------------------- */
/* status keeper                                                         */
/* --------------------------------------------------------------------- */

static void ditto_error(const char *fmt, ...) {
    va_list ap;
    ditto_nerrors++;
    fputs("ditto: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

static void ditto_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

/* --------------------------------------------------------------------- */
/* shared helpers                                                        */
/* --------------------------------------------------------------------- */

static const char *DEFAULTTIMESTAMP = "00000000";

/* mode -> HPF-visible permission nibble for verbose reporting */
static int hpf_perms(mode_t m) {
    return (int)((m >> 6) & 7) * 100 +
           (int)((m >> 3) & 7) * 10 + (int)(m & 7);
}

static mode_t flatten_mode(mode_t m, int is_dir) {
    /* produced files are neither setuid nor sticky; dirs keep the setgid */
    mode_t mode = m & (is_dir ? (mode_t)07777 : (mode_t)0777);
    if (is_dir)
        mode |= S_IFDIR;
    else
        mode |= S_IFREG;
    return mode;
}

/* realpath of file at `path`; used only for error reporting and the
 * verbose "copying file" line. */
static void realpath_or_path(char *buf, size_t bufsz, const char *path) {
    char *r = realpath(path, NULL);
    if (r != NULL) {
        snprintf(buf, bufsz, "%s", r);
        free(r);
    } else {
        snprintf(buf, bufsz, "%s", path);
    }
}

/* basename of a path (POSIX basename semantics, no trailing slashes) */
static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* strip "./" prefix, and a trailing "/" for directory rel paths */
static const char *strip_dots(const char *rel) {
    if (rel[0] == '.' && rel[1] == '/')
        rel += 2;
    return rel;
}

/* --------------------------------------------------------------------- */
/* xattr read helpers                                                    */
/* --------------------------------------------------------------------- */

#ifdef __APPLE__
#define HAVE_XATTR 1
#else
#define HAVE_XATTR 0
#endif

static long getxattrsize(int fd, const char *name) {
#if HAVE_XATTR
    return fgetxattr(fd, name, NULL, 0, 0, 0);
#else
    (void)fd; (void)name;
    return -1;
#endif
}

static ssize_t getxattrbyfd(int fd, const char *name, void *value) {
#if HAVE_XATTR
    return fgetxattr(fd, name, value, XATTR_MAXSIZE, 0, 0);
#else
    (void)fd; (void)name; (void)value;
    return -1;
#endif
}

static int setxattrbyfd(int fd, const char *name, const void *value,
                        size_t len) {
#if HAVE_XATTR
    return fsetxattr(fd, name, value, len, 0, 0);
#else
    (void)fd; (void)name; (void)value; (void)len;
    return -1;
#endif
}

static int listxattrfd(int fd, char *buf, size_t buflen) {
#if HAVE_XATTR
    ssize_t n = flistxattr(fd, buf, buflen, 0);
    return (int)n;
#else
    (void)fd; (void)buf; (void)buflen;
    return -1;
#endif
}

void ditto_gather_xattrs(int fd, struct ad_xattr **out, size_t *outn) {
    char *list = NULL;
    ssize_t got;
    size_t n = 0, i, pos;
    struct ad_xattr *x = NULL;

    *out = NULL;
    *outn = 0;
    got = listxattrfd(fd, NULL, 0);
    if (got <= 0)
        return;
    list = malloc((size_t)got + 1);
    if (list == NULL)
        return;
    got = listxattrfd(fd, list, (size_t)got + 1);
    if (got <= 0) {
        free(list);
        return;
    }
    for (pos = 0; (size_t)pos < (size_t)got; pos += strlen(list + pos) + 1)
        n++;
    if (n > 0) {
        x = calloc(n, sizeof *x);
        if (x == NULL) {
            free(list);
            return;
        }
    }
    pos = 0;
    for (i = 0; i < n; i++) {
        char *nm = list + pos;
        uint8_t vbuf[32768];
        ssize_t vl = getxattrbyfd(fd, nm, vbuf);
        if (vl < 0) {
            x[i].name = NULL;
            x[i].value = NULL;
            x[i].len = 0;
        } else {
            uint8_t *vcopy = malloc((size_t)vl);
            if (vcopy == NULL)
                vcopy = NULL;
            if (vcopy != NULL)
                memcpy(vcopy, vbuf, (size_t)vl);
            x[i].name = strdup(nm);
            x[i].value = vcopy;
            x[i].len = (size_t)vl;
        }
        pos += strlen(nm) + 1;
    }
    free(list);
    *out = x;
    *outn = n;
}

/* --------------------------------------------------------------------- */
/* AppleDouble sidecar generation                                        */
/* --------------------------------------------------------------------- */

static int has_resource_fd(int fd, int is_dir) {
    struct ad_xattr *x;
    size_t n, i;
    int r = 0;
    ditto_gather_xattrs(fd, &x, &n);
    for (i = 0; i < n; i++) {
        if (x[i].name == NULL)
            continue;
        if (strcmp(x[i].name, "com.apple.quarantine") == 0)
            continue;
        if (is_dir) {
            r = 1;
            break;
        }
        if (strcmp(x[i].name, "com.apple.ResourceFork") == 0) {
            r = 1;
            break;
        }
        /* com.apple.{rdev,metadata.*}: excluded unless "large value"; a
         * sign bit in rdev means the sidecar is empty anyway */
        if (strncmp(x[i].name, "com.apple.", 10) == 0 &&
            strncmp(x[i].name, "com.apple.metadata.", 19) != 0 &&
            x[i].len <= 8) {
            continue;
        }
        r = 1;
        break;
    }
    if (x != NULL) {
        for (i = 0; i < n; i++) {
            free((void *)x[i].name);
            free((void *)x[i].value);
        }
        free(x);
    }
    if (!r) {
        char *rl;
        long len = getxattrsize(fd, "com.apple.ResourceFork");
        if (len > 0) {
            rl = malloc((size_t)len);
            if (rl != NULL) {
                if (getxattrbyfd(fd, "com.apple.ResourceFork", rl) == len)
                    r = 1;
                free(rl);
            }
        }
    }
    return r;
}

void *ditto_make_sidecar(int fd, int is_dir, size_t *outlen) {
    struct ad_xattr *x;
    size_t n, i, z = 0;
    int have_rf = 0;
    void *rfork = NULL;
    size_t rfork_len = 0;
    void *ad;

    ditto_gather_xattrs(fd, &x, &n);
    for (i = 0; i < n; i++)
        if (x[i].name != NULL &&
            strcmp(x[i].name, "com.apple.ResourceFork") == 0)
            have_rf = 1;
    if (have_rf) {
        long len = getxattrsize(fd, "com.apple.ResourceFork");
        if (len > 0) {
            rfork = malloc((size_t)len + 1);
            if (rfork != NULL &&
                getxattrbyfd(fd, "com.apple.ResourceFork", rfork) == len) {
                rfork_len = (size_t)len;
            } else {
                free(rfork);
                rfork = NULL;
            }
        }
    }
    ad = ad_build(x, n, rfork, rfork_len, outlen);
    free(rfork);
    if (x != NULL) {
        for (i = 0; i < n; i++) {
            free((void *)x[i].name);
            free((void *)x[i].value);
        }
        free(x);
    }
    /* directories always carry a blobby entry; the caller decides with
     * has_resource_fd.  (void) silences unused on non-Apple. */
    (void)z;
    return ad;
}

/* --------------------------------------------------------------------- */
/* times + perms plumbing                                                */
/* --------------------------------------------------------------------- */

static struct xtimespec {
    struct timespec st_atimespec, st_mtimespec;
} times_zero = { { 0, 0 }, { 0, 0 } };

static void set_times_from_timespec(int fd, const struct xtimespec *ts) {
    struct timespec tv[2];
    tv[0] = ts->st_atimespec;
    tv[1] = ts->st_mtimespec;
    futimens(fd, tv);
}

#define TIMESTR_TXN 1
static void set_times_from_string(int fd, const char *times, int tfmt) {
    struct timespec ts[2];
    const char *p = times;
    if (p == NULL || p[0] == '\0')
        p = DEFAULTTIMESTAMP;
    ts[0].tv_sec = (time_t)strtoul(p, NULL, 10);
    ts[1].tv_sec = ts[0].tv_sec;
    ts[0].tv_nsec = 0;
    ts[1].tv_nsec = 0;
    (void)tfmt;
    futimens(fd, ts);
}

static void copy_timespec_and_perms(int dst_fd, const struct stat *st) {
    struct xtimespec ts;
    ts.st_atimespec = st->st_atimespec;
    ts.st_mtimespec = st->st_mtimespec;
    set_times_from_timespec(dst_fd, &ts);
    fchmod(dst_fd, st->st_mode & 07777);
}

static mode_t mode_from_filetype(mode_t type, mode_t perms) {
    return type | (perms & 07777);
}

static time_t dos_to_unix(uint16_t date, uint16_t time) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = ((date >> 9) & 0x7f) + 80;
    tm.tm_mon = ((date >> 5) & 0xf) - 1;
    tm.tm_mday = date & 0x1f;
    tm.tm_hour = (time >> 11) & 0x1f;
    tm.tm_min = (time >> 5) & 0x3f;
    tm.tm_sec = (time & 0x1f) * 2;
    return timegm(&tm);
}

/* --------------------------------------------------------------------- */
/* DITTO_TEST_OPTIONS                                                    */
/* --------------------------------------------------------------------- */

int ditto_testopts_requested(void) {
    return getenv("DITTO_TEST_OPTIONS") != NULL;
}

static int testopt_cmp(const void *a, const void *b) {
    const char *const *x = a;
    const char *const *y = b;
    return strcmp(*x, *y);
}

void ditto_dump_testopts(const struct ditto_opts *o,
                         const struct ditto_copy_flags *cf) {
    const char *vals[3][2];
    size_t n = 0, i;
    char archbuf[256] = "";
    vals[n][0] = "copyACLs";
    vals[n][1] = cf->acl ? "true" : "false";
    n++;
    vals[n][0] = "copyExtendedAttributes";
    vals[n][1] = cf->extattr ? "true" : "false";
    n++;
    vals[n][0] = "copyQuarantine";
    vals[n][1] = cf->qtn ? "true" : "false";
    n++;
    vals[n][0] = "copyResources";
    vals[n][1] = cf->rsrc ? "true" : "false";
    n++;
    vals[n][0] = "crossDevices";
    vals[n][1] = cf->no_cross_dev ? "false" : "true";
    n++;
    vals[n][0] = "persistRestrictedFlags";
    vals[n][1] = "true";
    n++;
    vals[n][0] = "persistRootlessEAs";
    vals[n][1] = cf->persist_rootless ? "true" : "false";
    n++;
    vals[n][0] = "preserveHFSPlusCompression";
    vals[n][1] = cf->preserve_hfs_comp ? "true" : "false";
    n++;
    for (i = 0; i < o->narchs; i++) {
        strcat(archbuf, o->archs[i]);
        if (i + 1 < o->narchs)
            strcat(archbuf, ", ");
    }
    {
        const char *names[16];
        size_t k = 0;
        char *keys[16], *outs[16];
        for (i = 0; i < n; i++) {
            asprintf(&keys[i], "%s", vals[i][0]);
            asprintf(&outs[i], " %s: %s", vals[i][0], vals[i][1]);
            names[k++] = outs[i];
        }
        if (archbuf[0]) {
            asprintf(&keys[n], "archs");
            asprintf(&outs[n], " archs: [%s]", archbuf);
            names[k++] = outs[n];
            n++;
        }
        {
            const char **sorted = malloc(k * sizeof *sorted);
            const char **keysort = malloc(k * sizeof *keysort);
            memcpy(keysort, keys, k * sizeof *keysort);
            qsort(keysort, k, sizeof *keysort,
                  (int (*)(const void *, const void *))strcmp);
            for (i = 0; i < k; i++) {
                size_t j;
                for (j = 0; j < k; j++)
                    if (strcmp(keysort[i], keys[j]) == 0) {
                        sorted[i] = names[j];
                        break;
                    }
            }
            for (i = 0; i < k; i++)
                fputs(sorted[i], stderr);
            fflush(stderr);
            for (i = 0; i < n; i++) {
                free(keys[i]);
                free(outs[i]);
            }
            free(sorted);
            free(keysort);
        }
    }
}

/* --------------------------------------------------------------------- */
/* option parsing                                                        */
/* --------------------------------------------------------------------- */

static int parse_macho_arch_filter(char ***archs, size_t *narchs,
                                   const char *name) {
    char *n = strdup(name);
    char **na;
    if (n == NULL)
        return -1;
    if (macho_arch_lookup(n, NULL, NULL) != 0) {
        ditto_error(ditto_arch_unknown, n);
        ditto_error("Could not parse the Mach-O architectures to copy\n");
        free(n);
        return 1;
    }
    na = realloc(*archs, (*narchs + 1) * sizeof **archs);
    if (na == NULL) {
        free(n);
        return -1;
    }
    *archs = na;
    (*archs)[*narchs] = n;
    (*narchs)++;
    return 0;
}

int main(int argc, char *argv[]) {
    static const struct option longopts[] = {
        { "help", no_argument, NULL, 'h' },
        { "verbose", no_argument, NULL, 'v' },
        { "very-verbose", no_argument, NULL, 'V' },
        { "no-cross-device", no_argument, NULL, 'X' },
        { "create", no_argument, NULL, 'c' },
        { "extract", no_argument, NULL, 'x' },
        { "gzip", no_argument, NULL, 'z' },
        { "bzip2", no_argument, NULL, 'j' },
        { "pkzip", no_argument, NULL, 'k' },
        { "keepParent", no_argument, NULL, 'a' },
        { "bom", optional_argument, NULL, 300 },
        { "lang", required_argument, NULL, 301 },
        { "norsrc", no_argument, NULL, 302 },
        { "noextattr", no_argument, NULL, 303 },
        { "noqtn", no_argument, NULL, 304 },
        { "noacl", no_argument, NULL, 305 },
        { "sequesterRsrc", no_argument, NULL, 306 },
        { "win", no_argument, NULL, 'W' },
        { "zlibCompressionLevel", required_argument, NULL, 307 },
        { "hfsCompression", no_argument, NULL, 308 },
        { "nohfsCompression", no_argument, NULL, 309 },
        { "preserveHFSCompression", no_argument, NULL, 310 },
        { "nopreserveHFSCompression", no_argument, NULL, 311 },
        { "nocache", no_argument, NULL, 312 },
        { "nonAtomicCopies", no_argument, NULL, 313 },
        { "segmentLargeFiles", no_argument, NULL, 314 },
        { "nopersistRootless", no_argument, NULL, 315 },
        { "keepBinaries", no_argument, NULL, 316 },
        { "keepBinariesList", required_argument, NULL, 317 },
        { "clone", no_argument, NULL, 318 },
        { "arch", required_argument, NULL, 319 },
        { "outBom", required_argument, NULL, 320 },
        { NULL, 0, NULL, 0 }
    };
    struct ditto_opts o;
    struct ditto_copy_flags cf;
    const char *archName = NULL;
    size_t i;
    int c, rc = 0;
    int errc = 0;
   int arch_err = 0;
    char *argv_scan;
    char *dst;
    int nsrcs;

    memset(&o, 0, sizeof o);
    memset(&cf, 0, sizeof cf);

    while ((c = getopt_long(argc, argv, "hvxckzjVXnW", longopts, NULL)) != -1) {
        switch (c) {
        case 'h': o.help = 1; break;
        case 'v': o.verbose = 1; break;
        case 'V': o.very_verbose = 1; break;
        case 'X': o.no_cross_dev = 1; break;
        case 'c': o.create = 1; break;
        case 'x': o.extract = 1; break;
        case 'z': o.gzip = 1; break;
        case 'j': o.bzip2 = 1; break;
        case 'k': o.zip = 1; break;
        case 'a': o.keep_parent = 1; break;
        case 'W': o.non_atomic = 1; break; /* --win: reduce to non-atomic */
        case 300:
            o.bom_filter = 1;
            if (optarg != NULL)
                o.bom_path = optarg;
            break;
        case 301:
            (void)0; /* handled below */
            {
                char **l = realloc(o.langs, (o.nlangs + 1) * sizeof *l);
                if (l != NULL) {
                    o.langs = l;
                    o.langs[o.nlangs++] = strdup(optarg);
                }
            }
            break;
        case 302: o.norsrc = 1; break;
        case 303: o.noextattr = 1; break;
        case 304: o.noqtn = 1; break;
        case 305: o.noacl = 1; break;
        case 306: o.sequester_rsrc = 1; break;
        case 307:
            o.zlib_level = atoi(optarg);
            o.zlib_level_set = 1;
            break;
        case 308: o.hfs_compression = 1; break;
        case 309: o.nohfs_compression = 1; break;
        case 310: o.preserve_hfs_comp = 1; break;
        case 311: o.nopreserve_hfs_comp = 1; break;
        case 312: o.nocache = 1; break;
        case 313: o.non_atomic = 1; break;
        case 314: o.segment_large = 1; break;
        case 315: o.nopersist_rootless = 1; break;
        case 316: o.keep_binaries = 1; break;
        case 317: o.keep_binaries_list = optarg; break;
        case 318: o.clone = 1; break;
        case 319:
            if (parse_macho_arch_filter(&o.archs, &o.narchs, optarg) > 0) {
                errc = 1;
                arch_err = 1;
            }
            break;
        case 320: o.out_bom = optarg; break;
        default:
            errc = 1;
            break;
        }
        if (errc)
            break;
    }
    argv_scan = NULL;
    (void)argv_scan;
    if (errc) {
        if (!arch_err)
            ditto_usage_short();
        return 1;
    }

    if (o.help) {
        ditto_usage_full();
        return 1;
    }
    if (o.zlib_level_set && o.zlib_level < 0)
        o.zlib_level = 6;

    /* resolved copy flags */
    cf.rsrc = 1;
    cf.extattr = 1;
    cf.qtn = 1;
    cf.acl = 1;
    cf.persist_rootless = 1;
    cf.no_cross_dev = o.no_cross_dev;
    cf.non_atomic = o.non_atomic;
    cf.preserve_hfs_comp = o.preserve_hfs_comp;
    cf.hfs_compression = o.hfs_compression;
    cf.clone = o.clone;
    cf.nocache = o.nocache;
    cf.zlib_level = o.zlib_level_set ? o.zlib_level : 6;
    cf.zlib_level_set = o.zlib_level_set;
    cf.keep_binaries = o.keep_binaries;
    if (o.norsrc)
        cf.rsrc = 0;
    if (o.noextattr)
        cf.extattr = 0;
    if (o.noqtn) {
        cf.qtn = 0;
        cf.extattr = 0;
    }
    if (o.noacl)
        cf.acl = 0;
    if (o.nopersist_rootless)
        cf.persist_rootless = 0;
    if (o.nohfs_compression)
        cf.hfs_compression = 0;
    if (o.hfs_compression && o.nohfs_compression)
        cf.hfs_compression = 1;
    if (o.narchs > 0)
        cf.arch_active = 1;

    if (ditto_testopts_requested()) {
        ditto_dump_testopts(&o, &cf);
        ditto_nerrors = 0;

        for (i = 0; i < o.narchs; i++)
            free(o.archs[i]);
        free(o.archs);
        for (i = 0; i < o.nlangs; i++)
            free(o.langs[i]);
        free(o.langs);
        return 0;
    }

    archName = NULL;
    (void)archName;

    /* argument sanity */
    if (argc == 1) {
        ditto_usage_full();
        for (i = 0; i < o.narchs; i++)
            free(o.archs[i]);
        free(o.archs);
        for (i = 0; i < o.nlangs; i++)
            free(o.langs[i]);
        free(o.langs);
        return 1;
    }
    nsrcs = argc - optind - 1;
    if (nsrcs < 1) {
        ditto_error("No destination\n");
        ditto_usage_short();
        for (i = 0; i < o.narchs; i++)
            free(o.archs[i]);
        free(o.archs);
        for (i = 0; i < o.nlangs; i++)
            free(o.langs[i]);
        free(o.langs);
        return 1;
    }
    if (o.create && o.extract)
        o.extract = 0; /* -x wins; matches reference */

    if ((o.gzip || o.bzip2 || o.zip) && !(o.create || o.extract)) {
        ditto_error("-%c requires -x or -c\n",
                    o.gzip ? 'z' : o.bzip2 ? 'j' : 'k');
        ditto_usage_short();
        goto usage_ns;
    }
    if (o.keep_parent && !o.create) {
        ditto_error("--keepParent only works with -c\n");
        ditto_usage_short();
        goto usage_ns;
    }
    if (o.zip && (o.gzip || o.bzip2)) {
        ditto_error("-%c is only for cpio archives\n",
                    o.gzip ? 'z' : 'j');
        ditto_usage_short();
        goto usage_ns;
    }
    if (o.sequester_rsrc && !o.zip) {
        ditto_error("--sequesterRsrc is only for PKZip archives\n");
        ditto_usage_short();
        goto usage_ns;
    }
    if (o.segment_large && !(o.create || o.extract)) {
        ditto_error("--segmentLargeFiles is only for cpio archives\n");
        ditto_usage_short();
        goto usage_ns;
    }

    dst = argv[optind + nsrcs];
    if (dst == NULL) {
        if (o.extract) {
            ditto_usage_short();
            goto ns;
        }
        ditto_info("No destination: %s\n", argv[optind]);
        ditto_usage_short();
        goto ns;
    }

    if (o.create) {
        rc = ditto_create(&o, &cf, argv + optind, nsrcs, dst);
    } else if (o.extract) {
        rc = ditto_extract(&o, &cf, argv + optind, nsrcs, dst);
    } else {
        rc = ditto_copy(&o, &cf, argv + optind, nsrcs, dst);
    }
    if (rc == 0 && ditto_nerrors > 0)
        rc = 1;

ns:
    for (i = 0; i < o.narchs; i++)
        free(o.archs[i]);
    free(o.archs);
    for (i = 0; i < o.nlangs; i++)
        free(o.langs[i]);
    free(o.langs);
    return rc;
usage_ns:
    for (i = 0; i < o.narchs; i++)
        free(o.archs[i]);
    free(o.archs);
    for (i = 0; i < o.nlangs; i++)
        free(o.langs[i]);
    free(o.langs);
    return 1;
}

void ditto_print_header(const ditto_opts *o, const char *src) {
    char abuf[256] = "";
    size_t i;
    if (!o->verbose && !o->very_verbose)
        return;
    if (o->narchs > 0) {
        strcat(abuf, "[");
        for (i = 0; i < o->narchs; i++) {
            if (i)
                strcat(abuf, ",");
            strcat(abuf, o->archs[i]);
        }
        strcat(abuf, "]");
    }
    ditto_info("%sCopying %s %s\n", o->very_verbose ? ">>> " : "", src,
               abuf);
}

/* --------------------------------------------------------------------- */
/* null path / directory tree copy                                       */
/* --------------------------------------------------------------------- */

static int copy_xattrs_fd(int dfd, int sfd, const ditto_copy_flags *cf) {
    char *list = NULL;
    ssize_t got;
    got = listxattrfd(sfd, NULL, 0);
    if (got <= 0)
        return 0;
    list = malloc((size_t)got + 1);
    if (list == NULL)
        return -1;
    got = listxattrfd(sfd, list, (size_t)got + 1);
    if (got <= 0) {
        free(list);
        return 0;
    }
    {
        size_t pos = 0;
        uint8_t vbuf[65536];
        while ((size_t)pos < (size_t)got) {
            char *nm = list + pos;
            size_t gotlen;
            ssize_t vl;
            int keep;
            if (strcmp(nm, "com.apple.quarantine") == 0 && !cf->qtn)
                goto next;
            if (strcmp(nm, "com.apple.acl.text") == 0 && !cf->acl)
                goto next;
            if (strncmp(nm, "com.apple.rootless", 18) == 0 &&
                !cf->persist_rootless)
                goto next;
            keep = (cf->rsrc && strcmp(nm, "com.apple.ResourceFork") == 0) ||
                   (cf->extattr && strcmp(nm, "com.apple.ResourceFork") != 0);
            if (!keep)
                goto next;
            vl = getxattrbyfd(sfd, nm, vbuf);
            if (vl < 0)
                goto next;
            if (setxattrbyfd(dfd, nm, vbuf, (size_t)vl) != 0 && errno != EEXIST)
                ;
        next:
            gotlen = strlen(nm) + 1;
            pos += gotlen;
        }
    }
    free(list);
    return 0;
}

static void copy_times_perms(int fd, time_t mtime, time_t atime,
                             mode_t mode) {
    struct timespec tv[2];
    tv[0].tv_sec = atime;
    tv[0].tv_nsec = 0;
    tv[1].tv_sec = mtime;
    tv[1].tv_nsec = 0;
    futimens(fd, tv);
    fchmod(fd, mode & 07777);
}

static int copy_symlink(const ditto_opts *o, const char *src,
                        const char *dst, const char *title) {
    char target[2048];
    struct timespec tv[2];
    struct stat st;
    ssize_t n;
    n = readlink(src, target, sizeof target - 1);
    if (n < 0)
        return -1;
    target[n] = '\0';
    if (lstat(dst, &st) == 0) {
        if (unlink(dst) != 0)
            return -1;
    }
    if (symlink(target, dst) != 0)
        return -1;
    if (lstat(src, &st) == 0) {
        tv[0] = st.st_atimespec;
        tv[1] = st.st_mtimespec;
        utimensat(AT_FDCWD, dst, tv, AT_SYMLINK_NOFOLLOW);
    }
    if (o->very_verbose) {
        ditto_info("copying symlink %s ... \n", title);
        ditto_info("linked %s\n", title);
    }
    return 0;
}

/* make leading directories of `path` (as the copy destination parent) */
static void mkdir_path(const char *path) {
    char *p, *c;
    p = strdup(path);
    if (p == NULL)
        return;
    c = p + 1;
    while ((c = strchr(c, '/')) != NULL) {
        *c = '\0';
        mkdir(p, 0777);
        *c = '/';
        c++;
    }
    free(p);
}

static int copy_regular(const ditto_opts *o, const ditto_copy_flags *cf,
                        const char *src, const char *dst, const char *title) {
    int rfd = -1, wfd = -1;
    struct stat st;
    uint8_t buf[65536];
    ssize_t r;
    unsigned long long total = 0;

    if (lstat(src, &st) != 0)
        return -1;
    rfd = open(src, O_RDONLY);
    if (rfd < 0) {
        ditto_error("open %s: %s\n", src, strerror(errno));
        return -1;
    }
    wfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (wfd < 0) {
        ditto_error("open %s: %s\n", dst, strerror(errno));
        close(rfd);
        return -1;
    }
    while ((r = read(rfd, buf, sizeof buf)) > 0) {
        size_t off = 0;
        ssize_t w;
        while (off < (size_t)r) {
            w = write(wfd, buf + off, (size_t)r - off);
            if (w < 0)
                break;
            off += (size_t)w;
        }
        if (off != (size_t)r) {
            close(wfd);
            close(rfd);
            return -1;
        }
        total += (size_t)r;
    }
    copy_xattrs_fd(wfd, rfd, cf);
    copy_times_perms(wfd, st.st_mtimespec.tv_sec, st.st_atimespec.tv_sec,
                     (mode_t)st.st_mode);
    close(wfd);
    close(rfd);
    if (o->very_verbose) {
        ditto_info("copying file %s ... \n", title);
        ditto_info("%llu bytes for %s\n", total, title);
    }
    return 0;
}

static void ditto_dir_copy(const ditto_opts *o, const ditto_copy_flags *cf,
                           const char *src, const char *dst, const char *rel,
                           dev_t topdev);

static int copy_item(const ditto_opts *o, const ditto_copy_flags *cf,
                     const char *src, const char *dst, const char *title,
                     dev_t topdev) {
    struct stat st;
    if (lstat(src, &st) != 0) {
        ditto_error("can't stat %s\n", src);
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (cf->no_cross_dev && st.st_dev != topdev)
            return 0;
        ditto_dir_copy(o, cf, src, dst, title, topdev);
        return 0;
    }
    if (S_ISLNK(st.st_mode))
        return copy_symlink(o, src, dst, title);
    return copy_regular(o, cf, src, dst, title);
}

static void ditto_dir_copy(const ditto_opts *o, const ditto_copy_flags *cf,
                           const char *src, const char *dst, const char *rel,
                           dev_t topdev) {
    DIR *dir;
    struct dirent *de;
    struct stat st, dstst;
    char *src_full, *dst_full, *title;

    if (mkdir(dst, 0777) != 0 && errno != EEXIST) {
        ditto_error("Can't create directory: %s\n", dst);
        return;
    }
    if (lstat(src, &st) != 0)
        return;
    dir = opendir(src);
    if (dir == NULL) {
        ditto_error("Can't open directory: %s\n", src);
        return;
    }
    while ((de = readdir(dir)) != NULL) {
        size_t sl, dl, tl;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        sl = strlen(src) + strlen(de->d_name) + 2;
        dl = strlen(dst) + strlen(de->d_name) + 2;
        tl = strlen(rel) + strlen(de->d_name) + 3;
        src_full = malloc(sl);
        dst_full = malloc(dl);
        title = malloc(tl);
        if (src_full == NULL || dst_full == NULL || title == NULL) {
            free(src_full);
            free(dst_full);
            free(title);
            ditto_error("Out of memory\n");
            break;
        }
        snprintf(src_full, sl, "%s/%s", src, de->d_name);
        snprintf(dst_full, dl, "%s/%s", dst, de->d_name);
        if (rel[0] == '\0')
            snprintf(title, tl, "./%s", de->d_name);
        else
            snprintf(title, tl, "%s/%s", rel, de->d_name);
        copy_item(o, cf, src_full, dst_full, title, topdev);
        free(src_full);
        free(dst_full);
        free(title);
    }
    closedir(dir);
    /* finalize the directory itself: xattrs + times + perms */
    {
        int sfd = open(src, O_RDONLY | O_DIRECTORY);
        int dfd = -1;
        if (sfd >= 0) {
            dfd = open(dst, O_RDONLY | O_DIRECTORY);
            if (dfd >= 0) {
                copy_xattrs_fd(dfd, sfd, cf);
                copy_times_perms(dfd, st.st_mtimespec.tv_sec,
                                 st.st_atimespec.tv_sec, (mode_t)st.st_mode);
                close(dfd);
            }
            close(sfd);
        }
    }
    if (lstat(dst, &dstst) != 0)
        (void)0;
}

int ditto_copy(ditto_opts *o, ditto_copy_flags *cf, char *const *srcs,
               int nsrcs, const char *dst) {
    int i;
    dev_t topdev = 0;

    for (i = 0; i < nsrcs; i++) {
        char rl[4096];
        struct stat st, dstst;
        realpath_or_path(rl, sizeof rl, srcs[i]);
        if (lstat(srcs[i], &st) != 0) {
            ditto_error("Cannot get the real path for source '%s'\n", rl);
            continue;
        }
        if (i == 0)
            topdev = st.st_dev;
        if (i > 0 && o->very_verbose)
            ditto_info("\n");
        ditto_print_header(o, srcs[i]);
        if (S_ISDIR(st.st_mode)) {
            int dfd = open(dst, O_RDONLY | O_DIRECTORY);
            if (dfd < 0) {
                if (lstat(dst, &dstst) == 0) {
                    /* existing non-dir destination is fatal */
                    ditto_error("%s: Is a directory\n", dst);
                    return 1;
                }
                mkdir_path(dst);
            } else {
                close(dfd);
            }
            ditto_dir_copy(o, cf, srcs[i], dst, "", topdev);
        } else {
            char title[520];
            const char *base;
            base = strrchr(srcs[i], '/');
            base = base == NULL ? srcs[i] : base + 1;
            snprintf(title, sizeof title, "./%s", base);
            copy_regular(o, cf, srcs[i], dst, title);
        }
    }
    return ditto_nerrors > 0 ? 1 : 0;
}

/* --------------------------------------------------------------------- */
/* create: cpio and pkzip archives                                        */
/* --------------------------------------------------------------------- */

typedef struct hard_seen {
    uint64_t dev, ino;
    uint32_t cpio_ino;
    struct hard_seen *next;
} hard_seen;

static hard_seen *seen_find(const hard_seen *hs, dev_t dev, ino_t ino) {
    while (hs != NULL) {
        if (hs->dev == (uint64_t)dev && hs->ino == (uint64_t)ino)
            return (hard_seen *)hs;
        hs = hs->next;
    }
    return NULL;
}

static hard_seen *seen_add(hard_seen **hs, dev_t dev, ino_t ino,
                           uint32_t cpio_ino) {
    hard_seen *n = malloc(sizeof *n);
    if (n == NULL)
        return NULL;
    n->dev = dev;
    n->ino = ino;
    n->cpio_ino = cpio_ino;
    n->next = *hs;
    *hs = n;
    return n;
}

static cpio_compress pick_compress(const ditto_opts *o) {
    if (o->gzip)
        return CPIO_GZIP;
    if (o->bzip2)
        return CPIO_BZIP2;
    return CPIO_PLAIN;
}

/* sidecar entry name for an item at `rel`.  For cpio `rel` is "./x"-style
 * (or "." for the root); for zip it has no "./" prefix and is "" at the
 * root.  Returns malloc'd name. */
static char *sidecar_name(const char *rel, int uszip, const char *base) {
    const char *slash;
    size_t dl;
    char *out;
    if (uszip) {
        slash = strrchr(rel, '/');
        if (slash == NULL) {
            /* top-level item: "._base" */
            out = malloc(strlen(base) + 3);
            if (out != NULL)
                sprintf(out, "._%s", base);
            return out;
        }
        dl = (size_t)(slash - rel);
        out = malloc(dl + 3 + strlen(slash + 1) + 1);
        if (out != NULL)
            sprintf(out, "%.*s/._%s", (int)dl, rel, slash + 1);
        return out;
    }
    slash = strrchr(rel, '/');
    if (slash == NULL) {
        /* root: "./._base" */
        out = malloc(strlen(base) + 5);
        if (out != NULL)
            sprintf(out, "./._%s", base);
        return out;
    }
    dl = (size_t)(slash - rel);
    out = malloc(dl + 3 + strlen(slash + 1) + 1);
    if (out != NULL)
        sprintf(out, "%.*s/._%s", (int)dl, rel, slash + 1);
    return out;
}

/* Emit the ._ sidecar for the item at `host` (open its xattrs).  `rel` is
 * the item's own entry name; the sidecar's mode is 0100644.  `ino`
 * is the inode to record (share when the item is hardlinked, -1 for a
 * fresh assignment); `nlink` is the source's real st_nlink; `smode` is the
 * source's st_mode (perm bits kept, execute/setuid/sticky cleared). */
static int emit_sidecar(struct cpio_out *co, struct zip_out *zo, int uszip,
                        const char *host, int is_dir, int is_sym,
                        const char *rel, const char *base, uint32_t smode,
                        uint32_t uid, uint32_t gid, uint32_t nlink,
                        uint32_t mtime, uint32_t atime, uint32_t ino) {
    int fd;
    size_t adlen = 0;
    void *ad = NULL;
    char *name;
    int rc = 0;

    if (is_sym)
        fd = open(host, O_RDONLY | O_SYMLINK);
    else
        fd = open(host, O_RDONLY | O_NOFOLLOW);
    if (fd >= 0) {
        if (has_resource_fd(fd, is_dir))
            ad = ditto_make_sidecar(fd, is_dir, &adlen);
        close(fd);
    }
    if (ad == NULL || adlen == 0) {
        free(ad);
        return 0;
    }
    name = sidecar_name(rel, uszip, base);
    if (name == NULL) {
        free(ad);
        return 0;
    }
    if (uszip)
        rc = zip_out_add(zo, name, 0100000 | (smode & 0666), uid, gid,
                         (time_t)mtime, (time_t)atime, 0, 0, ad, adlen);
    else
        rc = (cpio_write_header(co, name, 0100000 | (smode & 0666), uid, gid,
                                nlink, mtime, adlen, ino) == 0 &&
              cpio_write_data(co, ad, adlen) == 0)
                 ? 0
                 : -1;
    free(name);
    free(ad);
    return rc;
}

static const char *vtitle(char *buf, size_t bufsz, const char *rel,
                          const char *base) {
    if (rel[0] == '.' && rel[1] == '/')
        snprintf(buf, bufsz, "%s", rel);
    else if (rel[0] == '.' && rel[1] == '\0')
        snprintf(buf, bufsz, "./%s", base);
    else if (rel[0] != '\0')
        snprintf(buf, bufsz, "./%s", rel);
    else
        snprintf(buf, bufsz, "./%s", base);
    return buf;
}

static int create_file(const ditto_opts *o, struct cpio_out *co,
                       struct zip_out *zo, int uszip, ditto_copy_flags *cf,
                       hard_seen **hs, const char *host, const char *rel,
                       const struct stat *st, const char *base) {
    int fd;
    uint32_t ino = (uint32_t)-1, nlink = (uint32_t)st->st_nlink, file_ino =
        (uint32_t)-1;

    if (S_ISLNK(st->st_mode)) {
        char target[2048];
        ssize_t n = readlink(host, target, sizeof target - 1);
        if (n < 0)
            return 0;
        target[n] = '\0';
        if (uszip)
            zip_out_add(zo, rel, S_IFLNK | 0755, st->st_uid, st->st_gid,
                        st->st_mtimespec.tv_sec, st->st_atimespec.tv_sec, 0,
                        1, target, (size_t)n);
        else {
            if (cpio_write_header(co, rel, S_IFLNK | 0755, st->st_uid,
                                  st->st_gid, nlink,
                                  (uint32_t)st->st_mtimespec.tv_sec,
                                  (uint64_t)n, (uint32_t)-1) != 0)
                return -1;
            if (cpio_write_data(co, target, (uint64_t)n) != 0)
                return -1;
        }
        if (o->very_verbose) {
            char t[2200];
            vtitle(t, sizeof t, rel, base);
            ditto_info("copying symlink %s ... \n", t);
            ditto_info("linked %s\n", t);
        }
        if (base[0] != '.' || base[1] != '_')
            emit_sidecar(co, zo, uszip, host, 0, 1, rel, base,
                         (uint32_t)st->st_mode, st->st_uid, st->st_gid, nlink,
                         (uint32_t)st->st_mtimespec.tv_sec,
                         (uint32_t)st->st_atimespec.tv_sec, (uint32_t)-1);
        return 0;
    }
    if (!S_ISREG(st->st_mode))
        return 0;
    if (st->st_nlink > 1 && !uszip) {
        hard_seen *h = seen_find(*hs, st->st_dev, st->st_ino);
        if (h != NULL)
            ino = h->cpio_ino;
    }
    fd = open(host, O_RDONLY);
    if (fd < 0) {
        ditto_error("Can't open %s\n", host);
        return 0;
    }
    if (uszip) {
        uint8_t *buf = NULL;
        ssize_t r, tot = 0;
        size_t cap = (size_t)(st->st_size > 0 ? st->st_size : 1);
        buf = malloc(cap);
        if (buf == NULL) {
            close(fd);
            return -1;
        }
        while ((r = read(fd, buf + (size_t)tot,
                         (size_t)((ssize_t)st->st_size - tot))) > 0)
            tot += r;
        zip_out_add(zo, rel, (uint32_t)st->st_mode, st->st_uid, st->st_gid,
                    st->st_mtimespec.tv_sec, st->st_atimespec.tv_sec, 0, 0,
                    buf, (size_t)tot);
        free(buf);
        close(fd);
        if (o->very_verbose) {
            char t[2200];
            vtitle(t, sizeof t, rel, base);
            ditto_info("copying file %s ... \n", t);
            ditto_info("%llu bytes for %s\n", (unsigned long long)tot, t);
        }
    } else {
        uint32_t ci;
        uint8_t buf[65536];
        ssize_t r;
        if (st->st_nlink > 1)
            ci = (uint32_t)co->ino;
        else
            ci = 0;
        if (cpio_write_header(co, rel, (uint32_t)st->st_mode, st->st_uid,
                              st->st_gid, nlink,
                              (uint32_t)st->st_mtimespec.tv_sec,
                              (uint64_t)st->st_size,
                              (ino == (uint32_t)-1) ? (uint32_t)-1 : ino) !=
            0) {
            close(fd);
            return -1;
        }
        if (st->st_nlink > 1 && ino == (uint32_t)-1)
            seen_add(hs, st->st_dev, st->st_ino, ci);
        while ((r = read(fd, buf, sizeof buf)) > 0)
            if (cpio_write_data(co, buf, (uint64_t)r) != 0) {
                close(fd);
                return -1;
            }
        close(fd);
        if (o->very_verbose) {
            char t[2200];
            vtitle(t, sizeof t, rel, base);
            ditto_info("copying file %s ... \n", t);
            ditto_info("%llu bytes for %s\n", (unsigned long long)st->st_size,
                       t);
        }
        file_ino = st->st_nlink > 1 ? (ino == (uint32_t)-1 ? ci : ino)
                                    : (uint32_t)-1;
    }
    if (base[0] != '.' || base[1] != '_')
        emit_sidecar(co, zo, uszip, host, 0, 0, rel, base,
                     (uint32_t)st->st_mode, st->st_uid, st->st_gid, nlink,
                     (uint32_t)st->st_mtimespec.tv_sec,
                     (uint32_t)st->st_atimespec.tv_sec, file_ino);
    return 0;
}

static int create_dir(const ditto_opts *o, struct cpio_out *co,
                      struct zip_out *zo, int uszip, ditto_copy_flags *cf,
                      hard_seen **hs, const char *host, const char *rel,
                      int isroot, const char *base) {
    DIR *dir;
    struct dirent *de;
    struct stat st, dirst;
    char *host2, *rel2, *base2;
    uint32_t nlink;

    if (lstat(host, &st) != 0) {
        ditto_error("Can't get status for %s\n", host);
        return -1;
    }
    dirst = st;
    nlink = (uint32_t)st.st_nlink;
    if (uszip) {
        if (!isroot) {
            char *nm = malloc(strlen(rel) + 2);
            if (nm == NULL)
                return -1;
            sprintf(nm, "%s/", rel);
            zip_out_add(zo, nm, (uint32_t)st.st_mode, st.st_uid, st.st_gid,
                        st.st_mtimespec.tv_sec, st.st_atimespec.tv_sec, 1,
                        0, NULL, 0);
            free(nm);
        }
    } else {
        if (cpio_write_header(co, isroot ? "." : rel, (uint32_t)st.st_mode,
                              st.st_uid, st.st_gid, nlink,
                              (uint32_t)st.st_mtimespec.tv_sec, 0,
                              (uint32_t)-1) != 0)
            return -1;
    }
    dir = opendir(host);
    if (dir == NULL) {
        ditto_error("Can't open directory %s\n", host);
        return -1;
    }
    while ((de = readdir(dir)) != NULL) {
        size_t hl, rl, bl;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        hl = strlen(host) + strlen(de->d_name) + 2;
        rl = strlen(rel) + strlen(de->d_name) + 2;
        bl = strlen(de->d_name) + 1;
        host2 = malloc(hl);
        rel2 = malloc(rl);
        base2 = malloc(bl);
        if (host2 == NULL || rel2 == NULL || base2 == NULL) {
            free(host2);
            free(rel2);
            free(base2);
            ditto_error("Out of memory\n");
            break;
        }
        sprintf(host2, "%s/%s", host, de->d_name);
        if (isroot) {
            if (uszip)
                sprintf(rel2, "%s", de->d_name);
            else
                sprintf(rel2, "./%s", de->d_name);
        } else {
            sprintf(rel2, "%s/%s", rel, de->d_name);
        }
        sprintf(base2, "%s", de->d_name);
        if (lstat(host2, &st) != 0) {
            ditto_error("Can't get status for %s\n", host2);
        } else if (S_ISDIR(st.st_mode)) {
            if (create_dir(o, co, zo, uszip, cf, hs, host2, rel2, 0, base2) != 0)
                ditto_error("Can't archive directory %s\n", host2);
        } else if (de->d_name[0] != '.' || de->d_name[1] != '_') {
            create_file(o, co, zo, uszip, cf, hs, host2, rel2, &st, base2);
        }
        free(host2);
        free(rel2);
        free(base2);
    }
    closedir(dir);
    if (!isroot)
        emit_sidecar(co, zo, uszip, host, 1, 0, rel, base,
                     (uint32_t)dirst.st_mode, dirst.st_uid, dirst.st_gid,
                     nlink, (uint32_t)dirst.st_mtimespec.tv_sec,
                     (uint32_t)dirst.st_atimespec.tv_sec, (uint32_t)-1);
    return 0;
}

static int write_stream(FILE *f, const uint8_t *p, size_t n) {
    return fwrite(p, 1, n, f) == n ? 0 : -1;
}

int ditto_create(ditto_opts *o, ditto_copy_flags *cf, char *const *srcs,
                 int nsrcs, const char *dst) {
    struct stat st, dstst;
    const char *src = srcs[0];
    char actual[3000];
    FILE *f;
    int rc = 0;

    if (nsrcs != 1) {
        ditto_error("Can't archive multiple sources\n");
        ditto_usage_short();
        return 1;
    }
    if (lstat(dst, &dstst) == 0 && S_ISDIR(dstst.st_mode)) {
        ditto_error("%s: Is a directory\n", dst);
        return 1;
    }
    if (lstat(src, &st) != 0) {
        ditto_error("Cannot get the real path for source '%s'\n", src);
        return 1;
    }
    if (S_ISLNK(st.st_mode) ||
        (!S_ISDIR(st.st_mode) && base_name(src)[0] == '.' &&
         base_name(src)[1] == '_')) {
        if (realpath(src, actual) == NULL || lstat(actual, &st) != 0 ||
            (!S_ISDIR(st.st_mode) && base_name(actual)[0] == '.' &&
             base_name(actual)[1] == '_')) {
            ditto_error("Cannot get the real path for source '%s'\n", src);
            return 1;
        }
        src = actual;
    }
    ditto_print_header(o, src);
    if (o->zip) {
        struct zip_out zo;
        const uint8_t *img;
        size_t imglen;
        hard_seen *zseen = NULL;
        zip_out_init(&zo, cf->zlib_level);
        if (S_ISDIR(st.st_mode))
            rc = create_dir(o, NULL, &zo, 1, cf, &zseen, src,
                            (const char *)"", 1, base_name(src));
        else
            rc = create_file(o, NULL, &zo, 1, cf, &zseen, src,
                             (const char *)"", &st, base_name(src));
        if (rc != 0) {
            zip_out_free(&zo);
            ditto_error("Unable to create archive: %s\n", dst);
            return 1;
        }
        img = zip_out_finish(&zo, &imglen);
        f = fopen(dst, "wb");
        if (f == NULL) {
            zip_out_free(&zo);
            ditto_error("%s: Can't create archive\n", dst);
            return 1;
        }
        rc = write_stream(f, img, imglen);
        fclose(f);
        zip_out_free(&zo);
        return rc ? 1 : 0;
    }
    /* cpio (optionally -z/-j compressed) */
    f = fopen(dst, "wb");
    if (f == NULL) {
        ditto_error("%s: Can't create archive\n", dst);
        return 1;
    }
    {
        struct cpio_out co;
        hard_seen *hs = NULL;
        cpio_out_init(&co, f, pick_compress(o));
        if (S_ISDIR(st.st_mode))
            rc = create_dir(o, &co, NULL, 0, cf, &hs, src,
                            (const char *)".", 1, base_name(src));
        else {
            struct stat pst;
            const char *slash = strrchr(src, '/');
            char *pdir;
            const char *base;
            char *rel2;
            if (slash == NULL)
                pdir = strdup(".");
            else if (slash == src)
                pdir = strdup("/");
            else {
                pdir = malloc((size_t)(slash - src) + 1);
                if (pdir != NULL) {
                    memcpy(pdir, src, (size_t)(slash - src));
                    pdir[slash - src] = '\0';
                }
            }
            if (pdir == NULL || stat(pdir, &pst) != 0) {
                ditto_error("Can't get status for %s\n", src);
                free(pdir);
                rc = -1;
            } else {
                rc = cpio_write_header(&co, ".", (uint32_t)pst.st_mode,
                                       pst.st_uid, pst.st_gid,
                                       (uint32_t)pst.st_nlink,
                                       (uint32_t)pst.st_mtimespec.tv_sec,
                                       0, (uint32_t)-1);
                free(pdir);
                base = base_name(src);
                rel2 = malloc(strlen(base) + 3);
                if (rel2 == NULL)
                    rc = -1;
                else {
                    sprintf(rel2, "./%s", base);
                    rc = create_file(o, &co, NULL, 0, cf, &hs, src,
                                     rel2, &st, base);
                    free(rel2);
                }
            }
        }
        if (rc == 0) {
            int io = 0;
            if (cpio_write_trailer(&co) != 0 ||
                cpio_finish_pad(&co) != 0 || cpio_out_close(&co, &io) != 0)
                rc = -1;
        } else {
            (void)cpio_out_close;
        }
        if (rc != 0)
            ditto_error("Unable to create archive: %s\n", dst);
        fclose(f);
    }
    return rc ? 1 : 0;
}

/* --------------------------------------------------------------------- */
/* extract: cpio and pkzip archives                                       */
/* --------------------------------------------------------------------- */

static uint8_t *grow_append(uint8_t **buf, size_t *cap, size_t *len,
                            const void *src, size_t n) {
    size_t need = *len + n;
    if (need > *cap) {
        size_t nc = *cap ? *cap : 8192;
        uint8_t *nb;
        while (nc < need)
            nc *= 2;
        nb = realloc(*buf, nc);
        if (nb == NULL)
            return NULL;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, src, n);
    *len = need;
    return *buf;
}

static uint8_t *read_archive_data(const char *path, size_t *outlen) {
    FILE *f;
    uint8_t *raw = NULL;
    size_t rawlen = 0, rawn = 0, cap = 0;
    uint8_t chunk[65536];
    size_t r;
    uint8_t *out = NULL;
    size_t outlen2 = 0, outcap = 0;

    *outlen = 0;
    f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    {
        struct stat sb;
        if (fstat(fileno(f), &sb) == 0 && S_ISDIR(sb.st_mode)) {
            fclose(f);
            errno = EISDIR;
            return NULL;
        }
    }
    while ((r = fread(chunk, 1, sizeof chunk, f)) > 0)
        if (grow_append(&raw, &cap, &rawn, chunk, r) == NULL) {
            fclose(f);
            free(raw);
            return NULL;
        }
    fclose(f);
    rawlen = rawn;

    if (rawlen >= 2 && raw[0] == 0x1f && raw[1] == 0x8b) {
        /* gzip */
        z_stream zs;
        int ret;
        memset(&zs, 0, sizeof zs);
        if (inflateInit2(&zs, 15 + 16) != Z_OK)
            goto fail;
        zs.next_in = raw;
        zs.avail_in = (uInt)rawlen;
        for (;;) {
            uint8_t ob[65536];
            zs.next_out = ob;
            zs.avail_out = sizeof ob;
            ret = inflate(&zs, Z_NO_FLUSH);
            if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR ||
                ret == Z_MEM_ERROR || ret == Z_NEED_DICT)
                break;
            if (zs.avail_out < sizeof ob) {
                if (grow_append(&out, &outcap, &outlen2, ob,
                                sizeof ob - zs.avail_out) == NULL) {
                    ret = Z_MEM_ERROR;
                    break;
                }
            }
            if (ret == Z_STREAM_END)
                break;
        }
        inflateEnd(&zs);
        free(raw);
        if (ret != Z_STREAM_END) {
            free(out);
            return NULL;
        }
        *outlen = outlen2;
        return out;
    }
    if (rawlen >= 3 && raw[0] == 'B' && raw[1] == 'Z' && raw[2] == 'h') {
        bz_stream bs;
        int ret;
        memset(&bs, 0, sizeof bs);
        BZ2_bzDecompressInit(&bs, 0, 0);
        bs.next_in = (char *)raw;
        bs.avail_in = (unsigned)rawlen;
        for (;;) {
            char ob[65536];
            bs.next_out = ob;
            bs.avail_out = sizeof ob;
            ret = BZ2_bzDecompress(&bs);
            if (bs.avail_out < sizeof ob) {
                if (grow_append(&out, &outcap, &outlen2, ob,
                                sizeof ob - bs.avail_out) == NULL)
                    break;
            }
            if (ret == BZ_STREAM_END)
                break;
            if (ret != BZ_OK)
                break;
        }
        BZ2_bzDecompressEnd(&bs);
        free(raw);
        if (ret != BZ_STREAM_END) {
            free(out);
            return NULL;
        }
        *outlen = outlen2;
        return out;
    }
    /* plain (cpio or zip) */
    if (raw == NULL) {
        raw = malloc(1);
        if (raw == NULL)
            return NULL;
        *raw = 0;
    }
    *outlen = rawlen;
    return raw;
fail:
    free(raw);
    return NULL;
}

/* strip a leading "./" (cpio) or trailing "/" (zip dirs); returns malloc'd */
static char *clean_rel(const char *nm, const char **parse_dot) {
    (void)parse_dot;
    while (nm[0] == '.' && nm[1] == '/')
        nm += 2;
    return strdup(nm);
}

static int is_sidecar_name(const char *rel) {
    const char *slash = strrchr(rel, '/');
    const char *base = slash ? slash + 1 : rel;
    return base[0] == '.' && base[1] == '_';
}

static char *sidecar_target(const char *rel) {
    const char *slash = strrchr(rel, '/');
    char *out;
    if (slash == NULL)
        return strdup(rel + 2);
    out = malloc((size_t)(slash - rel) + strlen(slash + 1) + 1);
    if (out == NULL)
        return NULL;
    sprintf(out, "%.*s/%s", (int)(slash - rel), rel, slash + 1 + 2);
    return out;
}

/* apply a decoded AD image to `path` (file or dir, no follow) */
static int apply_sidecar_blob(const char *path, const uint8_t *ad, size_t n) {
    struct ad_xattr *x = NULL;
    size_t nx = 0;
    void *rfork = NULL;
    size_t rflen = 0;
    size_t i;
    int fd, rc = 0;
    if (ad_parse(ad, n, &x, &nx, &rfork, &rflen) != 0)
        return -1;
    fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        fd = open(path, O_RDONLY);
    }
    if (fd < 0) {
        free(x);
        free(rfork);
        return -1;
    }
    for (i = 0; i < nx; i++) {
        if (x[i].name != NULL && x[i].len > 0)
            setxattrbyfd(fd, x[i].name, x[i].value, x[i].len);
    }
    if (rflen > 0)
        setxattrbyfd(fd, "com.apple.ResourceFork", rfork, rflen);
    close(fd);
    if (x != NULL) {
        for (i = 0; i < nx; i++) {
            free((void *)x[i].name);
            free((void *)x[i].value);
        }
        free(x);
    }
    free(rfork);
    return rc;
}

static void touch_path(const char *path, time_t mtime, time_t atime,
                       int is_dir) {
    struct timespec tv[2];
    tv[0].tv_sec = atime;
    tv[0].tv_nsec = 0;
    tv[1].tv_sec = mtime;
    tv[1].tv_nsec = 0;
    if (is_dir) {
        int fd = open(path, O_RDONLY | O_DIRECTORY);
        if (fd >= 0) {
            futimens(fd, tv);
            close(fd);
        }
    } else {
        utimensat(AT_FDCWD, path, tv, AT_SYMLINK_NOFOLLOW);
    }
}

static int mkdirs_for(const char *path) {
    char *p, *c;
    int rc = 0;
    p = strdup(path);
    if (p == NULL)
        return -1;
    c = p + 1;
    while ((c = strchr(c, '/')) != NULL) {
        *c = '\0';
        if (mkdir(p, 0777) != 0 && errno != EEXIST)
            rc = -1;
        *c = '/';
        c++;
    }
    free(p);
    return rc;
}

static int write_new_file(const char *path, const uint8_t *data, size_t n,
                          uint32_t mode) {
    int fd;
    size_t off = 0;
    if (mkdirs_for(path) != 0)
        return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
    if (fd < 0) {
        ditto_error("Can't open %s\n", path);
        return -1;
    }
    while (off < n) {
        ssize_t w = write(fd, data + off, n - off);
        if (w <= 0)
            break;
        off += (size_t)w;
    }
    fchmod(fd, mode & 07777);
    close(fd);
    return off == n ? 0 : -1;
}

typedef struct ilink {
    uint64_t ino;
    char     *path;
    struct ilink *next;
} ilink;

static struct ilink *ilink_find(const struct ilink *h, uint64_t ino) {
    for (; h != NULL; h = h->next)
        if (h->ino == ino)
            return (struct ilink *)h;
    return NULL;
}

static struct ilink *ilink_add(struct ilink **h, uint64_t ino,
                               const char *path) {
    struct ilink *n = malloc(sizeof *n);
    if (n == NULL)
        return NULL;
    n->ino = ino;
    n->path = strdup(path);
    n->next = *h;
    *h = n;
    return n;
}

static void linked_free(struct ilink *h) {
    while (h) {
        struct ilink *n = h->next;
        free(h->path);
        free(h);
        h = n;
    }
}

static int ex_mode_type(uint32_t mode, int *isdir, int *islink) {
    *isdir = 0;
    *islink = 0;
    if (S_ISDIR(mode))
        *isdir = 1;
    else if (S_ISLNK(mode))
        *islink = 1;
    return 0;
}

typedef struct vlink {
    char *name;
    struct vlink *next;
} vlink;

typedef struct vdir {
    char *path;
    time_t mtime, atime;
    int has_sub;
    struct vdir *next;
} vdir;

static vdir *vdir_push(vdir **h, const char *path, time_t mt, time_t at) {
    vdir *n = malloc(sizeof *n);
    if (n == NULL)
        return NULL;
    n->path = strdup(path);
    if (n->path == NULL) {
        free(n);
        return NULL;
    }
    n->mtime = mt;
    n->atime = at;
    n->has_sub = 0;
    n->next = *h;
    *h = n;
    return n;
}

/* Mark the vdir node for the parent of `full` (a newly seen subdirectory) as
 * non-leaf: directories that contain subdirectories are only mtime-restored
 * when their recorded time differs from the archive's first-entry time. */
static void vdir_mark_nonleaf(vdir *h, const char *full) {
    char tmp[4096];
    size_t L = strlen(full);
    while (L > 0 && full[L - 1] == '/')
        L--;
    if (L == 0)
        return;
    while (L > 0 && full[L - 1] != '/')
        L--;
    if (L == 0)
        return;
    while (L > 1 && full[L - 1] == '/')
        L--;
    if (L >= sizeof tmp)
        return;
    memcpy(tmp, full, L);
    tmp[L] = '\0';
    for (; h != NULL; h = h->next)
        if (strcmp(h->path, tmp) == 0)
            h->has_sub = 1;
}

static void vdir_touch_free(vdir *h, time_t base_mt) {
    while (h != NULL) {
        vdir *n = h->next;
        if (!(h->has_sub && h->mtime == base_mt))
            touch_path(h->path, h->mtime, h->atime, 1);
        free(h->path);
        free(h);
        h = n;
    }
}

static void vlink_push(vlink **h, const char *name) {
    vlink *n = malloc(sizeof *n);
    if (n == NULL)
        return;
    n->name = strdup(name);
    n->next = *h;
    *h = n;
}

static void vlink_print_free(vlink *h) {
    while (h != NULL) {
        vlink *n = h->next;
        ditto_info("linked %s\n", h->name);
        free(h->name);
        free(h);
        h = n;
    }
}

static int extract_cpio(const ditto_opts *o, const uint8_t *buf, size_t n,
                        const char *dst, ditto_copy_flags *cf) {
    struct cpio_in in;
    struct cpio_entry ce;
    struct ilink *links = NULL;
    vlink *verb = NULL;
    vdir *dirs = NULL;
    int rc = 0, clean = 0, badfmt = 0;
    time_t base_mt = 0;
    int have_base = 0;

    in.p = buf;
    in.end = buf + n;
    (void)cf;
    errno = 0;
    for (;;) {
        int r;
        if (buf + n - in.p < 76)
            break;
        if (memcmp(in.p, "070707", 6) != 0) {
            badfmt = 1;
            break;
        }
        r = cpio_next(&in, &ce);
        if (r == 0) {
            clean = 1; /* TRAILER: clean end */
            break;
        }
        if (r < 0) {
            if (r == -1)
                badfmt = 1;
            break;
        }
        /* cpio_next has advanced in.p one past this entry's data */
        if (ce.size > (uint64_t)(size_t)(in.p - buf))
            break;
        if (!have_base) {
            base_mt = ce.mtime;
            have_base = 1;
        }
        {
            const uint8_t *data = in.p - ce.size;
            if (ce.name[0] == '\0' || strncmp(ce.name, "TRAILER", 7) == 0) {
                clean = 1;
                break;
            }
            if (ce.name[0] == '.' && ce.name[1] == '\0')
                continue;
            if (is_sidecar_name(ce.name)) {
                char *tgt = sidecar_target(ce.name);
                char *full = malloc(strlen(dst) + strlen(tgt) + 2);
                if (full == NULL) {
                    free(tgt);
                    break;
                }
                if (o->very_verbose) {
                    char b[2200];
                    ditto_info("copying file %s ... \n", ce.name);
                    snprintf(b, sizeof b, "%s__", ce.name);
                    ditto_info("%llu bytes for %s\n",
                               (unsigned long long)ce.size, b);
                }
                sprintf(full, "%s/%s", dst, tgt);
                apply_sidecar_blob(full, data, (size_t)ce.size);
                free(full);
                free(tgt);
                continue;
            }
            {
                char *rel, *full;
                int isdir, islink;
                ex_mode_type(ce.mode, &isdir, &islink);
                rel = clean_rel(ce.name, NULL);
                if (rel == NULL)
                    break;
                full = malloc(strlen(dst) + strlen(rel) + 2);
                if (full == NULL) {
                    free(rel);
                    break;
                }
                sprintf(full, "%s/%s", dst, rel);
                if (isdir) {
                    if (mkdir(full, ce.mode & 0777) != 0 && errno != EEXIST)
                        rc = -1;
                    chmod(full, ce.mode & 07777);
                    if (vdir_push(&dirs, full, ce.mtime, ce.mtime) == NULL)
                        rc = -1;
                    vdir_mark_nonleaf(dirs, full);
                } else if (islink) {
                    if (o->very_verbose) {
                        vlink_push(&verb, ce.name);
                        ditto_info("copying symlink %s ... \n", ce.name);
                    }
                    char *tgt = malloc((size_t)ce.size + 1);
                    char *p;
                    if (tgt != NULL) {
                        memcpy(tgt, data, (size_t)ce.size);
                        tgt[ce.size] = '\0';
                    }
                    p = strdup(full);
                    if (p != NULL) {
                        mkdirs_for(p);
                        free(p);
                    }
                    if (tgt != NULL) {
                        unlink(full);
                        if (symlink(tgt, full) == 0) {
                            /* reference leaves symlink mtime at creation */
                        } else {
                            ditto_error("Can't create link %s\n", full);
                        }
                        free(tgt);
                    }
                } else if (S_ISREG(ce.mode)) {
                    if (o->very_verbose) {
                        ditto_info("copying file %s ... \n", ce.name);
                        ditto_info("%llu bytes for %s\n",
                                   (unsigned long long)ce.size, ce.name);
                    }
                    struct ilink *li = NULL;
                    if (ce.nlink > 1)
                        li = ilink_find(links, ce.ino);
                    if (li != NULL) {
                        if (mkdirs_for(full) == 0) {
                            unlink(full);
                            link(li->path, full);
                            touch_path(full, ce.mtime, ce.mtime, 0);
                        }
                    } else {
                        if (write_new_file(full, data, (size_t)ce.size,
                                           ce.mode) == 0)
                            touch_path(full, ce.mtime, ce.mtime, 0);
                        if (ce.nlink > 1) {
                            if (ilink_add(&links, ce.ino, full) == NULL)
                                rc = -1;
                        }
                    }
                }
                free(full);
                free(rel);
            }
        }
    }
    vdir_touch_free(dirs, base_mt);
    vlink_print_free(verb);
    linked_free(links);
    if (!clean && !badfmt) {
        ditto_error("cpio read error: %s\n", strerror(0));
        rc = -1;
    }
    if (badfmt) {
        ditto_error("cpio read error: bad file format\n");
        rc = -1;
    }
    return rc;
}

static int extract_zip(const ditto_opts *o, const uint8_t *buf, size_t n,
                       const char *dst, ditto_copy_flags *cf) {
    struct zip_in zi;
    struct zip_cent ce;
    vdir *dirs = NULL;
    int rc = 0, one;
    time_t base_mt = 0;
    int have_base = 0;

    (void)cf;
    if (zip_in_open(&zi, buf, n) < 0) {
        ditto_error("Couldn't read PKZip signature\n");
        return -1;
    }
    while ((one = zip_in_next(&zi, &ce)) == 1) {
        char *rel, *full;
        int isdir, islink;
        ex_mode_type(ce.mode, &isdir, &islink);
        if (!have_base) {
            base_mt = ce.mtime;
            have_base = 1;
        }
        rel = clean_rel(ce.name, NULL);
        if (rel == NULL)
            break;
        /* strip trailing slash on dirs */
        if (isdir) {
            size_t L = strlen(rel);
            while (L > 0 && rel[L - 1] == '/')
                rel[--L] = '\0';
        }
        full = malloc(strlen(dst) + strlen(rel) + 2);
        if (full == NULL) {
            free(rel);
            break;
        }
        sprintf(full, "%s/%s", dst, rel);
        {
            /* AppleDouble sidecar: `._` prefixes the basename */
            const char *slash = strrchr(rel, '/');
            const char *base0 = slash != NULL ? slash + 1 : rel;
            if (base0[0] == '.' && base0[1] == '_' && !isdir &&
                base0[2] != '\0') {
                char *base, *bfull;
                if (o->very_verbose) {
                    char b[2200];
                    ditto_info("copying file %s ... \n", ce.name);
                    snprintf(b, sizeof b, "%s__", ce.name);
                    ditto_info("%llu bytes for %s\n", (unsigned long long)zi.dlen,
                               b);
                }
                if (slash != NULL) {
                    size_t L = (size_t)(slash - rel);
                    base = malloc(L + strlen(base0 + 2) + 1);
                    if (base != NULL) {
                        memcpy(base, rel, L);
                        strcpy(base + L, base0 + 2);
                    }
                } else {
                    base = strdup(base0 + 2);
                }
                if (base != NULL) {
                    bfull = malloc(strlen(dst) + strlen(base) + 2);
                    if (bfull != NULL) {
                        sprintf(bfull, "%s/%s", dst, base);
                        apply_sidecar_blob(bfull, zi.data, zi.dlen);
                        free(bfull);
                    }
                    free(base);
                }
            } else if (isdir) {
            if (mkdir(full, ce.mode & 0777) != 0 && errno != EEXIST)
                rc = -1;
            chmod(full, ce.mode & 07777);
            if (vdir_push(&dirs, full, ce.mtime, ce.atime) == NULL)
                rc = -1;
            vdir_mark_nonleaf(dirs, full);
        } else if (islink) {
            if (o->very_verbose) {
                ditto_info("copying file %s ... \n", ce.name);
                ditto_info("%llu bytes for %s\n", (unsigned long long)zi.dlen,
                           ce.name);
            }
            char *tgt = malloc(zi.dlen + 1);
            char *p;
            if (tgt != NULL) {
                memcpy(tgt, zi.data, zi.dlen);
                tgt[zi.dlen] = '\0';
            }
            p = strdup(full);
            if (p != NULL) {
                mkdirs_for(p);
                free(p);
            }
            if (tgt != NULL) {
                unlink(full);
                if (symlink(tgt, full) == 0)
                    /* reference leaves symlink mtime at creation */;
                free(tgt);
            }
        } else {
            if (o->very_verbose) {
                ditto_info("copying file %s ... \n", ce.name);
                ditto_info("%llu bytes for %s\n", (unsigned long long)zi.dlen,
                           ce.name);
            }
            if (write_new_file(full, zi.data, zi.dlen, ce.mode) == 0)
                touch_path(full, ce.mtime, ce.atime, 0);
        }
        }
        free(full);
        free(rel);
    }
    vdir_touch_free(dirs, base_mt);
    zip_in_close(&zi);
    return rc;
}

int ditto_extract(ditto_opts *o, ditto_copy_flags *cf, char *const *srcs,
                  int nsrcs, const char *dst) {
    uint8_t *buf;
    size_t n;
    int zip, i;
    int rc = 0;

    (void)cf;
    for (i = 0; i < nsrcs; i++) {
        if (i > 0 && o->very_verbose)
            ditto_info("\n");
        ditto_print_header(o, srcs[i]);
        buf = read_archive_data(srcs[i], &n);
        if (buf == NULL) {
            ditto_error("%s: %s\n", srcs[i], strerror(errno));
            rc = 1;
            continue;
        }
        if (mkdir(dst, 0777) != 0 && errno != EEXIST) {
            ditto_error("%s: %s\n", dst, strerror(errno));
            free(buf);
            return 1;
        }
        zip = o->zip;
        if (zip)
            extract_zip(o, buf, n, dst, cf);
        else
            extract_cpio(o, buf, n, dst, cf);
        free(buf);
        if (ditto_nerrors > 0)
            rc = 1;
    }
    return rc;
}
