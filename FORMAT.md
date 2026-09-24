# BOM File Format — empirically established

This document describes the Apple BOM (Bill of Materials) container format as
observed in real `.bom` files produced by the reference tool
`/usr/bin/mkbom` and read by `/usr/bin/lsbom` on macOS (the conformance
oracle). Everything below was either verified byte-for-byte against a captured
fixture (`fix.bom`, 35245 bytes) or cross-checked with the MIT-licensed
`apple-bom` Rust crate (G. Szorc, 2022), which documents the same layout.

All integers are big-endian. Layout assume a single seekable byte stream.

## 1. File header — `BOMStore`

| offset | size | field |
|--------|------|-------|
| 0x00   | 8    | magic, always `BOMStore` |
| 0x08   | 4    | format version (observed `1`) |
| 0x0c   | 4    | number_of_blocks — number of *populated* (non-null) block entries, i.e. block indices 1..N; block 0 is always the null block |
| 0x10   | 4    | blocks_index_offset — file offset of the block index (below) |
| 0x14   | 4    | blocks_index_length |
| 0x18   | 4    | vars_index_offset — file offset of the variables index (below) |
| 0x1c   | 4    | vars_index_length |

Header is 32 bytes; the remainder of the first 0x200 bytes is zero padding in
files produced by Apple's mkbom. Data blocks start at file offset 0x200.

`blocks_index_offset + blocks_index_length` equals the file size exactly.

## 2. Block index (`blocks_index_offset`)

```
u32  count                    # pool size; may be much larger than number_of_blocks
count * BlockEntry            # BlockEntry = { u32 file_offset; u32 length }
u32  free_list_count          # entries that were freed back into the pool
free_list_count * BlockEntry
padding                       # zeros, up to blocks_index_length
```

- `count` is a file-global allocation pool. In the fixture it is 2730 even
  though only 22 blocks are live. `number_of_blocks` in the header equals the
  number of live (non-null, `file_offset != 0`) entries *below* it.
- Entry with `file_offset == 0` (and index 0) is the always-null block.
- `file_offset` is absolute from the start of the file; `length` is the block
  byte length (an allocation size, not necessarily the semantically-used size —
  e.g. a `Paths` block is allocated at its tree's `block_size` of 0x1000
  although only 12 + 8*n bytes are used).

Free-list entries observed: `(0x20d, 3) (0x210, 12) (0x231, 5) (0x126e, 3)
(0x229f, 3)` — small holes left by the writer between live blocks.

## 3. Variables index (`vars_index_offset`)

```
u32  count
count * Var
```

Each `Var` is packed with **no** trailing NUL and **no** pad byte:

```
u32  block_index     # which block holds this variable's data
u8   name_length
name[name_length]    # raw bytes, not NUL-terminated
```

Observed variables, in order, for files created by Apple's mkbom:

| name     | block | kind of referenced block |
|----------|-------|--------------------------|
| `BomInfo`| 1     | `BomInfo` record |
| `Paths`  | 2     | `Tree` root for the path tree |
| `HLIndex`| 4     | `Tree` for hard-link index (empty tree possible) |
| `VIndex` | 6     | `VIndex` record |
| `Size64` | 9     | `Tree` for 64-bit (file-size) index |

Note: the `apple-bom` crate writes vars with `name_length = strlen + 1` and a
NUL terminator of its own; that is a writer-side quirk of that crate. Apple's
mkbom writes `name_length = strlen` and no terminator (verified: 5 vars occupy
exactly 60 bytes = 4 + 12 + 10 + 12 + 11 + 11).

## 4. Block kinds

The block index does not record block types; the type is inferred from how a
block is referenced (via the variable's `block_index` or a record's index
field). Known kinds, all little-format-documented types corroborated on the
fixture:

### 4.1 `BomInfo` (block 1)

```
u32  version                 # observed 1
u32  number_of_paths         # observed 5 for a 4-path tree (!) — see round-trip notes
u32  number_of_info_entries  # observed 1
number_of_info_entries * { u32 a; u32 b; u32 c; u32 d }   # arch-specific, unknown meaning
```

The `a b c d` entry is a 16-byte opaque tuple (observed `0 0 0x1e 0`).
`number_of_paths` discrepancy vs the path tree's count must be calibrated
against system mkbom during implementation.

### 4.2 `Tree`

```
u8[4]  magic                # "tree"
u32    version              # observed 1
u32    block_paths_index    # block holding this tree's Paths record (or 0)
u32    block_size           # allocation unit for its Paths blocks (0x1000; VIndex tree uses 0x80)
u32    path_count           # number of paths in the tree
u8     a                    # observed 0
```

Fixed 21 bytes. Root trees for `Paths` (blk 2), `HLIndex` (blk 4), VIndex
inner tree, `Size64` (blk 9). For `Paths`, `block_paths_index` → a `Paths`
block and `path_count` equals the number of paths (4 in the fixture).

### 4.3 `Paths`

```
u16  is_path_info   # 1 => entries point to PathInfoIndex blocks; 0 => pointers to other Paths blocks
u16  count          # number of BomPathsEntry records
u32  next_paths_block_index    # linked-list forward (0 = end)
u32  previous_paths_block_index # linked-list backward (0 = none)
count * PathsEntry
```

```
PathsEntry = { u32 block_index; u32 file_index }
```

When `is_path_info == 1`, `block_index` → a `PathInfoIndex` and `file_index` →
the `File` block carrying the path's name; when `0`, `block_index` → another
`Paths` block. `next/previous` chain multiple Paths blocks into a list (to
support trees with more paths than one block fits).

### 4.4 `PathInfoIndex`

```
u32 path_id            # unique internal numeric id for the path (1-based; 0 = no parent/root)
u32 path_record_index  # block holding this path's PathRecord
```

Fixed 8 bytes.

### 4.5 `PathRecord`

```
u8   path_type      # 1 = regular file, 2 = directory (see BomPathType)
u8   a              # observed 1
u16  architecture   # observed 0xf
u16  mode           # st_mode incl. S_IFMT bits, e.g. 0x41ed = 040755 dir; 0x81ed = 100755 file
u32  user           # uid
u32  group          # gid
u32  mtime          # seconds since epoch
u32  size           # bytes (directory: block-rounded size)
u8   b              # observed 1
u32  checksum_or_type   # CRC32 of file contents; dirs: 0
u32  link_name_length   # length of link target name including NUL, 0 if not a symlink
link_name[link_name_length]   # only when link_name_length > 0
```

Base record is 31 bytes; the fixture's file PathRecords are 35 bytes (extra 4
zero bytes appended when `path_type` is a regular file — calibrate during
round-trip; directories are exactly 31). Symlink records carry the target
after the base 31 bytes.

### 4.6 `File`

```
u32  parent_path_id   # path_id of parent directory (0 = root dir is its own parent)
u8[] name             # NUL-terminated leaf name
```

### 4.7 `VIndex`

```
u32  a                 # observed 1
u32  tree_block_index  # block holding a Tree serving as the actual VIndex index
u32  b                 # observed 0
u8   c                 # observed 0
```

Fixed 13 bytes.

## 5. Canonical layout in Apple-generated files

Blocks are packed contiguously from 0x200 in *allocation* order, but the block
*order in the file* is unrelated to block index order. Apple buffers index
metadata and writes it last. Observed ordering pattern:

1. low-offset header meta: BomInfo record, Tree roots, Paths blocks, VIndex,
   in allocation order (blocks 1, 4, 6, … at low offsets)
2. path triplets mid-file: each path = PathRecord + File + PathInfoIndex
   (repeats per path)
3. Paths/Tree blocks for the deferred indices at higher offsets
4. block index + vars index at the very end of the file (block table follows
   the last data block; `blocks_index_offset + length == filesize`)

The block index (blocks section 2) and the vars index (section 3) are the last
two structures.

## 6. Open items for round-trip calibration

- Exact meaning of `BomInfo.number_of_paths` (fixture: 5 for 4 paths).
- The trailing 4 zero bytes on regular-file `PathRecord`s (fixture: files 35
  bytes, dirs 31) — confirm system mkbom writes them identically.
- `architecture = 0xf` constant — confirm per-platform value.
- Whether the pool size 2730 and the free list must be reproduced verbatim or
  can be a smaller pool (lsbom only needs populated entries).