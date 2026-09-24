/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * Clean-room PKZip reader/writer matching ditto -c -k / -x -k byte-exactly
 * (FORMAT.md 13): stored dirs/symlinks/empty files, deflate-6 data
 * descriptors, 0x5855 atime/mtime/uid/gid extras, DOS stamps with even-
 * second rounding, and mode<<16|0x4000 external attributes. */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#include "ditto.h"

/* --------------------------------------------------------------------- */
/* little-endian helpers                                                 */
/* --------------------------------------------------------------------- */

static uint16_t r16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t r32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static void w16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}
static void w32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

/* --------------------------------------------------------------------- */
/* DOS timestamp                                                          */
/* --------------------------------------------------------------------- */

static int dim(int mon, int year) {
    static const int dims[12] = { 31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31 };
    if (mon == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        return 29;
    return dims[mon - 1];
}

/* Seconds rounded up to the next even second, with full carry. */
static void dos_stamp(time_t t, uint16_t *date, uint16_t *time24) {
    struct tm tmv;
    int sec, min, hour, day, mon, yr;
    localtime_r(&t, &tmv);
    sec = tmv.tm_sec;
    min = tmv.tm_min;
    hour = tmv.tm_hour;
    day = tmv.tm_mday;
    mon = tmv.tm_mon + 1;
    yr = tmv.tm_year + 1900;
    if (sec & 1) {
        /* round up to the next even second (carries upward) */
        sec++;
        if (sec >= 60) {
            sec = 0;
            min++;
            if (min >= 60) {
                min = 0;
                hour++;
                if (hour >= 24) {
                    hour = 0;
                    day++;
                }
            }
        }
        if (day > dim(mon, yr)) {
            day = 1;
            mon++;
            if (mon > 12) {
                mon = 1;
                yr++;
            }
        }
    }
    *date = (uint16_t)(((yr - 1980) << 9) | (mon << 5) | day);
    *time24 = (uint16_t)((hour << 11) | (min << 5) | (sec / 2));
}

/* --------------------------------------------------------------------- */
/* writer                                                                */
/* --------------------------------------------------------------------- */

static int zb_put(struct zip_out *zo, const void *p, size_t n) {
    size_t need = zo->len + n;
    if (need > zo->cap) {
        size_t ncap = zo->cap ? zo->cap : 4096;
        while (ncap < need)
            ncap <<= 1;
        uint8_t *nb = realloc(zo->b, ncap);
        if (nb == NULL) {
            zo->ioerr = 1;
            return -1;
        }
        zo->b = nb;
        zo->cap = ncap;
    }
    memcpy(zo->b + zo->len, p, n);
    zo->len += n;
    return 0;
}

int zip_out_init(struct zip_out *zo, int level) {
    memset(zo, 0, sizeof *zo);
    zo->level = level;
    return 0;
}

/* central directory records built as we go */
struct cent_rec {
    char       *name;
    uint16_t    method, flags, dos_date, dos_time;
    uint16_t    ver_needed;
    int         is_symlink;
    uint32_t    crc, csz, usz;
    uint32_t    ext_attr;
    uint32_t    local_off;
    time_t      mtime, atime;
};

int zip_out_add(struct zip_out *zo, const char *name, uint32_t mode,
                uint32_t uid, uint32_t gid, time_t mtime, time_t atime,
                int is_dir, int is_symlink, const void *payload, size_t plen) {
    uint16_t dos_date, dos_time;
    uint8_t hdr[30];
    uint16_t ver_needed, method, flags;
    uint32_t crc = 0, csz = plen, usz = plen;
    size_t namelen = strlen(name);
    int has_extra = !is_symlink;
    size_t extrasz = has_extra ? 16 : 0; /* 4-byte TLV header + 12 */
    uint32_t local_off = (uint32_t)zo->len;
    int deflated = 0;

    dos_stamp(mtime, &dos_date, &dos_time);

    if (is_dir || is_symlink || plen == 0) {
        method = 0;
        flags = 0;
        ver_needed = 10;
    } else {
        method = 8;
        flags = 0x0008;
        ver_needed = 20;
        deflated = 1;
    }

    /* local file header */
    w32(hdr + 0, 0x04034b50u);
    w16(hdr + 4, ver_needed);
    w16(hdr + 6, flags);
    w16(hdr + 8, method);
    w16(hdr + 10, dos_time);
    w16(hdr + 12, dos_date);
    /* crc/csz/usz patched for stored entries after payload */
    w32(hdr + 14, 0);
    w32(hdr + 18, 0);
    w32(hdr + 22, 0);
    w16(hdr + 26, (uint16_t)namelen);
    w16(hdr + 28, (uint16_t)extrasz);
    if (zb_put(zo, hdr, 30) != 0 || zb_put(zo, name, namelen) != 0)
        return -1;
    if (has_extra) {
        uint8_t ex[16];
        w16(ex + 0, 0x5855);
        w16(ex + 2, 0x000c);
        w32(ex + 4, (uint32_t)atime);
        w32(ex + 8, (uint32_t)mtime);
        w16(ex + 12, (uint16_t)uid);
        w16(ex + 14, (uint16_t)gid);
        if (zb_put(zo, ex, 16) != 0)
            return -1;
    }

    if (deflated) {
        /* raw deflate at configured level */
        z_stream zs;
        uint8_t out[1 << 16];
        size_t tot = 0;
        const uint8_t *in = (const uint8_t *)payload;
        memset(&zs, 0, sizeof zs);
        if (deflateInit2(&zs, zo->level, Z_DEFLATED, -15, 8,
                         Z_DEFAULT_STRATEGY) != Z_OK)
            return -1;
        zs.next_in = (Bytef *)in;
        zs.avail_in = (uInt)plen;
        for (;;) {
            int rc;
            zs.next_out = out;
            zs.avail_out = sizeof out;
            rc = deflate(&zs, Z_FINISH);
            if (zb_put(zo, out, sizeof out - zs.avail_out) != 0) {
                deflateEnd(&zs);
                return -1;
            }
            tot += sizeof out - zs.avail_out;
            if (rc == Z_STREAM_END)
                break;
            if (rc != Z_OK) {
                deflateEnd(&zs);
                return -1;
            }
        }
        deflateEnd(&zs);
        csz = (uint32_t)tot;
        crc = (uint32_t)crc32(0, (const Bytef *)payload, (uInt)plen);
        {
            uint8_t desc[16];
            w32(desc + 0, 0x08074b50u);
            w32(desc + 4, crc);
            w32(desc + 8, csz);
            w32(desc + 12, usz);
            if (zb_put(zo, desc, 16) != 0)
                return -1;
        }
    } else {
        if (plen > 0 && zb_put(zo, payload, plen) != 0)
            return -1;
        crc = (uint32_t)crc32(0, (const Bytef *)payload, (uInt)plen);
        /* patch crc/csz/usz into the local header */
        if (crc || usz || csz) {
            uint8_t *h = zo->b + local_off + 14;
            w32(h, crc);
            w32(h + 4, csz);
            w32(h + 8, usz);
        } else {
            /* leave zeros */
        }
    }

    {
        size_t i = zo->nentries++;
        struct cent_rec *c;
        if (zo->nentries == 1)
            c = malloc(sizeof *c);
        else
            c = realloc((struct cent_rec *)zo->cd,
                        zo->nentries * sizeof *c);
        if (c == NULL)
            return -1;
        zo->cd = c;
        c = (struct cent_rec *)zo->cd + i;
        c->name = strdup(name);
        c->method = method;
        c->flags = flags;
        c->dos_date = dos_date;
        c->dos_time = dos_time;
        c->ver_needed = ver_needed;
        c->is_symlink = is_symlink;
        c->crc = crc;
        c->csz = csz;
        c->usz = usz;
        c->ext_attr = (mode << 16) | 0x4000u;
        c->local_off = local_off;
        c->mtime = mtime;
        c->atime = atime;
    }
    return 0;
}

const uint8_t *zip_out_finish(struct zip_out *zo, size_t *len) {
    uint32_t cd_off = (uint32_t)zo->len;
    uint32_t cd_size = 0;
    size_t i;

    for (i = 0; i < zo->nentries; i++) {
        struct cent_rec *c = (struct cent_rec *)zo->cd + i;
        uint8_t hdr[46];
        uint8_t ex[12];
        size_t namelen = strlen(c->name);
        w32(hdr + 0, 0x02014b50u);
        w16(hdr + 4, 0x0315);                       /* made by */
        w16(hdr + 6, c->ver_needed);
        w16(hdr + 8, c->flags);
        w16(hdr + 10, c->method);
        w16(hdr + 12, c->dos_time);
        w16(hdr + 14, c->dos_date);
        w32(hdr + 16, c->crc);
        w32(hdr + 20, c->csz);
        w32(hdr + 24, c->usz);
        w16(hdr + 28, (uint16_t)namelen);
        w16(hdr + 30, c->is_symlink ? 0 : 12);  /* extra len: TLV+8 */
        w16(hdr + 32, 0);                      /* comment */
        w16(hdr + 34, 0);                      /* disk start */
        w16(hdr + 36, 0);                      /* int attrs */
        w32(hdr + 38, c->ext_attr);
        w32(hdr + 42, c->local_off);
        if (zb_put(zo, hdr, 46) != 0 || zb_put(zo, c->name, namelen) != 0)
            return NULL;
        if (!c->is_symlink) {
            w16(ex + 0, 0x5855);
            w16(ex + 2, 0x0008);
            w32(ex + 4, (uint32_t)c->atime);
            w32(ex + 8, (uint32_t)c->mtime);
            if (zb_put(zo, ex, 12) != 0)
                return NULL;
        }
    }
    cd_size = (uint32_t)(zo->len - cd_off);
    {
        uint8_t eocd[22];
        w32(eocd + 0, 0x06054b50u);
        w16(eocd + 4, 0);
        w16(eocd + 6, 0);
        w16(eocd + 8, (uint16_t)zo->nentries);
        w16(eocd + 10, (uint16_t)zo->nentries);
        w32(eocd + 12, cd_size);
        w32(eocd + 16, cd_off);
        w16(eocd + 20, 0);
        if (zb_put(zo, eocd, 22) != 0)
            return NULL;
    }
    *len = zo->len;
    return zo->b;
}

void zip_out_free(struct zip_out *zo) {
    free(zo->b);
    zo->b = NULL;
    zo->len = zo->cap = 0;
}

/* --------------------------------------------------------------------- */
/* reader                                                                */
/* --------------------------------------------------------------------- */

int zip_in_open(struct zip_in *zi, const uint8_t *p, size_t n) {
    size_t search = n < 65557 ? n : 65557;
    size_t eocd = (size_t)-1;
    size_t u;
    memset(zi, 0, sizeof *zi);

    /* find EOCD signature working backwards from the end */
    for (u = 0; u + 22 <= search; u++) {
        size_t off = n - search + u;
        if (p[off] == 'P' && p[off + 1] == 'K' && p[off + 2] == 0x05 &&
            p[off + 3] == 0x06) {
            eocd = off;
            break;
        }
    }
    if (eocd == (size_t)-1)
        return -1;
    {
        uint16_t nent = r16(p + eocd + 10);
        uint32_t cdsize = r32(p + eocd + 12);
        uint32_t cdoff = r32(p + eocd + 16);
        size_t i, pos = cdoff;
        size_t end = cdoff + cdsize;
        if (end > n)
            return -1;
        if (nent == 0)
            return 0;
        zi->cen = calloc(nent, sizeof(struct zip_cent));
        if (zi->cen == NULL)
            return -1;
        zi->ncen = nent;
        zi->p = p;
        zi->n = n;
        for (i = 0; i < nent && pos + 46 <= end; i++) {
            struct zip_cent *ce = zi->cen + i;
            uint16_t nl, el, cl;
            uint32_t ext;
            size_t j, xoff, xend;
            (void)xoff; (void)xend;
            if (r32(p + pos) != 0x02014b50u)
                goto bad;
            ce->method = r16(p + pos + 10);
            ce->flags = r16(p + pos + 8);
            ce->crc = r32(p + pos + 16);
            ce->csz = r32(p + pos + 20);
            ce->usz = r32(p + pos + 24);
            ext = r32(p + pos + 38);
            ce->mode = ext >> 16;
            ce->local_off = r32(p + pos + 42);
            nl = r16(p + pos + 28);
            el = r16(p + pos + 30);
            cl = r16(p + pos + 32);
            if (pos + 46 + nl + el + cl > end)
                goto bad;
            ce->name = malloc((size_t)nl + 1);
            if (ce->name == NULL)
                goto bad;
            memcpy(ce->name, p + pos + 46, nl);
            ce->name[nl] = '\0';
            ce->is_dir = (nl > 0 && ce->name[nl - 1] == '/');
            ce->is_symlink = (ce->mode & S_IFMT) == S_IFLNK;
            {
                /* parse the extra field for atime/mtime */
                size_t xoff = pos + 46 + nl;
                size_t xend = xoff + el;
                int have = 0;
                for (j = xoff; j + 4 <= xend; ) {
                    uint16_t t = r16(p + j);
                    uint16_t l = r16(p + j + 2);
                    if (j + 4 + l > xend)
                        break;
                    if (t == 0x5855 && l >= 8) {
                        ce->atime = (time_t)r32(p + j + 4);
                        ce->mtime = (time_t)r32(p + j + 8);
                        have = 1;
                    }
                    j += 4 + l;
                }
                if (!have) {
                    /* fall back to DOS stamp */
                    uint16_t dd = r16(p + pos + 14);
                    uint16_t dt = r16(p + pos + 12);
                    struct tm tmv;
                    tmv.tm_sec = (dt & 0x1f) * 2;
                    tmv.tm_min = (dt >> 5) & 0x3f;
                    tmv.tm_hour = (dt >> 11) & 0x1f;
                    tmv.tm_mday = dd & 0x1f;
                    tmv.tm_mon = ((dd >> 5) & 0x0f) - 1;
                    tmv.tm_year = ((dd >> 9) & 0x7f) + 80;
                    tmv.tm_isdst = -1;
                    ce->mtime = ce->atime = mktime(&tmv);
                }
            }
            pos += 46 + nl + el + cl;
        }
        return 0;
    }
bad:
    return -1;
}

int zip_in_next(struct zip_in *zi, struct zip_cent *ce) {
    size_t i;
    const uint8_t *p = zi->p;
    size_t pos;

    if (zi->icen >= zi->ncen)
        return 0;
    i = zi->icen;
    *ce = zi->cen[i];
    pos = ce->local_off;
    if (pos + 30 > zi->n || r32(p + pos) != 0x04034b50u)
        return -1;
    {
        uint16_t flags = r16(p + pos + 6);
        uint16_t method = r16(p + pos + 8);
        uint32_t csz = ce->csz;
        uint32_t usz = ce->usz;
        uint16_t nl = r16(p + pos + 26);
        uint16_t el = r16(p + pos + 28);
        size_t data_off = pos + 30 + nl + el;
        size_t j, xoff, xend;

        if (data_off > zi->n)
            return -1;

        /* local extra: prefer 0x5855 (has uid/gid too but we keep central) */
        xoff = pos + 30 + nl;
        xend = xoff + el;
        for (j = xoff; j + 4 <= xend; ) {
            uint16_t t = r16(p + j);
            uint16_t l = r16(p + j + 2);
            if (j + 4 + l > xend)
                break;
            if (t == 0x5855 && l >= 8) {
                ce->atime = (time_t)r32(p + j + 4);
                ce->mtime = (time_t)r32(p + j + 8);
            }
            j += 4 + l;
        }

        free(zi->data);
        zi->data = NULL;
        zi->dlen = 0;

        if (method == 0) {
            if (data_off + csz > zi->n)
                return -1;
            if (csz > 0) {
                zi->data = malloc(csz ? csz : 1);
                if (zi->data == NULL)
                    return -1;
                memcpy(zi->data, p + data_off, csz);
            }
            zi->dlen = csz;
            pos = data_off + csz;
        } else if (method == 8) {
            z_stream zs;
            size_t outcap, outlen;
            int rc;
            if (data_off + csz > zi->n)
                return -1;
            outcap = usz + 1;
            zi->data = malloc(outcap ? outcap : 1);
            if (zi->data == NULL)
                return -1;
            memset(&zs, 0, sizeof zs);
            if (inflateInit2(&zs, -15) != Z_OK)
                return -1;
            zs.next_in = (Bytef *)(p + data_off);
            zs.avail_in = (uInt)csz;
            zs.next_out = zi->data;
            zs.avail_out = (uInt)outcap;
            rc = inflate(&zs, Z_FINISH);
            inflateEnd(&zs);
            if (rc != Z_STREAM_END)
                return -1;
            outlen = outcap - zs.avail_out;
            zi->dlen = outlen;
            pos = (flags & 0x0008) ? data_off + csz + 16 : data_off + csz;
        } else {
            return -1;
        }
        zi->icen++;
        (void)pos;
        return 1;
    }
}

void zip_in_close(struct zip_in *zi) {
    size_t i;
    for (i = 0; i < zi->ncen; i++)
        free(zi->cen[i].name);
    free(zi->cen);
    free(zi->data);
    zi->cen = NULL;
    zi->data = NULL;
}