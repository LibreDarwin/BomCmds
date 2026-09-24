/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * Clean-room BOMCopier: the copy engine surface of Bom.framework.
 *
 * Functional plain copy: recursively copies a source directory tree to a
 * destination, creating directories and hard-copying file contents, and
 * streams the copy callbacks (started/update/finished/errors) to any
 * installed handlers.  Options, symlink semantics, metadata preservation
 * and pkzip password prompting from the reference framework are not yet
 * implemented; the copy does not consult/emit a bom.  Reference clients
 * pass BOMSys = NULL and ignore the copier-owned vendor context. */
#include "bom_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

BOMCopier *BOMCopierNew(void) {
    return (BOMCopier *)calloc(1, sizeof(BOMCopier));
}

void BOMCopierFree(BOMCopier *copier) {
    free(copier);
}

void BOMCopierSetFatalErrorHandler(BOMCopier *c,
                                   BOMCopierFatalErrorHandler h) {
    if (c) c->fatal = h;
}
void BOMCopierSetFatalFileErrorHandler(BOMCopier *c,
                                       BOMCopierFatalFileErrorHandler h) {
    if (c) c->fatal_file = h;
}
void BOMCopierSetFileErrorHandler(BOMCopier *c, BOMCopierFileErrorHandler h) {
    if (c) c->file_err = h;
}
void BOMCopierSetPKZipPasswordRequester(BOMCopier *c,
                                        BOMCopierPKZipPasswordRequester h) {
    if (c) c->passwd = h;
}
void BOMCopierSetCopyFileStartedHandler(BOMCopier *c,
                                        BOMCopierCopyFileStartedHandler h) {
    if (c) c->started = h;
}
void BOMCopierSetCopyFileFinishedHandler(BOMCopier *c,
                                         BOMCopierCopyFileFinishedHandler h) {
    if (c) c->finished = h;
}
void BOMCopierSetCopyFileUpdateHandler(BOMCopier *c,
                                       BOMCopierCopyFileUpdateHandler h) {
    if (c) c->update = h;
}

/* ---- copy engine ---- */

static char *join_path(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    int slash = la > 0 && a[la - 1] != '/';
    char *out = (char *)malloc(la + lb + 2);
    if (out == NULL)
        return NULL;
    memcpy(out, a, la);
    if (slash)
        out[la++] = '/';
    memcpy(out + la, b, lb + 1);
    return out;
}

static int copy_bytes(BOMCopier *c, const struct stat *st,
                      const char *path, const char *dst) {
    char buf[65536];
    FILE *in, *out;
    size_t n;
    int64_t total = 0, tick = 0;
    int rc = 0;
    in = fopen(path, "rb");
    if (in == NULL) {
        if (c->file_err)
            c->file_err(c, path, errno);
        return -1;
    }
    out = fopen(dst, "wb");
    if (out == NULL) {
        if (c->fatal_file)
            c->fatal_file(c, dst, errno);
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            if (c->fatal_file)
                c->fatal_file(c, dst, errno);
            rc = -1;
            break;
        }
        total += (int64_t)n;
        tick += (int64_t)n;
        if (c->update && tick >= 0x10000) {
            c->update(c, path, total);
            tick = 0;
        }
    }
    if (rc == 0 && st && fchmod(fileno(out), st->st_mode) != 0) {
        if (c->file_err)
            c->file_err(c, dst, errno);
        rc = -1;
    }
    if (rc == 0 && fclose(out) != 0) {
        if (c->fatal_file)
            c->fatal_file(c, dst, errno);
        rc = -1;
    } else if (rc != 0) {
        fclose(out);
    }
    fclose(in);
    if (c->update)
        c->update(c, path, total);
    return rc;
}

static int copy_one(BOMCopier *c, const struct stat *st,
                    const char *path, const char *dst) {
    int kind;
    if (S_ISDIR(st->st_mode))
        kind = 1; /* directory */
    else if (S_ISLNK(st->st_mode))
        kind = 2; /* symlink */
    else
        kind = 3; /* regular/other */
    if (c->started)
        c->started(c, path, kind);
    if (S_ISDIR(st->st_mode)) {
        if (mkdir(dst, st->st_mode) != 0 && errno != EEXIST) {
            if (c->fatal_file)
                c->fatal_file(c, dst, errno);
            if (c->finished)
                c->finished(c, path, kind, 0, BOM_COPY_SKIPPED);
            return -1;
        }
        if (c->finished)
            c->finished(c, path, kind, 0, BOM_COPY_OK);
        return 0;
    }
    if (S_ISLNK(st->st_mode)) {
        char *target = (char *)malloc((size_t)st->st_size + 1);
        int rc;
        if (target == NULL) {
            if (c->fatal)
                c->fatal(c, "out of memory");
            return -1;
        }
        rc = (int)readlink(path, target, (size_t)st->st_size);
        if (rc < 0) {
            if (c->file_err)
                c->file_err(c, path, errno);
            free(target);
            return -1;
        }
        target[rc] = '\0';
        if (symlink(target, dst) != 0 && errno != EEXIST) {
            if (c->fatal_file)
                c->fatal_file(c, dst, errno);
            free(target);
            if (c->finished)
                c->finished(c, path, kind, 0, BOM_COPY_SKIPPED);
            return -1;
        }
        free(target);
        if (c->finished)
            c->finished(c, path, kind, (int64_t)st->st_size, BOM_COPY_OK);
        return 0;
    }
    {
        int rc = copy_bytes(c, st, path, dst);
        if (c->finished)
            c->finished(c, path, kind, (int64_t)st->st_size,
                        rc == 0 ? BOM_COPY_OK : BOM_COPY_SKIPPED);
        return rc;
    }
}

static int copy_tree(BOMCopier *c, const char *src, const char *dst) {
    DIR *d;
    struct dirent *e;
    struct stat st;
    d = opendir(src);
    if (d == NULL) {
        if (c->fatal_file)
            c->fatal_file(c, src, errno);
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        char *sp, *dp;
        int rc;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        sp = join_path(src, e->d_name);
        dp = join_path(dst, e->d_name);
        if (sp == NULL || dp == NULL) {
            free(sp);
            free(dp);
            closedir(d);
            if (c->fatal)
                c->fatal(c, "out of memory");
            return -1;
        }
        rc = lstat(sp, &st);
        if (rc != 0) {
            if (c->file_err)
                c->file_err(c, sp, errno);
            free(sp);
            free(dp);
            continue;
        }
        rc = copy_one(c, &st, sp, dp);
        if (rc == 0 && S_ISDIR(st.st_mode))
            rc = copy_tree(c, sp, dp);
        free(sp);
        free(dp);
        if (rc != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

int BOMCopierCopyWithOptions(BOMCopier *copier, const char *src,
                             const char *dest, CFDictionaryRef options) {
    struct stat st;
    (void)options;
    if (copier == NULL || src == NULL || dest == NULL)
        return -1;
    if (lstat(src, &st) != 0)
        return -1;
    if (S_ISDIR(st.st_mode))
        return copy_tree(copier, src, dest);
    return copy_one(copier, &st, src, dest);
}