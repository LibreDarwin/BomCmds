# BOM File Format — empirically established

This document describes the Apple BOM (Bill of Materials) container format as
observed in real `.bom` files produced by the reference tool `/usr/bin/mkbom`
and read by `/usr/bin/lsbom` on macOS (the conformance oracle). Everything
below was either verified byte-for-byte against captured fixtures (see
`T/bomfmt` in the RE workspace: `fix`, `p1`–`p6`, `hme`, `hlx`, `y1`–`y5`,
`syme`, `empty`, `one`, `two`, `q4`, `s1`) or cross-checked with the
MIT-licensed `apple-bom` Rust crate (G. Szorc, 2022).

All integers are big-endian. Layout assumes a single seekable byte stream.

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

- `count` is a file-global allocation pool. In the fixture it is 2730 (0xAAA)
  even though only ~20-65 blocks are live. `number_of_blocks` in the header
  equals the number of live (non-null, `file_offset != 0`) entries *below* it
  (block indices are allocated contiguously from 1, so it equals the highest
  live index).
- Entry with `file_offset == 0` (and index 0) is the always-null block.
- `file_offset` is absolute from the start of the file; `length` is the block
  byte length (an allocation size, not necessarily the semantically-used size —
  e.g. a `Paths` block for the main tree is allocated at its tree's
  `block_size` of 0x1000 although only 12 + 8*n bytes are used).

Free-list entries observed: `(0x20d, 3) (0x210, 12) (0x231, 5) (0x126e, 3)
(0x229f, 3)` — small holes left by the writer between live blocks.

The accepted conformance bar (see `local/BomCmds.md`) is: **same block indices,
contents and lengths, plus byte-identical `lsbom` output.** Physical offsets
and the free list may differ from Apple's files; `lsbom` works purely off the
index (offset,length) pairs.

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

Observed variables, in this exact order, for files created by Apple's mkbom:

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
field).

### 4.1 Fixed block map

Every Apple-generated file (dir mode) has the following fixed prefix. Blocks
1-10 are always present; data blocks start at index 11.

| block | kind | content |
|-------|------|---------|
| 1     | `BomInfo` | 28 bytes (12 bytes if `ninfo == 0`) |
| 2     | `Tree` | Paths tree: ver 1, bpi 3, bsize 0x1000, pcount = #tree paths |
| 3     | `Paths` | main path table, allocated 0x1000 |
| 4     | `Tree` | HLIndex tree: bpi 5, bsize 0x1000, pcount = #hard-link groups |
| 5     | `Paths` | HLIndex path table (4K) |
| 6     | `VIndex` | 13 bytes |
| 7     | `Tree` | VIndex tree: bpi 8, bsize 0x80, pcount 0 |
| 8     | `Paths` | VIndex path table (128 bytes: header only, rest zeros) |
| 9     | `Tree` | Size64 tree: bpi 10, bsize 0x1000, pcount 0 |
| 10    | `Paths` | Size64 path table (4K: header only) |

Empty path tables (5, 8, 10 with no entries) contain only the 8-byte header
`0001 0000 | 00000000 00000000`.

### 4.2 `BomInfo` (block 1)

```
u32  version                 # observed 1
u32  number_of_paths         # highest path_id + 1 (path id 0 is unused); see below
u32  number_of_info_entries  # 1 when any non-directory inode exists, else 0
number_of_info_entries * { u32 a; u32 b; u32 c; u32 d }   # observed 0 0 SUM 0
```

- `number_of_paths` = (total paths in the tree) + (#special files that consume
  a pid) + 1. Path ids are assigned 1..N in DFS/pre-order over readdir order;
  id 0 is unused and never appears in a `File` parent field (the root dir has
  parent id 0). Verified across fixtures: empty(1 path → 2), one(2 → 3),
  two(3 → 4), fix(4 → 5), p1(4 → 5), p2(7 → 8), p6(9 → 10), hme(5 leaf paths
  + 1 fifo → 7), q4(2 paths + 1 special → 4).
- `c` is the sum of `st_size` over **distinct** non-directory inodes: regular
  files are deduplicated by inode (hard links count once), symlinks are each
  counted with `st_size = strlen(target)`. Verified: fix = 30 (20 + 10), p1 = 1,
  p2 = 3, p6 = 4, hme = 8, y1 = 5 (symlink-only).
- `number_of_info_entries == 0` iff the tree contains only directories
  (`empty.bom`, `q4.bom`); the record is then 12 bytes and `c` is absent.

### 4.3 `Tree`

```
u8[4]  magic                # "tree"
u32    version              # observed 1
u32    block_paths_index    # block holding this tree's Paths block (or 0)
u32    block_size           # allocation unit for its Paths blocks (0x1000; VIndex tree uses 0x80; trailer trees use 64)
u32    path_count           # number of paths in the tree
u8     a                    # observed 0
```

Fixed 21 bytes. Root trees for `Paths` (blk 2), `HLIndex` (blk 4), the VIndex
inner tree (blk 7), `Size64` (blk 9), and each hard-link group's trailer tree.

### 4.4 `Paths`

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

The meaning of the pair depends on the kind of Paths block:

- Main paths table (blk 3): entries are `(PathInfoIndex block, File block)`,
  sorted by `(parent_path_id, leaf name bytes)` — **not** by path id.
- Hard-link trailer path table: entries are `(empty block, full-path string
  block)`, sorted byte-wise by the full path string (`"./..."` form, nested
  `"./sub/deep/x"`).
- HLIndex path table (blk 5): entries are `(TreePtr block, PRPtr block)` (both
  4-byte blocks), sorted by the hard-link group's *first member's* path id.
- VIndex/Size64 tables: empty in every fixture.

`next/previous` chain multiple Paths blocks into a list (for trees with more
paths than fit in one block); all observed blocks are single (0/0).

### 4.5 `PathInfoIndex`

```
u32 path_id            # unique internal numeric id for the path (1-based; 0 = no parent/root)
u32 path_record_index  # block holding this path's PathRecord
```

Fixed 8 bytes.

### 4.6 `PathRecord`

```
u8   path_type      # 1 = regular file, 2 = directory, 3 = symbolic link
u8   a              # observed 1
u16  architecture   # observed 0xf
u16  mode           # st_mode incl. S_IFMT bits, e.g. 0x41ed = 40755 dir; 0x81a4 = 100644 file
u32  user           # uid
u32  group          # gid
u32  mtime          # seconds since epoch
u32  size           # bytes (st_size; dirs keep their st_size, e.g. 128)
u8   b              # observed 1
u32  checksum       # see 4.6a; directories: 0
u32  link_name_length   # strlen(target)+1 when symlink, else 0
link_name[link_name_length]   # only when link_name_length > 0
```

- Directory: 31 bytes total (fields through `link_name_length = 0`).
- Regular file: **35 bytes** — an extra 4 zero bytes after `link_name_length`.
- Symlink: `31 + link_name_length + 8` bytes (link bytes then 8 trailing zero
  bytes). Verified on y1/y4/y5: total lengths 45/52/42/46/60/103.

### 4.6a CRC32 algorithm

File/symlink checksums are CRC32 as implemented by Apple's `libstuff/crc32.c`
(cctools): MSB-first table over poly `0x04c11db7`, `crc` starts at 0, each
data byte does `crc = (crc << 8) ^ tab[(crc >> 24) ^ byte]`, then the byte
length is fed through the same step low-byte-first, and the result is
complemented (`~crc`). This is the classic BSD `cksum` crc (CRC-32 /CKSUM), not
a plain zlib-crc32 of the content.

Verified pairs: `hello` (5 B) → `0xC3F5812D`; `a.txt` (5 B) → `0x614D030D`;
`a.b.c` (5 B) → `0x1E5AADB9`; `prog` (20 B, fixture `bin/prog`) → `0x9BBD446F`;
`/usr/bin/cat` (74296 B) → `0x526E3329`; symlink checksum = cksum of the target
string. Symlink checksum verified via `y1`.

### 4.7 `File`

```
u32  parent_path_id   # path_id of parent directory (root dir: 0)
u8[] name             # NUL-terminated leaf name
```

### 4.8 `VIndex`

```
u32  a                 # observed 1
u32  tree_block_index  # block holding a Tree serving as the actual VIndex index (7)
u32  b                 # observed 0
u8   c                 # observed 0
```

Fixed 13 bytes.

### 4.9 `Size64` placeholder

Never populated in any fixture: tree (blk 9) with pcount 0, empty 4K path
table (blk 10). Its tree's `block_size` is 0x1000.

## 5. Path emission order (dir mode)

1. Assign path ids by depth-first pre-order over `readdir` order; root = 1.
   Hard-link group *members* are ordered by this pid too (encounter order).
2. Emit a `(PathRecord, File, PathInfoIndex)` triplet per path, in pid order,
   into blocks 11+. All triplets belong to the main tree.
3. Non-directory, non-symlink, non-regular special files (fifo, socket):
   consume a path id but emit no blocks and no tree entry. `lsbom` omits them.
4. Main `Paths` block 3 entries are sorted by `(parent_path_id, name)`, with
   each entry referencing the triplet's PathInfoIndex/File blocks.

## 6. Hard-link groups and the trailer

Multiple names sharing one inode form a group. The **first** member in pid
order gets the shared `PathRecord`. Subsequent members follow this algorithm
(for group size *g* known up front — the writer pre-scans `(dev,ino)` before
emitting):

- **1st member**: shared PR, File, PII (normal triplet).
- **2nd member**: File, PII, then the trailer, then string/empty block pairs
  for each member processed so far, in pid order:
  1. Trailer `Tree`: `"tree"` ver 1, `block_paths_index` = trailer Paths block,
     `block_size` = **64**, `path_count` = *g* (final group size, including
     members not yet emitted).
  2. Trailer `Paths`: header `(1, g, 0, 0)` (block_size 64, no allocation
     padding); `g` entries sorted byte-wise by full path string.
  3. `PRPtr`: 4-byte block = shared PathRecord block index.
  4. `TreePtr`: 4-byte block = trailer Tree block index.
  5. Per processed member, in pid order: a string block `"./full/path\0"`
     (nested paths use `"./sub/deep/x"` form) then an empty block (length 0).
- **3rd+ member**: File, PII at its walk position, then its own string + empty
  block pair.

The trailer's Paths entries may reference string blocks allocated later, so the
writer must know each group's full membership *and* each member's pid before
beginning emission.

Verified: `p1` (3-way a/c/b; trailer after 2nd member c: TREE 19, PATHS 20,
PRPtr 21, TreePtr 22, str/empty a 23/24, c 25/26; 3rd member b 27-30);
`p2` (2-way a/b + 3-way c/d/e); `hme` (c/d then a/b, trailing strings at 26/28
next to 3rd members); `p6` (4× 2-way groups s/s_h, t/t_h, r/r_h, q/q_h — first
member pids 2, 3, 4, 7); `hlx` (nested hard links x2/x1 at `./sub/deep/x2`,
`./sub/x1`).

**HLIndex blocks 4-5**: `Tree.path_count` = number of groups; Paths entries =
`(TreePtr_block, PRPtr_block)` per group, sorted by first-member path id
(e.g. p6 entries `(38,37) (28,27) (51,50) (61,60)` — HLPaths order is
independent of trailer emission order).

## 7. Canonical layout in Apple-generated files

Blocks are packed from 0x200 in *allocation* order, but the block **order in
the file is unrelated to block index order**: Apple buffers index metadata and
writes it last, and observed offsets are frequently *non-monotonic* in index
order (verified in `p6.bom`/`fix.bom`). The block index and vars index are the
last two structures (`blocks_index_offset + length == filesize`).

Because `lsbom` resolves everything through the index, our writer lays blocks
out contiguously in index order starting at 0x200 with no free list and a pool
of exactly `number_of_blocks`; the index (offset,length) pairs and all block
contents/sizes match mkbom's. This satisfies the accepted conformance bar.

## 8. Round-trip calibration notes

- Pool size 2730 vs smaller pool: `lsbom` only reads populated entries
  (1..number_of_blocks), so a smaller pool is safe. Confirm during conformance
  testing.
- `-s` (simplified) mode (`s1.bom`) differs in PathRecord/PII emission and is
  out of scope for the dir-mode writer milestone.
- VIndex/Size64 subtrees are always empty in modern mkbom output; treated as
  fixed placeholders.

## 9. lsbom query behavior (oracle)

- Entries are walked DFS pre-order from the root (parent_pid == 0), children
  in Paths-row order; the root prints as `.`, children as `./name`.
- Default line: `path\t<mode, u16 octal>\tuid/gid`; non-directories then add
  `\tsize\tchecksum`; symbolic links append `\ttarget` (stored link bytes).
- Type filters (`-f -d -l -b -c`) select on PathRecord.path_type (1 file, 2
  dir, 3 link, 4 block, 5 char), *not* on mode bits; filters union.
- `-x` drops the mode column for dirs and links; `-m` appends a ctime-formatted
  mtime column only for regular files.
- Block/char devices print `path\tmode\tuid/gid\t<signed int32 checksum>`; the
  size column is omitted. path_type 6/7 aborts that entry with
  `filesystem object has an invalid type: 0x6` + `Cannot dearchive.` on stderr.
- `-p` prints one tab-joined cell per requested letter, order-preserving;
  directories yield empty cells for `t` `T` `s` `S` `c`. `S` groups thousands
  with commas (hardcoded, locale-independent).
- `--arch` matches PathRecord.architecture: `0xf` (any) matches every request;
  a non-f entry that does not match the requested arch prints with all-zero
  metadata (`path\t0\t0/0\t0\t0`); the default (no `--arch`) requests x86_64.
  Recognized names: ppc, i386, hppa, sparc, ppc64, x86_64, any.