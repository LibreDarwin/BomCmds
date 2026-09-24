# BomCmds

Clean-room, BSD-3 reimplementation of Apple's BOM tooling for the
LibreDarwin release-control toolchain.

`xbs buildit -image` (ReleaseControl) and friends need a
Bill-of-Materials of a built root.  We reproduce, byte-compatible,
the on-disk `.bom` format and the tools that read and write it:

  - `libbom`       - the BOM store: block table, variable data, and
                     the B-tree index that maps paths to object
                     summaries (the format formerly served by Apple's
                     Bom.framework).
  - `mkbom`        - walk a directory (or read an lsbom-format
                     listing with `-i`) and emit a `.bom`.
  - `lsbom`        - pretty-print a `.bom` (path, mode, uid/gid,
                     size/mtime, checksums, tree/bom views).

## Why clean-room

The public BomCmds sources available are unlicensed, and Apple's
Bom.framework is a private framework.  This project therefore
reimplements the format and behavior from the ground up, without
deriving from either, using only:

  - facts about the on-disk format observed from real `.bom` files,
  - black-box behavior of the system `mkbom`/`lsbom` as the oracle.

See FORMAT.md for the peer-reviewed format notes that anchor the
implementation.  Compliance is proven by round-trip tests: our
`mkbom` output must be readable by the system `lsbom`, and our
`lsbom` must read system-produced boms byte-identically in output.

## Layout

    bom/    the library (store, block table, index, fs objects)
    cmd/    the command tools (mkbom, lsbom)
    tests/  round-trip and conformance fixtures

## Building

    make            # builds libbom.a, mkbom, lsbom
    make test       # round-trip and conformance checks

## License

BSD 3-Clause, Copyright (c) 2026 LibreDarwin.  See LICENSE.