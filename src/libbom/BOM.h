/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * BOM.h: the ABI surface of the Bom framework.
 *
 * Drop-in replacement header for Apple's private Bom.framework API.  Every
 * prototype, type and constant here was derived black-box: the symbol list
 * comes from the arm64e dyld shared-cache image of Apple's Bom (exported
 * names only), and each signature was recovered from the disassembly of the
 * three Apple clients /usr/bin/mkbom, /usr/bin/lsbom and /usr/bin/ditto --
 * the complete consumer set, whose union of imports is exactly the 56
 * symbols in local/reference/bom_cli_tool_imports_union.txt.  Integer
 * widths and return conventions follow the arm64 ABI of those binaries.
 *
 * Members of the opaque structs are never exposed; the framework's layout
 * is private.  All BOMSys arguments are unused by every reference client
 * (they pass NULL) and are retained purely for source compatibility. */
#ifndef LIBBOM_BOM_H
#define LIBBOM_BOM_H

#include <CoreFoundation/CoreFoundation.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- opaque object types ---- */

typedef struct BOMBom           BOMBom;
typedef struct BOMFSObject      BOMFSObject;
typedef struct BOMStorage       BOMStorage;
typedef struct BOMTree          BOMTree;
typedef struct BOMTreeIterator  BOMTreeIterator;
typedef struct BOMBomEnumerator BOMBomEnumerator;
typedef struct BOMCopier        BOMCopier;
typedef struct BOMSys           BOMSys;

/* ---- data flags ---- */

/* Values passed to BOMFSObjectSetFlags and to the New*WithOptions "options"
 * argument.  The reference clients read them as 16-bit quantities (ldrh) and
 * compare against B_ALLDATA to choose full-metadata vs path-only mode. */
extern uint16_t B_ALLDATA;   /* 0x000f */
extern uint16_t B_PATHONLY;  /* 0x0000 */

/* PathRecord path_type values as returned by BOMFSObjectType (our names;
 * values are part of the on-disk format, see FORMAT.md 4.6). */
#define BOM_TYPE_FILE    1   /* regular file */
#define BOM_TYPE_DIR     2   /* directory */
#define BOM_TYPE_LINK    3   /* symbolic link */
#define BOM_TYPE_DEVICE  4   /* block or character device */

/* Copier outcome codes passed to the copy-finished handler. */
#define BOM_COPY_OK       0
#define BOM_COPY_HARDLINK 1
#define BOM_COPY_SKIPPED  2

/* ---- copier callbacks ---- */

/* Handlers invoked by BOMCopierCopyWithOptions.  The first argument is a
 * copier-owned context (the reference clients ignore it); callback return
 * values are ignored by the framework (the reference clients' handlers
 * deliberately leave w0 undefined, so the framework must not act on them). */
typedef int (*BOMCopierFatalErrorHandler)(void *context, const char *message);
typedef int (*BOMCopierFatalFileErrorHandler)(void *context,
                                              const char *path, int err);
typedef int (*BOMCopierFileErrorHandler)(void *context,
                                         const char *path, int err);
typedef int (*BOMCopierCopyFileStartedHandler)(void *context,
                                               const char *path, int type);
typedef int (*BOMCopierCopyFileFinishedHandler)(void *context,
                                                const char *path, int type,
                                                int64_t bytes, int result);
typedef int (*BOMCopierCopyFileUpdateHandler)(void *context,
                                              const char *path,
                                              int64_t bytes);
typedef int (*BOMCopierPKZipPasswordRequester)(void *context,
                                               const char *path,
                                               char **outPassword);

/* ---- BOM storage (the "BOMStore" container) ---- */

/* 1 when `path` is a readable BOMStore file, 0 otherwise. */
int      BOMStorageIsStorageFileWithSys(const char *path, BOMSys *sys);

void     BOMStorageDump(BOMStorage *storage, uint32_t flags);

/* Returns the byte length of block `blockID` (1-based), 0 when the block is
 * null or out of range. */
size_t   BOMStorageSizeOfBlock(BOMStorage *storage, uint32_t blockID);

/* Copies block `blockID` into `buffer` (caller sized via SizeOfBlock).
 * Returns 0 on success, nonzero on failure. */
int      BOMStorageCopyFromBlock(BOMStorage *storage, uint32_t blockID,
                                 void *buffer);

/* ---- B-tree index over a BOMStore ---- */

BOMStorage *BOMTreeStorage(BOMTree *tree);

/* Opens the named tree variable ("Paths", "ProjectTagTable", ...) inside
 * `storage`; returns NULL when absent or malformed. */
BOMTree  *BOMTreeOpenWithName(BOMStorage *storage, const char *name,
                              uint32_t flags);

uint32_t BOMTreeCount(BOMTree *tree);

BOMTreeIterator *BOMTreeIteratorNew(BOMTree *tree, const void *startKey,
                                    const void *stopKey, uint32_t flags);
int      BOMTreeIteratorIsAtEnd(BOMTreeIterator *iter);
void    *BOMTreeIteratorKey(BOMTreeIterator *iter);     /* char * key */
void    *BOMTreeIteratorValue(BOMTreeIterator *iter);   /* raw value bytes */
void     BOMTreeIteratorNext(BOMTreeIterator *iter);
void     BOMTreeIteratorFree(BOMTreeIterator *iter);
void     BOMTreeFree(BOMTree *tree);

/* ---- filesystem objects (nodes of the path tree) ---- */

BOMFSObject  *BOMFSObjectNewWithSys(uint32_t type, BOMSys *sys);
void          BOMFSObjectFree(BOMFSObject *obj);

uint32_t      BOMFSObjectType(BOMFSObject *obj);
uint32_t      BOMFSObjectMode(BOMFSObject *obj);
const char   *BOMFSObjectPathName(BOMFSObject *obj);
/* Opaque per-object data (block id in byte-exact form for lsbom -v). */
void           *BOMFSObjectOpaqueData(BOMFSObject *obj);
size_t          BOMFSObjectOpaqueDataSize(BOMFSObject *obj);

int      BOMFSObjectIsBinaryObject(BOMFSObject *obj);
int      BOMFSObjectContainsArchitecture(BOMFSObject *obj,
                                         uint32_t cputype);

/* One line of lsbom stdout, e.g. "./base\t40755\t0/0\t0x0". */
BOMFSObject *BOMFSObjectParseSummary(const char *line);

void     BOMFSObjectSetFlags(BOMFSObject *obj, uint32_t flags);
/* copy != 0 makes the object own the string. */
void     BOMFSObjectSetPathName(BOMFSObject *obj, const char *path, int copy);
void     BOMFSObjectSetShortName(BOMFSObject *obj, const char *name, int copy);

/* Plain summary line (no -p formatting).  f1/f2 are two option-state bytes
 * consumed from client globals to gate path/data columns. */
const char *BOMFSObjectSummary(BOMFSObject *obj, uint32_t f1, uint32_t f2);
/* -p formatted summary.  arch is -1 for no arch filter, else a cputype. */
const char *BOMFSObjectSummaryWithFormat(BOMFSObject *obj,
                                         const char *params, int32_t arch);

void     BOMMemoryDump(const void *bytes, size_t length, uint32_t flags);

/* ---- bom objects ---- */

/* Creates a new, empty bom that will be written at `path` on commit/free. */
BOMBom  *BOMBomNewWithSys(const char *path, BOMSys *sys);

/* Opens an existing bom for reading; NULL when not a BOMStore. */
BOMBom  *BOMBomOpen(const char *path, uint32_t flags);
BOMBom  *BOMBomOpenWithSys(const char *path, uint32_t flags, BOMSys *sys);

/* Builds a bom of directory `dirPath` written as `bomPath`.  options is the
 * data flags (B_ALLDATA/B_PATHONLY); the final argument is a client flag
 * (1 in every reference call). */
BOMBom  *BOMBomNewFromDirectoryWithOptions(const char *bomPath,
                                           const char *dirPath,
                                           uint32_t options,
                                           uint32_t flags);

/* Copy-filtered re-write of `bom`, written as `outPath`. */
BOMBom  *BOMBomNewFromBomWithOptions(const char *outPath, BOMBom *bom,
                                     uint32_t options,
                                     const void *archFilter,
                                     const void *langFilter);

BOMFSObject *BOMBomGetRootFSObject(BOMBom *bom);
/* ".path/to/node" style; NULL when absent. */
BOMFSObject *BOMBomGetFSObjectAtPath(BOMBom *bom, const char *path);

/* Returns 0 on success.  Inserts copy the object's data (the reference
 * client frees the FSObject afterwards). */
int      BOMBomInsertFSObject(BOMBom *bom, BOMFSObject *obj,
                              uint32_t flags);
int      BOMBomRemoveFSObject(BOMBom *bom, BOMFSObject *obj);

/* The "Paths" tree. */
BOMTree  *BOMBomPathsTree(BOMBom *bom);

/* Enumerates children of `bom`'s root.  x1 (u64) and flags w2 are 0 and 1
 * in every reference call. */
BOMBomEnumerator *BOMBomEnumeratorNewWithOptions(BOMBom *bom, uint64_t options,
                                                 uint32_t flags);
BOMFSObject *BOMBomEnumeratorNext(BOMBomEnumerator *enumerator);
void     BOMBomEnumeratorFree(BOMBomEnumerator *enumerator);

void     BOMBomFree(BOMBom *bom);

/* ---- copier (directory (de)archival) ---- */

BOMCopier *BOMCopierNew(void);
void      BOMCopierFree(BOMCopier *copier);

void BOMCopierSetFatalErrorHandler(BOMCopier *copier,
                                   BOMCopierFatalErrorHandler handler);
void BOMCopierSetFatalFileErrorHandler(BOMCopier *copier,
                                       BOMCopierFatalFileErrorHandler handler);
void BOMCopierSetFileErrorHandler(BOMCopier *copier,
                                  BOMCopierFileErrorHandler handler);
void BOMCopierSetPKZipPasswordRequester(BOMCopier *copier,
                                        BOMCopierPKZipPasswordRequester h);
void BOMCopierSetCopyFileStartedHandler(BOMCopier *copier,
                                        BOMCopierCopyFileStartedHandler h);
void BOMCopierSetCopyFileFinishedHandler(BOMCopier *copier,
                                         BOMCopierCopyFileFinishedHandler h);
void BOMCopierSetCopyFileUpdateHandler(BOMCopier *copier,
                                       BOMCopierCopyFileUpdateHandler h);

/* Copies `src` to `dest` with a CFDictionary of options; returns 0 on
 * success, nonzero on error. */
int      BOMCopierCopyWithOptions(BOMCopier *copier, const char *src,
                                  const char *dest, CFDictionaryRef options);

#ifdef __cplusplus
}
#endif

#endif /* LIBBOM_BOM_H */