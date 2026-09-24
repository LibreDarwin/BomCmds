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
  - `ditto`        - directory archiver/extractor reproducing Apple's
                     cpio (`-c`), PKZip (`-k`), gzip (`-z`) and
                     bzip2 (`-j`) byte formats plus the `--arch`
                     thinning and copy semantics.
  - `Bom.framework` - byte-identical replica of Apple's private
                     framework container (Info.plist, version.plist,
                     CodeResources, symlinks) with the libbom objects
                     linked into `Versions/A/Bom`.

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

    src/libbom/     the library (store, block table, index, fs objects),
                    plus the framework metadata (Info.plist, version.plist,
                    CodeResources) committed byte-for-byte from Apple's
                    private framework
    src/mkbom/      the mkbom command
    src/lsbom/      the lsbom command
    src/ditto/      the ditto command
    tools/          conformance batteries (mkbom/lsbom/ditto)
    man/            manual pages

## Building

    make            # builds mkbom, lsbom, ditto and Bom.framework
    make test       # round-trip and conformance checks (all three tools)
    make install    # binaries + man pages; framework -> $(PREFIX)/Library/Frameworks

    # Alternative build system (Xcode):
    xcodebuild -project BomCmds.xcodeproj -target X build    # X in {mkbom, lsbom, ditto, Bom}

## License

BSD 3-Clause, Copyright (c) 2026 LibreDarwin.  See LICENSE.