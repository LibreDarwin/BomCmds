#ifndef BOM_WRITER_H
#define BOM_WRITER_H

#include "fs_walk.h"

/* Clean-room dir-mode BOM writer. Reproduces Apple mkbom's output for a
 * directory tree at the block level: fixed blocks 1-10, (PathRecord, File,
 * PathInfoIndex) triplets, hard-link group trailers, block index and vars
 * index have identical indices, lengths and contents, so /usr/bin/lsbom
 * output is byte-identical. Physical offsets are contiguous (no free list),
 * per the conformance bar in docs. */

/* Writes `out_path` as a BOM of the scanned walk.
 * Returns 0 on success, -1 on error (message in errbuf if non-NULL). */
int bm_write_bom(const bm_walk *w, const char *out_path,
                 char *errbuf, size_t errbufsz);

#endif /* BOM_WRITER_H */