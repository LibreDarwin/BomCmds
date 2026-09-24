/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * Clean-room reimplementation of Apple's /usr/bin/lsbom.
 *
 * Output and error text are byte-identical to the reference tool for the
 * dir-mode BOM files this project produces (verified against macOS 15.4
 * /usr/bin/lsbom).  See FORMAT.md and local/BomCmds.md. */
#include "libbom/bom_read.h"

#include <errno.h>
#include <getopt.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static const char kShortUsage[] =
    "Usage: lsbom [-h] [-s] [-f] [-d] [-l] [-b] [-c] [-m] [-x]\n"
    "    [--arch archVal] [-p parameters] bom ...\n";

static const char kFullUsage[] =
    "Usage: lsbom [-h] [-s] [-f] [-d] [-l] [-b] [-c] [-m] [-x]\n"
    "    [--arch archVal] [-p parameters] bom ...\n"
    "\n"
    "    -h              print full usage\n"
    "    -s              print pathnames only\n"
    "    -f              list files\n"
    "    -d              list directories\n"
    "    -l              list symbolic links\n"
    "    -b              list block devices\n"
    "    -c              list character devices\n"
    "    -m              print modified times\n"
    "    -x              suppress modes for directories and symlinks\n"
    "    --arch archVal  print info for architecture archVal (\"ppc\", \"i386\", \"hppa\", \"sparc\", etc)\n"
    "    -p parameters   print only some of the results.  EACH OPTION CAN ONLY BE USED ONCE\n"
    "\t\tParameters:\n"
    "\t\t\tf\tfile name\n"
    "\t\t\tF\tfile name with quotes (i.e. \"/usr/bin/lsbom\")\n"
    "\t\t\tm\tfile mode (permissions)\n"
    "\t\t\tM\tsymbolic file mode\n"
    "\t\t\tg\tgroup id\n"
    "\t\t\tG\tgroup name\n"
    "\t\t\tu\tuser id\n"
    "\t\t\tU\tuser name\n"
    "\t\t\tt\tmod time\n"
    "\t\t\tT\tformatted mod time\n"
    "\t\t\ts\tfile size\n"
    "\t\t\tS\tformatted size\n"
    "\t\t\tc\t32-bit checksum\n"
    "\t\t\t/\tuser id/group id\n"
    "\t\t\t?\tuser name/group name\n";

/* --arch names map to Mach-O cpu_type values (slice cputype match; the
 * stored per-arch table in each Mach-O PathRecord is consulted, FORMAT.md
 * 4.6).  Matching is by cputype only, so x86_64h (cputype 0x01000007,
 * subtype 8) matches a stored x86_64 slice (subtype 3).  "any" is a valid
 * name whose cpu type matches no stored slice, so it drops every binary
 * row; non-binary rows are never filtered. */
#define CPU_TYPE_POWERPC    0x00000012
#define CPU_TYPE_I386       0x00000007
#define CPU_TYPE_X86_64     0x01000007
#define CPU_TYPE_HPPA       0x0000000b
#define CPU_TYPE_SPARC      0x0000000e
#define CPU_TYPE_POWERPC64  0x01000012
#define CPU_TYPE_ARM64      0x0100000c
#define CPU_TYPE_ANY        0x0fffffff

typedef struct {
    uint32_t val;
    const char *name;
} arch_entry;

static const arch_entry kArchs[] = {
    { CPU_TYPE_POWERPC, "ppc" },   { CPU_TYPE_I386, "i386" },
    { CPU_TYPE_HPPA, "hppa" },     { CPU_TYPE_SPARC, "sparc" },
    { CPU_TYPE_POWERPC64, "ppc64" }, { CPU_TYPE_X86_64, "x86_64" },
    { CPU_TYPE_ARM64, "arm64" },   { CPU_TYPE_ARM64, "arm64e" },
    { CPU_TYPE_X86_64, "x86_64h" },
    { CPU_TYPE_ANY, "any" },
};

typedef struct opts {
    int s;            /* -s pathnames only */
    int f, d, l, b, c;/* -f -d -l -b -c type filters */
    int m;            /* -m print modified times */
    int x;            /* -x suppress modes for dirs and symlinks */
    int arch_set;
    uint32_t arch;
    int have_params;
    char params[64];
} opts;

/* One generated Paths-block row of the main ("Paths", "Tree") variable.
 * name points into the file image.  When --arch matched a stored slice,
 * `slice` points at that 16-byte entry (cputype, subtype, size, checksum)
 * inside the image so rendering prints the per-arch size/checksum. */
typedef struct prow {
    uint32_t pid, parent;
    char *name;
    bom_pathrec pr;
    const uint8_t *slice;
} prow;

static uint16_t r16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t r32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void usage_full(void) {
    fputs(kFullUsage, stderr);
}

static void usage_short(void) {
    fputs(kShortUsage, stderr);
}

/* ---- -p parameter table ---- */

/* Function groups: f and F share "function 0"; every other letter its own. */
static int param_func(char c) {
    switch (c) {
    case 'f': case 'F': return 0;
    case 'm': return 1;
    case 'M': return 2;
    case 'g': return 3;
    case 'G': return 4;
    case 'u': return 5;
    case 'U': return 6;
    case 't': return 7;
    case 'T': return 8;
    case 's': return 9;
    case 'S': return 10;
    case 'c': return 11;
    case '/': return 12;
    case '?': return 13;
    default:  return -1;
    }
}

static int parse_params(opts *o, const char *p) {
    int seen[14] = { 0 };
    size_t i;
    if (strlen(p) >= sizeof(o->params)) {
        fputs("EACH OPTION CAN ONLY BE USED ONCE.\n", stderr);
        usage_short();
        return -1;
    }
    for (i = 0; p[i] != '\0'; i++) {
        int f = param_func(p[i]);
        if (f < 0) {
            fprintf(stderr, "Unknown print option '%c'\n", p[i]);
            usage_short();
            return -1;
        }
        if (seen[f]) {
            fputs("EACH OPTION CAN ONLY BE USED ONCE.\n", stderr);
            usage_short();
            return -1;
        }
        seen[f] = 1;
    }
    strcpy(o->params, p);
    o->have_params = 1;
    return 0;
}

static int parse_arch(opts *o, const char *name) {
    size_t i;
    for (i = 0; i < sizeof(kArchs) / sizeof(kArchs[0]); i++) {
        if (strcmp(kArchs[i].name, name) == 0) {
            o->arch = kArchs[i].val;
            o->arch_set = 1;
            return 0;
        }
    }
    fprintf(stderr, "Unrecognized architecture %s\n", name);
    usage_short();
    return -1;
}

static int type_filter(const opts *o, uint8_t pt) {
    if (!o->f && !o->d && !o->l && !o->b && !o->c)
        return 1;
    switch (pt) {
    case BM_PT_FILE:  return o->f;
    case BM_PT_DIR:   return o->d;
    case BM_PT_LINK:  return o->l;
    case BM_PT_BLOCK: return o->b;
    case BM_PT_CHAR:  return o->c;
    default:          return 0;
    }
}

/* ---- tree load ---- */

static int find_var_block(const bom_file *bf, const char *want) {
    uint32_t voff, vlen, count, i;
    const uint8_t *p;
    size_t wantlen;
    if (bf->len < 32)
        return 0;
    voff = r32(bf->data + 24);
    vlen = r32(bf->data + 28);
    if (voff == 0 || voff > bf->len || vlen < 4 || vlen > bf->len - voff)
        return 0;
    count = r32(bf->data + voff);
    p = bf->data + voff + 4;
    wantlen = strlen(want);
    for (i = 0; i < count; i++) {
        uint32_t b;
        uint8_t nl;
        if ((size_t)(p - (bf->data + voff)) + 5 > vlen)
            return 0;
        b = r32(p);
        nl = p[4];
        if (p + 5 + nl > bf->data + voff + vlen)
            return 0;
        if (nl == wantlen && memcmp(p + 5, want, nl) == 0)
            return b != 0 ? (int)b : 0;
        p += 5 + nl;
    }
    return 0;
}

static int load_tree(const bom_file *bf, prow **out, size_t *nout,
                     const opts *o) {
    int tree_idx = find_var_block(bf, "Paths");
    const uint8_t *tree, *pb;
    uint32_t tree_len, pb_len;
    prow *rows = NULL;
    size_t n = 0, cap = 0;

    if (tree_idx <= 0)
        return 0;
    tree = bom_block(bf, (uint32_t)tree_idx, &tree_len);
    if (tree == NULL || tree_len < 12)
        return 0;
    pb = bom_block(bf, r32(tree + 8), &pb_len);
    while (pb != NULL) {
        uint32_t next, count, i;
        int is_pi;
        if (pb_len < 12)
            break;
        is_pi = r16(pb);
        count = r16(pb + 2);
        next = r32(pb + 4);
        if (is_pi == 1) {
            for (i = 0; i < count; i++) {
                uint32_t piib, fblk, prl, fl;
                const uint8_t *pii, *fb, *prb;
                const char *name;
                size_t nl;
                uint32_t pid, parent;
                prow row;
                if (12 + 8 * i + 8 > pb_len)
                    goto done;
                pii = pb + 12 + 8 * i;
                piib = r32(pii);
                fblk = r32(pii + 4);
                pii = bom_block(bf, piib, &prl);
                fb = bom_block(bf, fblk, &fl);
                if (pii == NULL || prl < 8 || fb == NULL || fl < 5)
                    continue;
                pid = r32(pii);
                parent = r32(fb);
                name = (const char *)fb + 4;
                nl = 0;
                while (nl < fl - 4 && name[nl] != '\0')
                    nl++;
                if (name[nl] != '\0')
                    continue;
                prb = bom_block(bf, r32(pii + 4), &prl);
                row.pid = pid;
                row.parent = parent;
                row.name = (char *)name;
                row.pr.link = NULL;
                row.pr.link_len = 0;
                row.pr.valid = 0;
                row.slice = NULL;
                if (bom_pathrec_decode(prb, prl, &row.pr) != 0)
                    continue;
                /* With --arch, a Mach-O row must contain a stored slice
                 * whose cputype matches; otherwise the row is dropped
                 * entirely (not zeroed).  Non-binary rows are never
                 * filtered. */
                if (o->arch_set && row.pr.nslice > 0) {
                    uint32_t j;
                    int found = 0;
                    for (j = 0; j < row.pr.nslice; j++) {
                        const uint8_t *s = row.pr.slices + 16 * j;
                        if (r32(s) == o->arch) {
                            row.slice = s;
                            found = 1;
                            break;
                        }
                    }
                    if (!found)
                        continue;
                }
                if (n == cap) {
                    size_t ncap = cap ? cap * 2 : 256;
                    prow *nr = (prow *)realloc(rows, ncap * sizeof *nr);
                    if (nr == NULL)
                        goto done;
                    rows = nr;
                    cap = ncap;
                }
                rows[n++] = row;
            }
        }
        if (next == 0)
            break;
        pb = bom_block(bf, next, &pb_len);
    }
done:
    *out = rows;
    *nout = n;
    return 0;
}

/* ---- rendering ---- */

/* ctime()-style local time: "Thu Sep 24 03:09:47 2026", English always
 * (asctime_r is not locale-dependent). */
static void fmt_time(uint32_t when, char *buf, size_t sz) {
    time_t t = (time_t)when;
    struct tm tm;
    char tmp[32];
    size_t n;
    if (localtime_r(&t, &tm) == NULL) {
        buf[0] = '\0';
        return;
    }
    asctime_r(&tm, tmp);
    n = strlen(tmp);
    if (n > 0 && tmp[n - 1] == '\n')
        tmp[--n] = '\0';
    snprintf(buf, sz, "%s", tmp);
}

static void fmt_grouped(char *buf, size_t sz, uint32_t v) {
    char tmp[16];
    int n = snprintf(tmp, sizeof tmp, "%u", v);
    int i, out = 0;
    for (i = 0; i < n && (size_t)(out + 1) < sz; i++) {
        if (i > 0 && (n - i) % 3 == 0)
            buf[out++] = ',';
        buf[out++] = tmp[i];
    }
    buf[out] = '\0';
}

static void usr_name(uint32_t uid, char *buf, size_t sz) {
    struct passwd *pw = getpwuid(uid);
    if (pw != NULL && pw->pw_name != NULL)
        snprintf(buf, sz, "%s", pw->pw_name);
    else
        snprintf(buf, sz, "<unknown>");
}

static void grp_name(uint32_t gid, char *buf, size_t sz) {
    struct group *gr = getgrgid(gid);
    if (gr != NULL && gr->gr_name != NULL)
        snprintf(buf, sz, "%s", gr->gr_name);
    else
        snprintf(buf, sz, "<unknown>");
}

static const char *sym_mode(uint16_t m, char *buf) {
    char t;
    if (S_ISDIR(m))
        t = 'd';
    else if (S_ISREG(m))
        t = '-';
    else if (S_ISLNK(m))
        t = 'l';
    else if (S_ISBLK(m))
        t = 'b';
    else if (S_ISCHR(m))
        t = 'c';
    else if (S_ISFIFO(m))
        t = 'p';
    else if (S_ISSOCK(m))
        t = 's';
    else
        t = '?';
    buf[0] = t;
    buf[1] = (m & S_IRUSR) ? 'r' : '-';
    buf[2] = (m & S_IWUSR) ? 'w' : '-';
    buf[3] = (m & S_ISUID) ? (m & S_IXUSR) ? 's' : 'S'
                            : (m & S_IXUSR) ? 'x' : '-';
    buf[4] = (m & S_IRGRP) ? 'r' : '-';
    buf[5] = (m & S_IWGRP) ? 'w' : '-';
    buf[6] = (m & S_ISGID) ? (m & S_IXGRP) ? 's' : 'S'
                            : (m & S_IXGRP) ? 'x' : '-';
    buf[7] = (m & S_IROTH) ? 'r' : '-';
    buf[8] = (m & S_IWOTH) ? 'w' : '-';
    buf[9] = (m & S_ISVTX) ? (m & S_IXOTH) ? 't' : 'T'
                            : (m & S_IXOTH) ? 'x' : '-';
    buf[10] = ' ';
    buf[11] = '\0';
    return buf;
}

/* Value surface used for printing.  With --arch the matched slice's
 * size/checksum replace the stored whole-file values; mode/uid/gid/mtime
 * stay as stored.  Without --arch the stored values are used unchanged
 * (verified: stored table, not the live file). */
static void pr_view(const prow *n, bom_pathrec *v) {
    *v = n->pr;
    if (n->slice != NULL) {
        v->size = r32(n->slice + 8);
        v->checksum = r32(n->slice + 12);
    }
}

static void row_columns(const opts *o, const prow *n, const char *path,
                        FILE *out) {
    const char *params = o->params;
    bom_pathrec v;
    int first = 1;
    pr_view(n, &v);
    while (*params != '\0') {
        char c = *params++;
        char tmp[64], un[64], gn[64];
        /* Apple's columner drops an empty 'S' cell entirely: no value, no
         * separator, and it does not advance the "first" flag.  All other
         * empty fields (t/T/c/s) still occupy their tab cell. */
        if (c == 'S' && v.path_type == BM_PT_DIR)
            continue;
        if (!first)
            fputc('\t', out);
        first = 0;
        switch (c) {
        case 'f': fprintf(out, "%s", path); break;
        case 'F': fprintf(out, "\"%s\"", path); break;
        case 'm': fprintf(out, "%o", v.mode); break;
        case 'M': fprintf(out, "%s", sym_mode(v.mode, tmp)); break;
        case 'g': fprintf(out, "%u", v.gid); break;
        case 'G': grp_name(v.gid, tmp, sizeof tmp); fputs(tmp, out); break;
        case 'u': fprintf(out, "%u", v.uid); break;
        case 'U': usr_name(v.uid, tmp, sizeof tmp); fputs(tmp, out); break;
        case 't':
            if (v.path_type != BM_PT_DIR)
                fprintf(out, "%u", v.mtime);
            break;
        case 'T':
            if (v.path_type != BM_PT_DIR) {
                fmt_time(v.mtime, tmp, sizeof tmp);
                fputs(tmp, out);
            }
            break;
        case 's':
            if (v.path_type != BM_PT_DIR)
                fprintf(out, "%u", v.size);
            break;
        case 'S':
            if (v.path_type != BM_PT_DIR) {
                fmt_grouped(tmp, sizeof tmp, v.size);
                fputs(tmp, out);
            }
            break;
        case 'c':
            if (v.path_type != BM_PT_DIR)
                fprintf(out, "%u", v.checksum);
            break;
        case '/': fprintf(out, "%u/%u", v.uid, v.gid); break;
        case '?':
            usr_name(v.uid, un, sizeof un);
            grp_name(v.gid, gn, sizeof gn);
            fprintf(out, "%s/%s", un, gn);
            break;
        default: break;
        }
    }
    fputc('\n', out);
}

static void print_node(const opts *o, const prow *n, const char *path,
                       FILE *out) {
    bom_pathrec v;
    pr_view(n, &v);

    if (o->s) {
        fputs(path, out);
        fputc('\n', out);
        return;
    }
    if (o->have_params) {
        row_columns(o, n, path, out);
        return;
    }

    fputs(path, out);
    if (!(o->x && (v.path_type == BM_PT_DIR || v.path_type == BM_PT_LINK)))
        fprintf(out, "\t%o", v.mode);
    fprintf(out, "\t%u/%u", v.uid, v.gid);
    if (v.path_type == BM_PT_DIR) {
        /* no size / checksum for directories */
    } else if (v.path_type == BM_PT_BLOCK || v.path_type == BM_PT_CHAR) {
        fprintf(out, "\t%d", (int32_t)v.checksum);
    } else {
        fprintf(out, "\t%u\t%u", v.size, v.checksum);
    }
    if (v.link_len > 0 && v.link != NULL)
        fprintf(out, "\t%.*s", (int)v.link_len, (const char *)v.link);
    if (o->m && v.path_type == BM_PT_FILE) {
        char tb[64];
        fmt_time(v.mtime, tb, sizeof tb);
        fprintf(out, "\t%s", tb);
    }
    fputc('\n', out);
}

static void walk(const opts *o, const prow *rows, size_t n, const prow *cur,
                 const char *prefix, FILE *out) {
    size_t i;
    char *path;
    size_t plen;
    int print_it = 1;

    if (cur->pr.path_type == BM_PT_FIFO || cur->pr.path_type == BM_PT_SOCKET) {
        fprintf(stderr, "filesystem object has an invalid type: 0x%x\n",
                cur->pr.path_type);
        fprintf(stderr, "Cannot dearchive.\n");
        return;
    }
    if (cur->pid == 1) {
        path = strdup(".");
        plen = 1;
    } else {
        plen = strlen(prefix) + 1 + strlen(cur->name);
        path = (char *)malloc(plen + 1);
        if (path == NULL)
            return;
        snprintf(path, plen + 1, "%s/%s", prefix, cur->name);
    }
    if (!type_filter(o, cur->pr.path_type))
        print_it = 0;
    if (print_it)
        print_node(o, cur, path, out);
    for (i = 0; i < n; i++) {
        if (rows[i].parent == cur->pid)
            walk(o, rows, n, &rows[i], path, out);
    }
    free(path);
}

static void process(const opts *o, const char *path) {
    bom_file bf;
    prow *rows = NULL;
    size_t n = 0, i;
    int root = -1;

    if (bom_file_open(path, &bf) != 0) {
        int e = errno;
        if (bf.open_failed)
            fprintf(stderr, "can't open %s: %s\n", path, strerror(e));
        else
            fprintf(stderr, "read: %s\ncan't read from %s\n", strerror(e),
                    path);
        fprintf(stderr, "**** Can't open %s.\n", path);
        return;
    }
    load_tree(&bf, &rows, &n, o);
    for (i = 0; i < n; i++) {
        if (rows[i].pid == 1) {
            root = (int)i;
            break;
        }
    }
    if (root >= 0)
        walk(o, rows, n, &rows[root], "", stdout);
    free(rows);
    bom_file_close(&bf);
}

int main(int argc, char *argv[]) {
    opts o;
    int ch, i;
    static const struct option lopts[] = {
        { "arch", required_argument, NULL, 'A' },
        { NULL, 0, NULL, 0 },
    };

    memset(&o, 0, sizeof o);
    if (argc == 1) {
        usage_full();
        return 1;
    }
    while ((ch = getopt_long(argc, argv, "hsfdlbcmxp:", lopts, NULL)) != -1) {
        switch (ch) {
        case 'h':
            usage_full();
            return 1;
        case 's': o.s = 1; break;
        case 'f': o.f = 1; break;
        case 'd': o.d = 1; break;
        case 'l': o.l = 1; break;
        case 'b': o.b = 1; break;
        case 'c': o.c = 1; break;
        case 'm': o.m = 1; break;
        case 'x': o.x = 1; break;
        case 'p':
            if (parse_params(&o, optarg) != 0)
                return 1;
            break;
        case 'A':
            if (parse_arch(&o, optarg) != 0)
                return 1;
            break;
        default:
            usage_short();
            return 1;
        }
    }
    if (optind == argc) {
        usage_short();
        return 1;
    }
    for (i = optind; i < argc; i++)
        process(&o, argv[i]);
    return 0;
}