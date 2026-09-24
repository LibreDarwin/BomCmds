/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * AppleDouble / hashed-ATTR blob construction and parsing used by ditto's
 * `._` sidecar files (FORMAT.md 11). */
#include <stdlib.h>
#include <string.h>

#include "ditto.h"

/* 50-byte AppleDouble header + the ATTR blob layout documented in
 * FORMAT.md 11. */
#define AD_HEADER_SIZE 50
#define AD_BLOB_BASE 70
#define AD_REC_BASE 11

static void wbe16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

static void wbe32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xff);
    p[2] = (uint8_t)((v >> 8) & 0xff);
    p[3] = (uint8_t)(v & 0xff);
}

static uint16_t rbe16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rbe32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Record size for an xattr whose name is `namelen` bytes (including the
 * trailing NUL Apple stores in AppleDouble name fields); records are rounded
 * up to 4 and followed immediately by the value area. */
static size_t rec_size(size_t namelen) {
    return (AD_REC_BASE + namelen + 3) & ~(size_t)3;
}

void *
ad_build(const struct ad_xattr *xattrs, size_t n, const void *rfork,
         size_t rfork_len, size_t *outlen) {
    uint8_t *ad, *p;
    size_t i, recs = 0, datalen = 0, cum = 0;
    size_t blob_len, value_start, total, rofs, vofs;

    if (n == 0 && rfork_len == 0)
        return NULL;

    for (i = 0; i < n; i++) {
        recs += rec_size(strlen(xattrs[i].name) + 1);
        datalen += xattrs[i].len;
    }
    blob_len = AD_BLOB_BASE + recs + datalen;
    value_start = AD_HEADER_SIZE + AD_BLOB_BASE + recs;
    total = AD_HEADER_SIZE + blob_len + rfork_len;

    ad = calloc(1, total ? total : 1);
    if (ad == NULL)
        return NULL;

    /* AppleDouble header */
    wbe32(ad + 0, AD_MAGIC);
    wbe32(ad + 4, AD_VERSION);
    memcpy(ad + 8, "Mac OS X        ", 16); /* "Mac OS X" + 8 spaces */
    wbe16(ad + 24, 2);
    /* entry 0: attributes */
    wbe32(ad + 26 + 0, AD_ENTRY_ATTR);
    wbe32(ad + 26 + 4, AD_HEADER_SIZE);
    wbe32(ad + 26 + 8, (uint32_t)blob_len);
    wbe32(ad + 26 + 12, AD_ENTRY_DATA);
    if (rfork_len > 0) {
        wbe32(ad + 26 + 16, (uint32_t)(AD_HEADER_SIZE + blob_len));
        wbe32(ad + 26 + 20, (uint32_t)rfork_len);
    } else {
        wbe32(ad + 26 + 16, (uint32_t)total);
        wbe32(ad + 26 + 20, 0);
    }

    /* ATTR blob */
    p = ad + AD_HEADER_SIZE;
    wbe32(p + 34, 0x41545452u);           /* 'ATTR' */
    wbe32(p + 38, 0);                     /* debug_tag */
    wbe32(p + 42, (uint32_t)total);       /* total_size */
    wbe32(p + 46, (uint32_t)value_start); /* data_start */
    wbe32(p + 50, (uint32_t)datalen);     /* data_length */
    wbe16(p + 66, 0);                     /* flags */
    wbe16(p + 68, (uint16_t)n);           /* num_attrs */
    rofs = AD_BLOB_BASE;
    vofs = AD_BLOB_BASE + recs;
    for (i = 0; i < n; i++) {
        size_t namelen = strlen(xattrs[i].name) + 1;
        size_t rlen = rec_size(namelen);
        uint32_t abs = (uint32_t)(value_start + cum);
        wbe32(p + rofs + 0, abs);
        wbe32(p + rofs + 4, (uint32_t)xattrs[i].len);
        wbe16(p + rofs + 8, 0);
        p[rofs + 10] = (uint8_t)namelen;
        memcpy(p + rofs + 11, xattrs[i].name, namelen - 1);
        p[rofs + 11 + namelen - 1] = '\0';
        memcpy(p + vofs, xattrs[i].value, xattrs[i].len);
        cum += xattrs[i].len;
        rofs += rlen;
        vofs += xattrs[i].len;
    }
    if (rfork_len > 0)
        memcpy(ad + AD_HEADER_SIZE + blob_len, rfork, rfork_len);

    *outlen = total;
    return ad;
}

int ad_parse(const void *ad, size_t len, struct ad_xattr **xattrs, size_t *nx,
             void **rfork, size_t *rfork_len) {
    const uint8_t *p = (const uint8_t *)ad;
    const uint8_t *blob;
    uint16_t nent;
    uint32_t aoff, alen;
    size_t num, i;
    struct ad_xattr *out = NULL;
    void *fork = NULL;
    size_t forklen = 0;

    *xattrs = NULL;
    *nx = 0;
    *rfork = NULL;
    *rfork_len = 0;
    if (len < 50 || rbe32(p + 0) != AD_MAGIC || rbe32(p + 4) != AD_VERSION)
        return -1;
    nent = rbe16(p + 24);
    if (nent < 2 || len < 26 + (size_t)nent * 12)
        return -1;
    if (rbe32(p + 26 + 0) != AD_ENTRY_ATTR)
        return -1;
    aoff = rbe32(p + 26 + 4);
    alen = rbe32(p + 26 + 8);
    if (aoff + alen > len)
        return -1;
    blob = p + aoff;
    if (alen < AD_BLOB_BASE || rbe32(blob + 34) != 0x41545452u)
        return -1;
    num = rbe16(blob + 68);
    if (num > 0) {
        out = calloc(num, sizeof *out);
        if (out == NULL)
            return -1;
        for (i = 0; i < num; i++) {
            size_t rofs = AD_BLOB_BASE;
            size_t j, rlen;
            uint8_t namelen;
            uint32_t oft, l;
            char *nm;
            for (j = 0; j < i; j++)
                rofs += rec_size(((const uint8_t *)blob + rofs)[10]);
            if (rofs + 11 > alen)
                goto bad;
            oft = rbe32(blob + rofs + 0);
            l = rbe32(blob + rofs + 4);
            namelen = blob[rofs + 10];
            rlen = rec_size(namelen);
            if (namelen < 1 || rofs + rlen > alen || oft + l > len)
                goto bad;
            nm = malloc((size_t)namelen);
            if (nm == NULL)
                goto bad;
            memcpy(nm, blob + rofs + 11, namelen - 1);
            nm[namelen - 1] = '\0';
            out[i].name = nm;
            if (l > 0) {
                void *val = malloc(l ? l : 1);
                if (val == NULL)
                    goto bad;
                memcpy(val, p + oft, l);
                out[i].value = val;
            } else {
                out[i].value = NULL;
            }
            out[i].len = l;
        }
    }
    if (rbe32(p + 26 + 12) == AD_ENTRY_DATA) {
        uint32_t o2 = rbe32(p + 26 + 16);
        uint32_t l2 = rbe32(p + 26 + 20);
        if (l2 > 0 && o2 + l2 <= len) {
            fork = malloc(l2 ? l2 : 1);
            if (fork == NULL)
                goto bad;
            memcpy(fork, p + o2, l2);
            forklen = l2;
        }
    }
    *xattrs = out;
    *nx = num;
    *rfork = fork;
    *rfork_len = forklen;
    return 0;
bad:
    if (out != NULL) {
        for (i = 0; i < num; i++) {
            free((void *)out[i].name);
            free((void *)out[i].value);
        }
        free(out);
    }
    free(fork);
    return -1;
}