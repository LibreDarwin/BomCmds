#ifndef BOM_WRITER_H
#define BOM_WRITER_H

#include "fs_walk.h"

/* Clean-room BOM writer. Reproduces Apple mkbom's output at the block
 * level: fixed blocks 1-10, (PathRecord, File, PathInfoIndex) triplets,
 * hard-link group trailers, block index and vars index have identical
 * indices, lengths and contents, so /usr/bin/lsbom output is byte-identical.
 * The emission is driven by the walk's mode (see enum bm_mode in fs_walk.h):
 * BM_MODE_DIR emits full records with content checksums and Mach-O arch
 * tables; BM_MODE_PATHONLY (-s) 4-byte typed records; BM_MODE_FILELIST (-i)
 * records built from an lsbom(8) listing. Physical offsets are contiguous
 * (no free list), per the conformance bar in docs. */

/* Writes `out_path` as a BOM of the scanned walk.
 * Returns 0 on success, -1 on error (message in errbuf if non-NULL). */
int bm_write_bom(const bm_walk *w, const char *out_path,
                 char *errbuf, size_t errbufsz);

#endif /* BOM_WRITER_H */