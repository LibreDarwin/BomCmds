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

## 10. CPIO archive format (`ditto -c`, `070707` odc)

All numeric fields are octal ASCII. There is **no per-entry padding**: each
entry is `header(76) + name(namesize, NUL-terminated) + data(filesize)`, next
header immediately after. Trailer entry `TRAILER!!!` (name size 11 incl NUL),
all numeric fields 0, nlink=1; then the whole stream is zero-padded up to the
next 512-byte multiple (the pad is part of `-c`, and of `-z`/`-j` — see §12).

Header field layout (offsets within the 76-byte header):

| offset | size | field |
|--------|------|-------|
| 0 | 6 | magic `070707` |
| 6 | 6 | dev |
| 12 | 6 | ino |
| 18 | 6 | mode |
| 24 | 6 | uid |
| 30 | 6 | gid |
| 36 | 6 | nlink |
| 42 | 6 | rdev |
| 48 | 11 | mtime (octal) |
| 59 | 6 | namesize (incl trailing NUL) |
| 65 | 11 | filesize |

Field semantics (verified against oracle):
- `inos` are sequential synthetic counters 0..N-1, assigned in entry order and
  shared across a whole archive run (e.g. `c.cpio` inos for 12 entries are
  0..11, with `TRAILER!!!` taking the next value 11).
- Hard-linked files: both entries carry the **same ino**, and **both carry the
  data copy** (nlink=2 on each, size/data repeated) — verified `hl2.cpio`.
- Dir `nlink` = 2 + number of direct children in the source dir (not st_nlink
  semantics: `c/` has 4 children → nlink 6; `c/sub` has 1 child → 3).
- `._dir` sidecar inherits the dir's nlink; files' sidecars nlink=1; symlinks
  nlink=1.
- Symlinks: mode `0120755`, filesize = len(target), content = target bytes.
- mtime is the source mtime (octal) — no mtime=0, no atime in CPIO.
- Entry order = `readdir` order of the source directory; per directory the
  order is `[dir, <children...>, ._dir]` where each child is emitted as
  `[<name>, <._name>]` (sidecar immediately after its item). `.` root is the
  first entry with name `"."`. No atime is stored in CPIO headers.

## 11. AppleDouble `._` sidecar + ATTR blob

The AppleDouble file backing each `._name` entry. Layout of the 50-byte header:

| offset | size | field |
|--------|------|-------|
| 0 | 4 | magic `0005 1607` (BE) |
| 4 | 4 | version `0002 0000` |
| 8 | 16 | filler: `"Mac OS X"` + 8 spaces |
| 24 | 2 | nentries (always 2) |
| 26 | 24 | entries: `{u32 type, u32 offset, u32 length}` ×2 |

Two entries always present: entry 0 type 0x9 (AppleDouble attributes -> the
ATTR blob), entry 1 type 0x2 (resource fork). For files without a resource
fork, entry 1 has `offset = EOF (total file size)` and `length = 0`; with a
resource fork it holds the fork bytes (`offset = 163, length = fork size`,
verified `_data.bin` size 181 = 163 + 18 fork bytes).

ATTR blob (entry-0 payload):
- 32 bytes FinderInfo (zeros) + 2 bytes pad
- `'ATTR'` magic at blob+34
- `debug_tag` u32 (0) at +38, `total_size` u32 = whole AD file size at +42,
  `data_start` u32 at +46 (file-offset of the value area), `data_length` u32
  (sum of value sizes) at +50, `reserved[3]` at +54, u16 `flags`=0 at +66,
  u16 `num_attrs` at +68.
- Records follow at blob+70: each `{u32 offset, u32 length, u16 flags, u8
  namelen incl NUL, name..., pad-to-4}` where record length =
  `((11 + namelen + 3) & ~3)`.
- Record offsets are **absolute offsets within the whole AD file** (50-byte
  header + blob index). Values are packed contiguously in value area, in
  `listxattr` order, ending exactly at EOF.
- Verified sizes: 0 xattrs→ no sidecar; provenance-only → 163-byte AD (blob
  113); provenance + 1 xattr → 193 (blob 143); provenance + 3 xattrs → 247
  (blob 197: `v`@212 len1, provenance@213 len11, `long`@224 len21, `xy`@245
  len2).
- `com.apple.provenance`: on-disk value is **11 bytes** `01 02 00 1b e2 9d 29
  ad 6b aa b4` (matches `xattr -x`); the AppleDouble stores the same 11 bytes
  verbatim. The 8-byte tail `1b e2 9d 29 ad 6b aa b4` is a constant signature
  present on every provenance sample (system-generated on new files too).
- `--norsrc` suppresses AppleDouble sidecars entirely (including dir `._path`
  entries) but `com.apple.provenance` still round-trips because APFS
  regenerates it on extraction; user xattrs are lost.
- Resource fork: stored in AD entry-1 (type 2), round-trips through `-x`.

## 12. `-z` / `-j` wrappers

- `-z`: standard gzip over the **fully padded** CPIO stream (incl. trailing
  zero pad to 512). Header `1f 8b 08 00 | mtime=0 (4B) | XFL=0 | OS=3 (Unix)`,
  raw deflate payload == zlib level 6. File name flag never set. Decompressed
  inner == `-c` output exactly.
- `-j`: bzip2 (`BZh91AY&SY…`, block size 9), inner == `-c` output exactly.
- `-z -k` is an error: `ditto: -z is only for cpio archives`.
- `-x` accepts `-z`/`-j` (matching the magic) and yields the same result as
  `-x` of the plain `-c` archive.

## 13. PKZip (`-k`) — ditto's zip writer

Entries are stored **without** the `./` prefix (CPIO uses `./`, zip uses bare
relative names) and dirs get a trailing `/`; no `.` root entry. Entry order
and sidecar pairing match CPIO exactly.

Local header: `PK\x03\x04`, version-needed 10 (stored) / 20 (deflated),
flags 0 (stored) / 0x8 (deflated → data descriptor follows), method 0
(stored) / 8 (deflate), DOS timestamp (see below), then crc/comp/uncomp
= 0 for deflated (in descriptor, bit 3 set) / real for stored. Extra field
`0x5855` (12 bytes): atime(4) + mtime(4) + uid(2) + gid(2) — unix seconds,
LE. **Central directory** extra is `0x5855` 8 bytes (atime+mtime only, no
uid/gid).

Central header: `PK\x01\x02`, version made-by `0x0315` (unix/2.1), version
needed 10/20 matching compression, flags/method as local, DOS timestamp,
real crc/comp/uncomp, external attrs = `mode << 16 | 0x4000` (low word is
0x4000 const). Data descriptor: `PK\x07\x08` + crc + comp + uncomp.

- DOS timestamp = source mtime encoded `(dosDate << 16) | dosTime`, i.e. high
  word = `((year-1980) << 9) | (month << 5) | day`, low word =
  `(hour << 11) | (min << 5) | (sec/2)`, with seconds **rounded up to the
  next even second** (`(sec+1)/2`, carries) — verified `03:04:09 -> 03:04:10`,
  `03:04:59 -> 03:05:00`.
- Compressed with zlib deflate level 6 (raw, no zlib header); verified
  byte-identical across stored streams incl. a 34K semi-compressible file
  where levels 5 and 7 differ.
- Stored-vs-deflated rule: directories(method 0), symlinks(method 0), and
  empty files(method 0); regular non-empty files → deflate (method 8).
- Symlink entry: mode `0120755`, content = target, **stored**; no `0x5855`
  extra field (empty extra), but DOS timestamp still present.
- `-c -z -k` (gzip+zip) unsupported (error, see §12).
- On `-x -k`, ditto splits zip members at the `_` sidecar pairs to apply
  resources and re-creates symlinks from the symlink entries.

## 14. `-x` extraction behavior (oracle)

- Header: `>>> Copying <src>\n` (src is the archive path as invoked).
- `-x` verbose lines: `copying file ./name ... `, `N bytes for ./name`.
  AppleDouble members print `N bytes for ./name__` (**two** trailing
  underscores, no trailing space) — the `_` marks the EA/AppleDouble pass.
- Symlink entries: `copying symlink ./name ... ` at member position, then
  `linked ./name` **after** all files (deferred to end). Regular file copies
  print `copying file` + `N bytes for` immediately.
- Extraction mtime source: the `0x5855` unix seconds (exact), not DOS
  (verified odd-second round-trips exactly); resource forks restored; sidecar
  (._) members are consumed to set xattrs, not left as files.
- `-x -k` paths have no `./` prefix; `-x` of CPIO/gzip/bzip2 print `./`-prefixed
  names.

## 15. `--arch` thinning (copy mode)

- `ditto --arch <name> src dst`: for Mach-O **fat** inputs, writes the
  requested slice byte-identical to the source slice file; for already-thin
  matching inputs, copies as-is (byte-identical); full dir copies recurse with
  the flag set.
- Unknown name: `ditto: can't get arch info for '<name>'` then
  `ditto: Could not parse the Mach-O architectures to copy`, rc=1.
- `-V` header shows the arch in brackets: `>>> Copying thin.univ [arm64]`.
- Slice extraction does not alter the slice bytes; dst mtime set from src,
  dst atime = now. Recognized names: ppc, i386, hppa, sparc, ppc64, x86_64,
  any (per §9 table).

## 16. Create-side oracle rules (`ditto -c`)

Verified against the oracle (macOS 15/16 ditto):

- **Orphan `._` entries are dropped at create.** Non-directory entries whose
  name begins with `._` in a source directory are skipped entirely (no member,
  no sidecar) — verified with a `._leading` file: oracle cpio emits neither
  `._leading` nor `._._leading`. This applies to both CPIO and PKZip.
- **`._`-prefixed directories are archived normally** (with contents and their
  own `._._name` sidecar).
- **Symlink sources are followed.** `ditto -c <symlink> dst` resolves the link;
  the target's content is archived under the **target's** basename
  (`lk -> real` yields member `./real`, not `./lk`). A symlink to a directory
  archives the directory contents (the resolved dir becomes the archive root,
  same as `-c <dir>`). If `realpath()` fails (broken link): `ditto: Cannot get
  the real path for source '<src>'` (echoes the argument verbatim), rc=1, no
  output. Applies to cpio, zip, and `-z`/`-j`.
- **Direct `._`-named non-directory sources are rejected** with the same
  `Cannot get the real path` error — except that a symlink named `._x` whose
  target resolves to a non-`._` name is accepted (the check runs on the
  resolved basename).
- **Broken symlinks inside a directory** are archived as normal symlink
  members (readlink content), no error.
- **Header echo**: `-V -c <symlink> dst` prints `>>> Copying <symlink>` (the
  original argument), then `copying file ./<targetbase> ... `.
- The zip `0x5855` extras carry the **real atime** of the source on every
  member (sidecars included); atime != mtime on live files.
- **Multi-archive `-x` oracle bug (not replicated):** when any archive contains
  directory members with `._` sidecars, the oracle fails the second archive at
  its top-level `._` members with `ditto: da2//._e: No such file or directory`
  (double-slash path, partial extraction, rc=1) — a plain single-archive `-x`
  of the same archive succeeds. Our tool extracts correctly (rc=0); the
  conformance battery therefore uses file-only multi-archive fixtures and this
  oracle defect is documented, not copied.
- `-x -k` on a non-PKZip input: `ditto: Couldn't read PKZip signature`, rc=1.
- **rearch byte-identity caveat:** oracle zip/cpio `-c` embeds the source
  mtime/atime; two tools only produce byte-identical archives when they are
  run from the same extracted tree within the same wall-clock second. Beyond
  that window the battery compares structurally (ignoring mtimes).

## 17. `Bom.framework` container

The built `Bom.framework` is a byte-identical replica of Apple's private
`/System/Library/PrivateFrameworks/Bom.framework` container on macOS 26.5.
On that system Apple's framework ships **no code and no headers** on disk
(the dylib lives in the dyld shared cache); only the container remains:

```
Bom.framework/
  Bom -> Versions/Current/Bom           (symlink, target "Versions/Current/Bom")
  Resources -> Versions/Current/Resources (symlink)
  Versions/
    Current -> A                        (symlink)
    A/
      _CodeSignature/CodeResources      byte-identical to Apple's (rule-only
                                        plist, no file hashes)
      Bom                               our dylib linked from the libbom objects
      Resources/
        Info.plist                      byte-identical (com.apple.bom, CFBundle
                                        ShortVersionString 14.0, CFBundleVersion
                                        277, LSMinimumSystemVersion 26.5)
        version.plist                   byte-identical (BuildVersion 3135)
```

- `src/libbom/{Info.plist,version.plist,CodeResources}` are committed
  byte-for-byte from the reference (verify with `cmp`); `shasum -a 256`
  matches the reference files exactly.
- There is intentionally **no `Headers/`** directory — Apple ships none, so
  shipping ours would break byte identity. The public headers stay in
  `src/libbom/` for the tool builds.
- No `versions` plist key; Apple's `Info.plist` carries no
  `CFBundleVersion`-dependent symlink at framework top level either.
- Signing: builds leave `_CodeSignature/CodeResources` as the verbatim Apple
  file and do **not** re-sign. Import the built framework elsewhere with
  `codesign -f -s - Bom.framework`, which rewrites `CodeResources` and adds a
  seal; the dylib itself has no signature either way.