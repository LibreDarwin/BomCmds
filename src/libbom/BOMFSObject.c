/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * Clean-room BOMFSObject: a filesystem object node of the path tree plus
 * the summary rendering used by lsbom (byte-identical to Apple's output;
 * the rendering logic mirrors the verified src/lsbom/lsbom.c columner). */
#include "bom_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <sys/stat.h>

uint16_t B_ALLDATA = 0x000f;
uint16_t B_PATHONLY = 0x0000;

/* ---- help ---- */

/* ---- constructor / free ---- */

BOMFSObject *BOMFSObjectNewWithSys(uint32_t type, BOMSys *sys) {
    BOMFSObject *o;
    (void)sys;
    o = (BOMFSObject *)calloc(1, sizeof(BOMFSObject));
    if (o == NULL)
        return NULL;
    o->type = type;
    return o;
}

void BOMFSObjectFree(BOMFSObject *obj) {
    if (obj == NULL)
        return;
    if (obj->own_path)
        free(obj->pathname);
    if (obj->own_short)
        free(obj->shortname);
    free(obj->link);
    free(obj->slices);
    free(obj->opaque);
    free(obj);
}

/* ---- accessors ---- */

uint32_t BOMFSObjectType(BOMFSObject *obj) {
    if (obj == NULL)
        return 0;
    return obj->type;
}

uint32_t BOMFSObjectMode(BOMFSObject *obj) {
    if (obj == NULL)
        return 0;
    return obj->mode;
}

const char *BOMFSObjectPathName(BOMFSObject *obj) {
    if (obj == NULL)
        return NULL;
    return obj->pathname ? obj->pathname : obj->shortname;
}

void *BOMFSObjectOpaqueData(BOMFSObject *obj) {
    if (obj == NULL)
        return NULL;
    return (void *)obj->opaque;
}

size_t BOMFSObjectOpaqueDataSize(BOMFSObject *obj) {
    if (obj == NULL)
        return 0;
    return obj->opaque_len;
}

int BOMFSObjectIsBinaryObject(BOMFSObject *obj) {
    if (obj == NULL)
        return 0;
    return obj->binary != 0;
}

int BOMFSObjectContainsArchitecture(BOMFSObject *obj, uint32_t cputype) {
    uint32_t i;
    if (obj == NULL)
        return 0;
    if (!obj->binary)
        return 0;
    for (i = 0; i < obj->nslice; i++)
        if (_bom_r32(obj->slices + 16 * i) == cputype)
            return 1;
    return 0;
}

/* ---- shared internal constructors (used by BOMBom.c) ---- */

void _bom_fso_from_pathrec(BOMFSObject *o, const bom_pathrec *pr) {
    if (o == NULL || pr == NULL || !pr->valid)
        return;
    switch (pr->path_type) {
    case BM_PT_FILE: o->type = BOM_TYPE_FILE; break;
    case BM_PT_DIR:  o->type = BOM_TYPE_DIR; break;
    case BM_PT_LINK: o->type = BOM_TYPE_LINK; break;
    default:         o->type = BOM_TYPE_DEVICE; break;
    }
    o->mode = pr->mode;
    o->uid = pr->uid;
    o->gid = pr->gid;
    o->mtime = pr->mtime;
    o->size = pr->size;
    o->checksum = pr->checksum;
    if (pr->link != NULL && pr->link_len > 0) {
        o->link = (char *)malloc(pr->link_len + 1);
        if (o->link != NULL) {
            memcpy(o->link, pr->link, pr->link_len);
            o->link[pr->link_len] = '\0';
            o->link_len = pr->link_len;
        }
    }
    if (pr->nslice > 0 && pr->slices != NULL) {
        uint32_t i;
        o->nslice = pr->nslice;
        o->slices = (uint8_t *)malloc(16 * (size_t)pr->nslice);
        if (o->slices != NULL) {
            for (i = 0; i < pr->nslice; i++)
                memcpy(o->slices + 16 * i, pr->slices + 16 * i, 16);
            o->binary = 1;
        }
    }
}

BOMFSObject *_bom_fso_from_row(const struct bom_tree_row *row) {
    BOMFSObject *o;
    if (row == NULL)
        return NULL;
    o = BOMFSObjectNewWithSys(BOM_TYPE_FILE, NULL);
    if (o == NULL)
        return NULL;
    _bom_fso_from_pathrec(o, &row->pr);
    if (row->path != NULL)
        BOMFSObjectSetPathName(o, row->path, 1);
    if (row->leaf != NULL)
        BOMFSObjectSetShortName(o, row->leaf, 1);
    o->flags = B_ALLDATA;
    return o;
}

/* ---- setters ---- */

void BOMFSObjectSetFlags(BOMFSObject *obj, uint32_t flags) {
    if (obj == NULL)
        return;
    obj->flags = (uint16_t)flags;
    if ((obj->flags & B_ALLDATA) == B_ALLDATA) {
        /* full metadata mode: nothing extra to do */
    } else {
        /* path-only: drop metadata */
        obj->size = 0;
        obj->checksum = 0;
    }
}

void BOMFSObjectSetPathName(BOMFSObject *obj, const char *path, int copy) {
    if (obj == NULL || path == NULL)
        return;
    if (obj->own_path)
        free(obj->pathname);
    if (copy) {
        obj->pathname = strdup(path);
        obj->own_path = 1;
    } else {
        obj->pathname = (void *)(uintptr_t)path;
        obj->own_path = 0;
    }
}

void BOMFSObjectSetShortName(BOMFSObject *obj, const char *name, int copy) {
    if (obj == NULL || name == NULL)
        return;
    if (obj->own_short)
        free(obj->shortname);
    if (copy) {
        obj->shortname = strdup(name);
        obj->own_short = 1;
    } else {
        obj->shortname = (void *)(uintptr_t)name;
        obj->own_short = 0;
    }
}

/* ---- parse one lsbom line ---- */

/* Elements: path, mode, uid/gid, size/checksum, optional link.  Fills the
 * object's mode/uid/gid and returns a summary we did not need. */
BOMFSObject *BOMFSObjectParseSummary(const char *line) {
    BOMFSObject *o;
    char *copy, *p, *q;
    if (line == NULL)
        return NULL;
    o = BOMFSObjectNewWithSys(BOM_TYPE_FILE, NULL);
    if (o == NULL)
        return NULL;
    copy = strdup(line);
    if (copy == NULL) {
        BOMFSObjectFree(o);
        return NULL;
    }
    /* path */
    for (p = copy; *p != '\0' && *p != '\t'; p++)
        ;
    *p = '\0';
    BOMFSObjectSetPathName(o, copy, 1);
    /* mode */
    p++;
    o->mode = (uint32_t)strtoul(p, &q, 8);
    /* uid/gid */
    p = q;
    while (*p == '\t')
        p++;
    o->uid = (uint32_t)strtoul(p, &q, 10);
    o->gid = (uint32_t)strtoul(q + ((*q == '/') ? 1 : 0), &q, 10);
    free(copy);
    return o;
}

/* ---- summary rendering (mirrors lsbom.c columner) ---- */

static void fmt_time_big(uint32_t when, char *buf, size_t sz) {
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

static void fmt_grouped_big(char *buf, size_t sz, uint32_t v) {
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

static void usr_name_big(uint32_t uid, char *buf, size_t sz) {
    struct passwd *pw = getpwuid(uid);
    if (pw != NULL && pw->pw_name != NULL)
        snprintf(buf, sz, "%s", pw->pw_name);
    else
        snprintf(buf, sz, "<unknown>");
}

static void grp_name_big(uint32_t gid, char *buf, size_t sz) {
    struct group *gr = getgrgid(gid);
    if (gr != NULL && gr->gr_name != NULL)
        snprintf(buf, sz, "%s", gr->gr_name);
    else
        snprintf(buf, sz, "<unknown>");
}

static const char *sym_mode_big(uint16_t m, char *buf) {
    char t;
    if (m == 0)
        m = 0;
    switch (m & 0170000) {
    case 0040000: t = 'd'; break;
    case 0100000: t = '-'; break;
    case 0120000: t = 'l'; break;
    case 0060000: t = 'b'; break;
    case 0020000: t = 'c'; break;
    case 0010000: t = 'p'; break;
    case 0140000: t = 's'; break;
    default:
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
    }
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

/* One default lsbom line for the object (path, mode, uid/gid, size/checksum
 * or device number, symlink target). */
static void summary_default(const BOMFSObject *o, const char *path,
                            char *buf, size_t sz) {
    char tmp[64];
    size_t off;
    off = (size_t)snprintf(buf, sz, "%s\t%o\t%u/%u", path, o->mode, o->uid,
                           o->gid);
    if (o->type == BOM_TYPE_DEVICE) {
        snprintf(buf + off, sz - off, "\t%d", (int32_t)o->checksum);
    } else if (o->type != BOM_TYPE_DIR) {
        snprintf(buf + off, sz - off, "\t%u\t%u", o->size, o->checksum);
    }
    if (o->link != NULL && o->link_len > 0)
        snprintf(buf + strlen(buf), sz - strlen(buf), "\t%.*s",
                 (int)o->link_len, o->link);
    (void)tmp;
}

static void row_columns_big(const BOMFSObject *o, const char *path,
                            const char *params, int32_t arch, char *buf,
                            size_t sz) {
    char tmp[64], un[64], gn[64];
    size_t off = 0;
    int first = 1;
    const uint8_t *slice = NULL;
    uint32_t vsize = o->size, vcksum = o->checksum;
    uint32_t is_dir = (o->type == BOM_TYPE_DIR);
    (void)tmp;
    if (arch >= 0 && o->binary) {
        uint32_t i;
        for (i = 0; i < o->nslice; i++) {
            if ((int32_t)_bom_r32(o->slices + 16 * i) == arch) {
                slice = o->slices + 16 * i;
                break;
            }
        }
    }
    if (slice != NULL) {
        vsize = _bom_r32(slice + 8);
        vcksum = _bom_r32(slice + 12);
    }
    while (*params != '\0') {
        char c = *params++;
        if (c == 'S' && is_dir)
            continue;
        if (!first)
            buf[off++] = '\t';
        first = 0;
        switch (c) {
        case 'f': off += (size_t)snprintf(buf + off, sz - off, "%s", path); break;
        case 'F': off += (size_t)snprintf(buf + off, sz - off, "\"%s\"", path); break;
        case 'm': off += (size_t)snprintf(buf + off, sz - off, "%o", o->mode); break;
        case 'M': sym_mode_big((uint16_t)o->mode, tmp);
                  off += (size_t)snprintf(buf + off, sz - off, "%s", tmp); break;
        case 'g': off += (size_t)snprintf(buf + off, sz - off, "%u", o->gid); break;
        case 'G': grp_name_big(o->gid, tmp, sizeof tmp);
                  off += (size_t)snprintf(buf + off, sz - off, "%s", tmp); break;
        case 'u': off += (size_t)snprintf(buf + off, sz - off, "%u", o->uid); break;
        case 'U': usr_name_big(o->uid, tmp, sizeof tmp);
                  off += (size_t)snprintf(buf + off, sz - off, "%s", tmp); break;
        case 't':
            if (!is_dir)
                off += (size_t)snprintf(buf + off, sz - off, "%u", o->mtime);
            break;
        case 'T':
            if (!is_dir) {
                fmt_time_big(o->mtime, tmp, sizeof tmp);
                off += (size_t)snprintf(buf + off, sz - off, "%s", tmp);
            }
            break;
        case 's':
            if (!is_dir)
                off += (size_t)snprintf(buf + off, sz - off, "%u", vsize);
            break;
        case 'S':
            if (!is_dir) {
                fmt_grouped_big(tmp, sizeof tmp, vsize);
                off += (size_t)snprintf(buf + off, sz - off, "%s", tmp);
            }
            break;
        case 'c':
            if (!is_dir)
                off += (size_t)snprintf(buf + off, sz - off, "%u", vcksum);
            break;
        case '/': off += (size_t)snprintf(buf + off, sz - off, "%u/%u", o->uid, o->gid); break;
        case '?':
            usr_name_big(o->uid, un, sizeof un);
            grp_name_big(o->gid, gn, sizeof gn);
            off += (size_t)snprintf(buf + off, sz - off, "%s/%s", un, gn);
            break;
        default: break;
        }
        if (off + 2 >= sz) {
            buf[sz - 1] = '\0';
            return;
        }
    }
    buf[off] = '\0';
}

const char *BOMFSObjectSummary(BOMFSObject *obj, uint32_t f1, uint32_t f2) {
    static __thread char buf[4096];
    const char *path;
    if (obj == NULL)
        return "";
    (void)f1;
    (void)f2;
    path = BOMFSObjectPathName(obj);
    path = path ? path : ".";
    if (obj->link != NULL && obj->link_len > 0 && obj->type != BOM_TYPE_DIR) {
        (void)0;
    }
    summary_default(obj, path, buf, sizeof buf);
    return buf;
}

const char *BOMFSObjectSummaryWithFormat(BOMFSObject *obj,
                                         const char *params, int32_t arch) {
    static __thread char buf[4096];
    const char *path;
    if (obj == NULL || params == NULL)
        return "";
    path = BOMFSObjectPathName(obj);
    path = path ? path : ".";
    row_columns_big(obj, path, params, arch, buf, sizeof buf);
    return buf;
}

/* ---- memory dump ---- */

void BOMMemoryDump(const void *bytes, size_t length, uint32_t flags) {
    const uint8_t *p = (const uint8_t *)bytes;
    size_t i;
    (void)flags;
    if (p == NULL)
        return;
    for (i = 0; i < length; i += 16) {
        size_t j;
        fprintf(stdout, "%04zx: ", i);
        for (j = 0; j < 16 && i + j < length; j++)
            fprintf(stdout, "%02x ", p[i + j]);
        for (; j < 16; j++)
            fprintf(stdout, "   ");
        fprintf(stdout, " |");
        for (j = 0; j < 16 && i + j < length; j++)
            fprintf(stdout, "%c", p[i + j] >= 32 && p[i + j] < 127 ? p[i + j] : '.');
        fprintf(stdout, "|\n");
    }
}