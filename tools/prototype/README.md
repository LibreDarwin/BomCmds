# Prototype BOM writer (reference implementation)

Clean-room, dir-mode `.bom` writer in Python. It was used to pin down and
verify every byte-level detail of the format (see `FORMAT.md`) before the C
port, and to produce reference outputs that the C implementation will be
tested against.

It is self-contained: `run_tests.py` builds fixture trees from scratch,
runs `/usr/bin/mkbom` and `mkbom_proto.py` on each, and compares the results
two ways:

  * *block-level* — same `number_of_blocks`, and identical block length and
    block content bytes for every index (resolved through the index);
  * *output-level* — `/usr/bin/lsbom` prints reference and produced files
    byte-identically.

Physical file offsets and the index free list are allowed to differ (they are
not reachable through the block index), per the conformance bar in
`local/BomCmds.md` and `FORMAT.md` §2.

## Run

    python3 run_tests.py          # exit 0 = all fixtures conformance-identical

Scratch files land under `.test_tmp/` (gitignored).

## Validation status

Current results: **11/11 fixtures OK** (block-identical and lsbom-identical),
covering:

| class | fixtures |
|---|---|
| empty dir | `empty` |
| empty file only | `one`, `two` (with empty dir) |
| symlink only | `syme` |
| mixed (dirs, files, empty file, unicode name, rel/abs/dir symlinks) | `basic` |
| hard-link groups: 2-way / 3-way / 4-way | `hl2`, `hl3`, `hl4` |
| multiple groups in one tree | `hl_multi` |
| nested-dir group members | `hl_nested` |
| byte-wise (parent, name) ordering vs pid order | `ord` |

Context from the original workspace this prototype was validated against
(30 fixture directories, incl. 4-way groups `p6`, nested `hlx`, symlink-only
`y1`–`y5`, `mkbom -s` variants) is archived in the analysis notes; those
results are equivalent to the fixtures above.

## Key format facts the prototype encodes (see FORMAT.md for full detail)

* Fixed blocks `1`–`10`: `BomInfo` (28 B when files present, else 12 B),
  Paths tree (bpi=3, bsize 0x1000), Paths table, HLIndex tree (bpi=5,
  bsize 0x1000), HLPaths table, `VIndex` (13 B  `00000001 00000007 00000000
  00`), VIndex tree (bpi=8, bsize 0x80), VIndex paths (128 B), Size64 tree
  (bpi=10, bsize 0x1000), Size64 paths (4096 B).
* Paths entries, per block kind: main `(PII block, File block)` sorted by
  (parent_pid, name bytes); trailer `(empty block, string block)` sorted by
  the `"./..."` path string; HLPaths `(TreePtr block, PRPtr block)` sorted by
  first-member pid.
* `number_of_blocks` == last allocated block index (empty trailing slots
  included as `(0, 0)` index entries); index slot 0 is always the null
  `(0,0)` entry.
* path ids are DFS pre-order over `readdir` order; `PathRecord` layouts:
  dir 31 B, file 35 B (extra 4 zero), symlink `31 + linklen + 8`.
* checksums: BSD `cksum` CRC-32 (poly `0x04c11db7`, length fed low-byte-first)
  over file content / symlink target (`crc.py`).
* hard-link groups are detected by `(st_dev, st_ino)`: 1st member gets
  shared PR + File + PII; 2nd member additionally the trailer
  (TREE + PATHS + PRPtr + TreePtr) plus string/empty pairs emitted in member
  encounter order; 3rd+ members File + PII + string + empty.

## Mapping to the C port

The port of `mkbom` dir mode can follow the prototype's function layout:

| prototype | C module (proposed) |
|---|---|
| `crc.cksum` (crc.py) | `libbom`/`bom_cksum.c` |
| `scan_dir` (readdir DFS, pre-order pids, symlink-mode `lstat`) | `fs_walk.c` |
| `relpath` | `fs_walk.c` |
| group detection `(st_dev, st_ino)` + member rank | `bom_hl.c` |
| `build_pr` (per-type PathRecord) | `bom_store.c` (writer) |
| block allocation/simulation (single index counter) | `bom_writer.c` |
| fixed blocks + trailer assembly (`main` emission phase) | `bom_writer.c` |
| file layout: header, contiguous blocks, slot-0 index, vars | `bom_store.c` / `bom_write.c` |

The C port is not required to match the prototype's exact allocation order;
it must reproduce the same index `(offset,length)` pairs and block contents,
which `run_tests.py` verifies against the system `mkbom`.