/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * Minimal Mach-O recognition and fat-file slicing for ditto --arch
 * (FORMAT.md 15).  Only the magic words and cpu type/subtype fields are
 * inspected; slices are copied byte-for-byte. */
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <mach/machine.h>
#else
typedef int cpu_type_t;
typedef int cpu_subtype_t;
#define CPU_ARCH_ABI64 0x01000000
#define CPU_TYPE_ANY   ((cpu_type_t)-1)
#define CPU_TYPE_POWERPC ((cpu_type_t)18)
#define CPU_TYPE_POWERPC64 ((cpu_type_t)(CPU_ARCH_ABI64 | 18))
#define CPU_TYPE_I386   ((cpu_type_t)7)
#define CPU_TYPE_X86_64 ((cpu_type_t)(CPU_ARCH_ABI64 | 7))
#define CPU_TYPE_ARM64  ((cpu_type_t)(CPU_ARCH_ABI64 | 12))
#define CPU_TYPE_ARM64E CPU_TYPE_ARM64
#define CPU_TYPE_ARM    ((cpu_type_t)12)
#define CPU_TYPE_HPPA   ((cpu_type_t)11)
#define CPU_TYPE_SPARC  ((cpu_type_t)14)
#define CPU_TYPE_MC680x0 ((cpu_type_t)6)
#define CPU_SUBTYPE_POWERPC_ALL 0
#define CPU_SUBTYPE_I386_ALL    3
#define CPU_SUBTYPE_X86_64_ALL  3
#define CPU_SUBTYPE_ARM64_ALL   0
#define CPU_SUBTYPE_ARM64E      2
#endif

#include "ditto.h"

/* arch table (name, cputype, cpusubtype, match_everything) */
struct archtab {
    const char *name;
    cpu_type_t  cputype;
    cpu_subtype_t cpusubtype;
    int everything;
};

static const struct archtab archs[] = {
    { "ppc",    CPU_TYPE_POWERPC,   CPU_SUBTYPE_POWERPC_ALL, 0 },
    { "ppc64",  CPU_TYPE_POWERPC64, CPU_SUBTYPE_POWERPC_ALL, 0 },
    { "i386",   CPU_TYPE_I386,      CPU_SUBTYPE_I386_ALL, 0 },
    { "x86_64", CPU_TYPE_X86_64,    CPU_SUBTYPE_X86_64_ALL, 0 },
    { "arm",    CPU_TYPE_ARM,       0, 0 },
    { "arm64",  CPU_TYPE_ARM64,     CPU_SUBTYPE_ARM64_ALL, 0 },
    { "arm64e", CPU_TYPE_ARM64,     CPU_SUBTYPE_ARM64E, 0 },
    { "hppa",   CPU_TYPE_HPPA,      0, 0 },
    { "sparc",  CPU_TYPE_SPARC,     0, 0 },
    { "m68k",   CPU_TYPE_MC680x0,   0, 0 },
    { "any",    CPU_TYPE_ANY,       0, 1 },
    { NULL,     0,                  0, 0 },
};

int macho_arch_lookup(const char *name, uint32_t *cputype_val,
                      int *match_everything) {
    const struct archtab *a;
    for (a = archs; a->name; a++) {
        if (strcmp(a->name, name) == 0) {
            if (cputype_val)
                *cputype_val = (uint32_t)a->cputype;
            if (match_everything)
                *match_everything = a->everything;
            return 0;
        }
    }
    return -1;
}

/* name for a (cputype, subtype) pair, if our table knows it */
static const char *arch_name(cpu_type_t ct, cpu_subtype_t st) {
    const struct archtab *a;
    for (a = archs; a->name; a++) {
        if (a->everything)
            continue;
        if (a->cputype == ct && (a->cpusubtype == 0 || a->cpusubtype == st))
            return a->name;
    }
    return NULL;
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int macho_scan(const uint8_t *buf, size_t len, struct macho_fat *mf) {
    uint32_t magic, cputype, cpusubtype, off, sz, align;
    size_t i;
    int big;
    size_t entlen;

    memset(mf, 0, sizeof *mf);
    if (len < 4)
        return 1;
    magic = be32(buf);
    if (magic == 0xcafebabe) {
        big = 0;
    } else if (buf[0] == 0xca && buf[1] == 0xfe && buf[2] == 0xba &&
               buf[3] == 0xbe) {
        big = 1;
        magic = 0xcafebabe;
    } else if (magic == 0xcafebabf) {
        big = 0;
    } else if (buf[0] == 0xca && buf[1] == 0xfe && buf[2] == 0xba &&
               buf[3] == 0xbf) {
        big = 1;
        magic = 0xcafebabf;
    } else {
        return 1; /* not a fat file */
    }
    mf->is_fat = 1;
    mf->big_endian = big;
    entlen = (magic == 0xcafebabf) ? 32 : 20;

    if (big) {
        cputype = be32(buf + 4);
    } else {
        cputype = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
                  ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    }
    if (len < 8 + entlen || cputype > 16 || cputype == 0)
        return -1;
    mf->fat_count = cputype;
    if (mf->fat_count > 16)
        mf->fat_count = 16;

    for (i = 0; i < mf->fat_count; i++) {
        const uint8_t *e = buf + 8 + i * entlen;
        if (big) {
            cputype = (cpu_type_t)be32(e + 0);
            cpusubtype = (cpu_subtype_t)be32(e + 4);
            off = be32(e + 8);
            sz = be32(e + 12);
        } else {
            cputype = (cpu_type_t)((uint32_t)e[0] | ((uint32_t)e[1] << 8) |
                                   ((uint32_t)e[2] << 16) |
                                   ((uint32_t)e[3] << 24));
            cpusubtype = (cpu_subtype_t)((uint32_t)e[4] |
                                         ((uint32_t)e[5] << 8) |
                                         ((uint32_t)e[6] << 16) |
                                         ((uint32_t)e[7] << 24));
            off = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                  ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
            sz = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
                 ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
        }
        (void)align;
        if (entlen == 32) {
            if (big)
                align = be32(e + 16);
            else
                align = (uint32_t)e[16] | ((uint32_t)e[17] << 8) |
                        ((uint32_t)e[18] << 16) | ((uint32_t)e[19] << 24);
        }
        if (off + sz > len)
            return -1;
        snprintf(mf->cpu_names[i], sizeof mf->cpu_names[i], "%s",
                 arch_name(cputype, cpusubtype) ? arch_name(cputype, cpusubtype)
                                                : "?");
        mf->slice_off[i] = off;
        mf->slice_len[i] = sz;
    }
    return 0;
}

/* thin Mach-O?  returns matching arch name or NULL */
static const char *thin_arch(const uint8_t *buf, size_t len) {
    uint32_t magic, cputype = 0, cpusubtype = 0;
    const struct archtab *a;
    if (len < 12)
        return NULL;
    magic = be32(buf);
    if (magic == 0xfeedface || magic == 0xfeedfacf ||
        magic == 0xcefaedfe || magic == 0xcffaedfe) {
        int big = (magic == 0xcefaedfe || magic == 0xcffaedfe);
        if (big) {
            cputype = be32(buf + 4);
            cpusubtype = be32(buf + 8);
        } else {
            cputype = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
                      ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
            cpusubtype = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8) |
                         ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
        }
    } else {
        return NULL;
    }
    for (a = archs; a->name; a++) {
        if (a->cputype == (cpu_type_t)cputype &&
            (a->cpusubtype == 0 || a->cpusubtype == (cpu_subtype_t)cpusubtype))
            return a->name;
    }
    return NULL;
}

int macho_is_thin_for(const uint8_t *buf, size_t len, const char *arch) {
    const char *ta = thin_arch(buf, len);
    int all;
    if (ta == NULL)
        return 0;
    if (macho_arch_lookup(arch, NULL, &all) != 0)
        return 0;
    if (all)
        return 1;
    return strcmp(ta, arch) == 0;
}

int ditto_thin_build(const uint8_t *src, size_t len, const char *const *archs,
                     size_t narchs, uint8_t **out, size_t *outlen) {
    struct macho_fat mf;
    int rc;
    size_t ai;

    *out = NULL;
    *outlen = 0;

    rc = macho_scan(src, len, &mf);
    if (rc == -1)
        return -1;
    if (mf.is_fat) {
        /* collect the requested slices in arch order (dedup offsets) */
        size_t total = 0;
        size_t i;
        for (ai = 0; ai < narchs; ai++) {
            for (i = 0; i < mf.fat_count; i++) {
                if (strcmp(mf.cpu_names[i], archs[ai]) == 0)
                    total += mf.slice_len[i];
            }
        }
        if (total == 0)
            return 1; /* no matching slice */
        {
            uint8_t *img = malloc(total ? total : 1);
            size_t used = 0;
            if (img == NULL)
                return -1;
            for (ai = 0; ai < narchs; ai++) {
                for (i = 0; i < mf.fat_count; i++) {
                    if (strcmp(mf.cpu_names[i], archs[ai]) == 0) {
                        memcpy(img + used, src + mf.slice_off[i],
                               mf.slice_len[i]);
                        used += mf.slice_len[i];
                    }
                }
            }
            *out = img;
            *outlen = used;
            return 0;
        }
    }

    /* non-fat: thin Mach-O must match, everything else copies verbatim */
    if (thin_arch(src, len) != NULL) {
        int matched = 0;
        for (ai = 0; ai < narchs; ai++) {
            int all;
            if (macho_arch_lookup(archs[ai], NULL, &all) != 0)
                continue;
            if (all || macho_is_thin_for(src, len, archs[ai])) {
                matched = 1;
                break;
            }
        }
        if (!matched)
            return 1; /* skip this file */
    }
    {
        uint8_t *img = malloc(len ? len : 1);
        if (img == NULL)
            return -1;
        memcpy(img, src, len);
        *out = img;
        *outlen = len;
        return 0;
    }
}