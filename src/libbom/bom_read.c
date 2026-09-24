/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * Clean-room BOM reader (see bom_read.h). */
#include "bom_read.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint16_t r16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t r32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

#define BM_MAGIC "BOMStore"
#define BM_HEADER_SIZE 32

int bom_file_open(const char *path, bom_file *bf) {
    int fd, rc = -1;
    struct stat st;
    uint8_t *buf = NULL;
    size_t alloc = 4096, total = 0;

    memset(bf, 0, sizeof *bf);

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        bf->open_failed = 1;
        return -1;
    }
    if (fstat(fd, &st) != 0)
        goto out;
    if (S_ISDIR(st.st_mode)) {
        errno = EISDIR;
        goto out;
    }
    if (st.st_size > 0)
        alloc = (size_t)st.st_size;
    buf = (uint8_t *)malloc(alloc ? alloc : 1);
    if (buf == NULL)
        goto out;
    for (;;) {
        ssize_t r;
        if (total == alloc) {
            uint8_t *nb;
            if (alloc > (size_t)-1 / 2) {
                errno = EINVAL;
                goto out;
            }
            nb = (uint8_t *)realloc(buf, alloc * 2);
            if (nb == NULL)
                goto out;
            buf = nb;
            alloc *= 2;
        }
        r = read(fd, buf + total, alloc - total);
        if (r < 0)
            goto out;
        if (r == 0)
            break;
        total += (size_t)r;
    }
    if (total < BM_HEADER_SIZE ||
        memcmp(buf, BM_MAGIC, sizeof(BM_MAGIC) - 1) != 0) {
        errno = EACCES; /* lsbom reports "Operation not permitted" here */
        goto out;
    }
    {
        uint32_t nob, bi_off, bi_len, count;
        uint32_t i;
        nob = r32(buf + 12);
        bi_off = r32(buf + 16);
        bi_len = r32(buf + 20);
        if (nob == 0 || bi_off > total || bi_len < 4 ||
            bi_len > total - bi_off) {
            errno = EACCES;
            goto out;
        }
        count = r32(buf + bi_off);
        if (bi_len < 4 + (size_t)count * 8) {
            errno = EACCES;
            goto out;
        }
        bf->block_off = (uint32_t *)calloc((size_t)nob + 1, sizeof(uint32_t));
        bf->block_len = (uint32_t *)calloc((size_t)nob + 1, sizeof(uint32_t));
        if (bf->block_off == NULL || bf->block_len == NULL) {
            errno = ENOMEM;
            goto out;
        }
        bf->data = buf;
        bf->len = (uint32_t)total;
        bf->num_blocks = nob;
        for (i = 1; i <= nob; i++) {
            bf->block_off[i] = r32(buf + bi_off + 4 + 8 * i);
            bf->block_len[i] = r32(buf + bi_off + 8 + 8 * i);
            if (bf->block_off[i]) {
                if ((size_t)bf->block_off[i] > total) {
                    errno = EACCES;
                    goto err_free;
                }
                if ((size_t)bf->block_off[i] + bf->block_len[i] > total) {
                    errno = EACCES;
                    goto err_free;
                }
            }
        }
        buf = NULL;
        rc = 0;
        goto out;
    }

err_free:
    free(bf->block_off);
    free(bf->block_len);
    bf->block_off = NULL;
    bf->block_len = NULL;
    errno = EACCES;
out:
    if (fd >= 0)
        close(fd);
    free(buf);
    if (rc != 0)
        free(bf->data);
    return rc;
}

void bom_file_close(bom_file *bf) {
    free(bf->data);
    free(bf->block_off);
    free(bf->block_len);
    memset(bf, 0, sizeof *bf);
}

const uint8_t *bom_block(const bom_file *bf, uint32_t index, uint32_t *len_out) {
    if (index == 0 || index > bf->num_blocks || bf->block_off[index] == 0) {
        if (len_out)
            *len_out = 0;
        return NULL;
    }
    if (len_out)
        *len_out = bf->block_len[index];
    return bf->data + bf->block_off[index];
}

int bom_pathrec_decode(const uint8_t *b, uint32_t len, bom_pathrec *pr) {
    memset(pr, 0, sizeof *pr);
    if (b == NULL || len < 31)
        return -1;
    pr->path_type = b[0];
    pr->architecture = r16(b + 2);
    pr->mode = r16(b + 4);
    pr->uid = r32(b + 6);
    pr->gid = r32(b + 10);
    pr->mtime = r32(b + 14);
    pr->size = r32(b + 18);
    pr->checksum = r32(b + 23);
    pr->link_len = r32(b + 27);
    if (pr->link_len > 0 && pr->link_len <= len - 31) {
        pr->link = b + 31;
    } else {
        pr->link = NULL;
        pr->link_len = 0;
    }
    pr->valid = 1;
    return 0;
}