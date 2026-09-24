/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * Clean-room CPIO "odc" (070707) reader/writer for ditto -c/-x.
 *
 * Layout contract (FORMAT.md 10): each entry is header(76) + name + NUL +
 * data, no per-entry padding; octal ASCII fields; TRAILER!!! ends the
 * stream; the fully padded stream is what -z/-j compress. */
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <zlib.h>
#include <bzlib.h>

#include "ditto.h"

/* --------------------------------------------------------------------- */
/* low-level stream write                                                */
/* --------------------------------------------------------------------- */

/* Hand-rolled gzip framing: raw deflate plus an explicitly written 10-byte
 * header (mtime=0, XFL=0, OS=3) and 8-byte trailer (CRC-32, ISIZE).  This
 * gives byte-identical output to the reference without relying on the
 * zlib gzdopen header defaults. */
struct gz_state {
    z_stream strm;
    uLong crc;
    uint32_t isize;
    int header_written;
};

static int gz_write_raw(FILE *f, struct gz_state *g, const void *data,
                        size_t len) {
    g->strm.next_in = (Bytef *)data;
    g->strm.avail_in = (uInt)len;
    while (g->strm.avail_in > 0) {
        uint8_t out[65536];
        int ret;
        g->strm.next_out = out;
        g->strm.avail_out = sizeof out;
        ret = deflate(&g->strm, Z_NO_FLUSH);
        if (ret != Z_OK)
            return -1;
        if (fwrite(out, 1, sizeof out - g->strm.avail_out, f) !=
            sizeof out - g->strm.avail_out)
            return -1;
    }
    return 0;
}

static int gz_write_header(FILE *f, struct gz_state *g) {
    uint8_t hdr[10] = { 0x1f, 0x8b, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0x03 };
    g->header_written = 1;
    return fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr ? 0 : -1;
}

static int co_out(struct cpio_out *co, const void *p, uint64_t len) {
    if (len == 0)
        return 0;
    co->written += len;
    if (co->compress == CPIO_PLAIN) {
        if (fwrite(p, 1, (size_t)len, co->f) != len)
            return -1;
        return 0;
    } else if (co->compress == CPIO_GZIP) {
        struct gz_state *g = (struct gz_state *)co->z;
        if (!g->header_written && gz_write_header(co->f, g) != 0)
            return -1;
        g->crc = crc32(g->crc, (const Bytef *)p, (uInt)len);
        g->isize += (uint32_t)len;
        return gz_write_raw(co->f, g, p, (size_t)len);
    } else { /* CPIO_BZIP2 */
        BZFILE *bz = (BZFILE *)co->z;
        int bzerr;
        BZ2_bzWrite(&bzerr, bz, (void *)p, (int)len);
        return (bzerr == BZ_OK || bzerr == BZ_STREAM_END) ? 0 : -1;
    }
}

void cpio_out_init(struct cpio_out *co, FILE *f, cpio_compress c) {
    memset(co, 0, sizeof *co);
    co->f = f;
    co->compress = (int)c;
    if (c == CPIO_GZIP) {
        struct gz_state *g = calloc(1, sizeof *g);
        if (g == NULL) {
            co->compress = CPIO_PLAIN;
            return;
        }
        /* raw deflate (negative window bits): we write the gzip header and
         * trailer ourselves for byte-identical output */
        if (deflateInit2(&g->strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                         -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            free(g);
            co->compress = CPIO_PLAIN;
            return;
        }
        g->crc = crc32(0L, Z_NULL, 0);
        co->z = g;
    } else if (c == CPIO_BZIP2) {
        int bzerr;
        co->z = BZ2_bzWriteOpen(&bzerr, f, 9, 0, 0);
    }
}

int cpio_write_header(struct cpio_out *co, const char *name, uint32_t mode,
                      uint32_t uid, uint32_t gid, uint32_t nlink,
                      uint32_t mtime, uint64_t size, uint32_t ino) {
    uint32_t namesize;
    char hdr[77];
    if (ino == (uint32_t)-1)
        ino = (uint32_t)co->ino++;
    namesize = (uint32_t)strlen(name) + 1;
    snprintf(hdr, sizeof hdr,
             "070707%06o%06o%06o%06o%06o%06o%06o%011o%06o%011o",
             0, ino, mode, uid, gid, nlink, 0, mtime, namesize, (unsigned)size);
    if (co_out(co, hdr, 76) != 0)
        return -1;
    if (co_out(co, name, namesize) != 0)
        return -1;
    return 0;
}

int cpio_write_data(struct cpio_out *co, const void *data, uint64_t len) {
    return co_out(co, data, len);
}

int cpio_write_trailer(struct cpio_out *co) {
    static const char trailer[] = "TRAILER!!!";
    /* ino = the next synthetic value (current counter) */
    return cpio_write_header(co, trailer, 0, 0, 0, 1, 0, 0,
                             (uint32_t)co->ino);
}

int cpio_finish_pad(struct cpio_out *co) {
    uint64_t rem = (uint64_t)(512 - (co->written % 512)) % 512;
    uint8_t z[512];
    memset(z, 0, sizeof z);
    while (rem > 0) {
        uint64_t n = rem < sizeof z ? rem : sizeof z;
        if (co_out(co, z, n) != 0)
            return -1;
        rem -= n;
    }
    return 0;
}

int cpio_flush(struct cpio_out *co) {
    /* no sync flush: forcing one would emit a stored block and change the
     * compressed bytes versus the reference tool's plain gzclose. */
    (void)co;
    return 0;
}

int cpio_out_close(struct cpio_out *co, int *io_err) {
    int rc = 0;
    if (co->compress == CPIO_GZIP) {
        struct gz_state *g = (struct gz_state *)co->z;
        uint8_t out[65536], tail[8];
        int ret;
        if (!g->header_written && gz_write_header(co->f, g) != 0)
            rc = -1;
        /* drain the deflate stream (Z_FINISH), then CRC-32 + ISIZE LE */
        for (;;) {
            g->strm.next_in = Z_NULL;
            g->strm.avail_in = 0;
            g->strm.next_out = out;
            g->strm.avail_out = sizeof out;
            ret = deflate(&g->strm, Z_FINISH);
            if (fwrite(out, 1, sizeof out - g->strm.avail_out, co->f) !=
                sizeof out - g->strm.avail_out) {
                rc = -1;
                break;
            }
            if (ret == Z_STREAM_END)
                break;
            if (ret != Z_OK) {
                rc = -1;
                break;
            }
        }
        tail[0] = (uint8_t)g->crc;
        tail[1] = (uint8_t)(g->crc >> 8);
        tail[2] = (uint8_t)(g->crc >> 16);
        tail[3] = (uint8_t)(g->crc >> 24);
        tail[4] = (uint8_t)g->isize;
        tail[5] = (uint8_t)(g->isize >> 8);
        tail[6] = (uint8_t)(g->isize >> 16);
        tail[7] = (uint8_t)(g->isize >> 24);
        if (fwrite(tail, 1, sizeof tail, co->f) != sizeof tail)
            rc = -1;
        deflateEnd(&g->strm);
        free(g);
        co->z = NULL;
    } else if (co->compress == CPIO_BZIP2) {
        int bzerr;
        BZ2_bzWriteClose(&bzerr, (BZFILE *)co->z, 0, NULL, NULL);
        if (bzerr != BZ_OK && bzerr != BZ_STREAM_END)
            rc = -1;
        co->z = NULL;
    }
    if (fflush(co->f) != 0)
        rc = -1;
    *io_err = rc;
    return rc;
}

/* --------------------------------------------------------------------- */
/* reader                                                                */
/* --------------------------------------------------------------------- */

/* parse an exactly-width octal field; returns ULONG_MAX on bad digit */
static unsigned long oct_field(const char *p, int len) {
    char tmp[16];
    char *endp;
    unsigned long v;
    if (len >= (int)sizeof tmp)
        return ULONG_MAX;
    memcpy(tmp, p, len);
    tmp[len] = '\0';
    v = strtoul(tmp, &endp, 8);
    if (endp != tmp + len)
        return ULONG_MAX;
    return v;
}

int cpio_next(struct cpio_in *in, struct cpio_entry *ce) {
    const uint8_t *p = in->p;
    size_t remaining = (size_t)(in->end - p);

    for (;;) {
        const char *f;
        unsigned long namesize, filesize, v;
        size_t consumes;

        remaining = (size_t)(in->end - p);
        if (remaining < 76)
            return 0; /* clean end of (padded) stream */
        if (memcmp(p, "070707", 6) != 0)
            return -1;
        f = (const char *)p + 6;
        v = oct_field(f, 6);
        ce->dev   = (uint32_t)v;
        v = oct_field(f + 6, 6);
        ce->ino   = (uint32_t)v;
        v = oct_field(f + 12, 6);
        ce->mode  = (uint32_t)v;
        v = oct_field(f + 18, 6);
        ce->uid   = (uint32_t)v;
        v = oct_field(f + 24, 6);
        ce->gid   = (uint32_t)v;
        v = oct_field(f + 30, 6);
        ce->nlink = (uint32_t)v;
        v = oct_field(f + 36, 6); /* rdev */
        (void)v;
        v = oct_field(f + 42, 11);
        ce->mtime = (uint32_t)v;
        namesize = oct_field(f + 53, 6);
        filesize = oct_field(f + 59, 11);
        if (namesize == 0 || namesize == ULONG_MAX ||
            namesize > sizeof ce->name)
            return -1;
        if (filesize == ULONG_MAX)
            return -1;
        consumes = 76 + namesize + filesize;
        if (consumes > remaining)
            return -2; /* truncated data (EOF-style) */
        memcpy(ce->name, p + 76, (size_t)namesize);
        ce->name[namesize - 1] = '\0';
        ce->size = filesize;
        in->p = p + consumes;
        p = in->p;
        if (strcmp(ce->name, "TRAILER!!!") == 0)
            return 0;
        return 1;
    }
}