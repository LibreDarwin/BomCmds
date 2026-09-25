# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026, LibreDarwin
"""Round-trip conformance runner for the clean-room BOM writer prototype.

For every fixture tree built in a scratch dir it runs the *reference*
mkbom (default /usr/bin/mkbom, the oracle) and the clean-room writer under
test (default the Python prototype; pass --subject to test the C port),
then verifies equivalence two ways:

  1. block-level: number_of_blocks, and for every block index both the
     recorded length and the exact block content bytes are identical;
  2. output-level: `/usr/bin/lsbom` prints reference and produced files
     byte-identically.

Physical file offsets and the free list are allowed to differ (they are
not reachable through the block index), per the conformance bar in
local/BomCmds.md and FORMAT.md section 2.

Usage: python3 run_tests.py [--subject CMD] [--ref-mkbom CMD]
"""
import argparse
import os
import os.path
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROTO = os.path.join(HERE, 'mkbom_proto.py')
LSBOM = '/usr/bin/lsbom'
_LSBOM_SUBJECT = None
BE = '>'


def mk(op, base):
    """op ∈ {dir, file, ln, link}; creates under base. Returns None."""
    kind, rest = op
    if kind == 'dir':
        os.makedirs(os.path.join(base, rest), exist_ok=True)
    elif kind == 'file':
        path, content = rest
        path = os.path.join(base, path)
        d, _ = os.path.split(path)
        if d:
            os.makedirs(d, exist_ok=True)
        with open(path, 'wb') as f:
            f.write(content)
    elif kind == 'ln':
        target, path = rest
        path = os.path.join(base, path)
        d, _ = os.path.split(path)
        if d:
            os.makedirs(d, exist_ok=True)
        os.symlink(target, path)
    elif kind == 'link':
        src, dst = rest
        os.link(os.path.join(base, src), os.path.join(base, dst))


def mk_tree(root):
    """Build the fixture trees this runner validates (deterministic content)."""
    fixtures = {
        'empty': [],
        'one':      [('file', ('a.txt', b''))],
        'two':      [('file', ('f.txt', b'')), ('dir', 'd')],
        'syme':     [('ln', ('a.txt', 'l'))],
        'basic': [
            ('dir', 'sub'),
            ('dir', 'sub/deep'),
            ('file', ('sub/a.txt', b'hello world\n')),
            ('file', ('sub/deep/x2', b'x')),
            ('file', ('dir2/b.txt', b'longer file content line')),
            ('ln', ('../sub/a.txt', 'sub/lnk_rel')),
            ('ln', ('dir2/b.txt', 'sub/lnk_absl')),
            ('ln', ('sub', 'dirlnk')),
            ('file', ('empty_f', b'')),
            ('file', ('uni-\xc3\xa9.txt', b'u')),
        ],
        'hl2':      [('file', ('x', b'same')), ('link', ('x', 'x2'))],
        'hl3':      [('file', ('a', b'z')), ('link', ('a', 'b')), ('link', ('a', 'c')),
                     ('file', ('solo', b'other'))],
        'hl4':      [('file', ('q', b'w')), ('link', ('q', 'r')),
                     ('link', ('q', 's')), ('link', ('q', 't'))],
        'hl_multi': [
            ('file', ('g1/g.txt', b'gg')), ('file', ('g2/i.txt', b'ii')),
            ('link', ('g1/g.txt', 'g1/h.txt')), ('link', ('g2/i.txt', 'g2/j.txt')),
        ],
        'hl_nested': [
            ('file', ('sub/deep/x2', b'n')), ('link', ('sub/deep/x2', 'sub/x1')),
        ],
        'ord': [
            ('file', ('b_zz', b'1')), ('file', ('a_aa', b'2')),
            ('file', ('a_ab', b'3')), ('file', ('c', b'4')),
        ],
    }
    for name, items in fixtures.items():
        d = os.path.join(root, name)
        os.makedirs(d, exist_ok=True)
        for it in items:
            mk(it, d)


def lsbom(path):
    r = subprocess.run([LSBOM, path], capture_output=True, text=True)
    return r.returncode, r.stdout


def lsbom_battery(bom):
    """Compare /usr/bin/lsbom vs the clean-room lsbom (pass --subject-lsbom)
    on `bom` for every argument set of interest: stdout, stderr and exit
    status must all be byte-identical."""
    battery = [
        [],
        ['-s'], ['-f'], ['-d'], ['-l'], ['-b'], ['-c'], ['-m'], ['-x'],
        ['-sf'], ['-fd'], ['-l', '-s'], ['-m', '-x'],
        ['-p', ''], ['-p', 'f'], ['-p', 'F'], ['-p', 'fm'], ['-p', 'mA'],
        ['-p', 'uUGG'], ['-p', 'sc'], ['-p', 'tT'], ['-p', '/'], ['-p', '?'],
        ['-p', 'S'], ['-p', 'M'], ['-s', '-p', 'f'],
        ['--arch', 'x86_64'], ['--arch', 'any'], ['--arch', 'ppc'],
        ['-p', 'ff'], ['-p', 'z'], ['--arch', 'bogus'], ['-Z'],
    ]
    global _LSBOM_SUBJECT
    for args in battery:
        ref = subprocess.run([LSBOM] + args + [bom], capture_output=True,
                             text=True)
        out = subprocess.run(_LSBOM_SUBJECT + args + [bom], capture_output=True,
                             text=True)
        if (ref.returncode, ref.stdout, ref.stderr) != \
           (out.returncode, out.stdout, out.stderr):
            return ('MISMATCH', 'args=%r rc %d/%d' %
                    (args, ref.returncode, out.returncode))
    return None


def blockmap(path):
    """Return (nob, {index: block_bytes}) resolved via the file index."""
    d = open(path, 'rb').read()
    g = lambda o: struct.unpack(BE + 'I', d[o:o + 4])[0]
    nob = g(12)
    io = g(16)
    t0 = io + 4
    m = {}
    for i in range(1, nob + 1):
        off, l = struct.unpack(BE + 'II', d[t0 + 8 * i:t0 + 8 * i + 8])
        m[i] = d[off:off + l]
    return nob, m


def _compare(ref, out, label):
    """Block-index + lsbom comparison. Returns (status, detail) or None on OK."""
    rn, rm = blockmap(ref)
    on, om = blockmap(out)
    if rn != on:
        return 'BLOCK-DIFF', '%s: nob %d != %d' % (label, rn, on)
    diff = [i for i in range(1, rn + 1) if rm[i] != om[i]]
    if diff:
        return 'BLOCK-DIFF', '%s: blocks differ: %s' % (label, diff)
    if lsbom(ref) != lsbom(out):
        return 'LSBOM-DIFF', label
    return None


def run(fixture, trees, subject, ref_mkbom):
    scratch = os.path.join(HERE, '.test_tmp')
    os.makedirs(scratch, exist_ok=True)
    ref = os.path.join(scratch, fixture + '.ref.bom')
    out = os.path.join(scratch, fixture + '.out.bom')
    d = os.path.join(scratch, fixture)
    if os.path.isdir(d):
        shutil.rmtree(d)
    shutil.copytree(os.path.join(trees, fixture), d, symlinks=True)
    if subprocess.run([ref_mkbom, d, ref]).returncode != 0:
        return 'REF-MKBOM-FAIL', ''
    if subprocess.run(subject + [d, out]).returncode != 0:
        return 'SUBJECT-FAIL', ''
    bad = _compare(ref, out, 'dir')
    if bad:
        return bad
    # -s: path-only bom
    ref_s = os.path.join(scratch, fixture + '.ref.s.bom')
    out_s = os.path.join(scratch, fixture + '.out.s.bom')
    if subprocess.run([ref_mkbom, '-s', d, ref_s]).returncode != 0:
        return 'REF-MKBOM-FAIL', '-s'
    if subprocess.run(subject + ['-s', d, out_s]).returncode != 0:
        return 'SUBJECT-FAIL', '-s'
    bad = _compare(ref_s, out_s, '-s')
    if bad:
        return bad
    # -i: rebuild from /usr/bin/lsbom output of the reference bom
    rc, listing = lsbom(ref)
    if rc != 0:
        return 'REF-LSBOM-FAIL', ''
    listpath = os.path.join(scratch, fixture + '.ls.txt')
    with open(listpath, 'wb') as f:
        f.write(listing.encode('utf-8'))
    ref_i = os.path.join(scratch, fixture + '.ref.i.bom')
    out_i = os.path.join(scratch, fixture + '.out.i.bom')
    if subprocess.run([ref_mkbom, '-i', listpath, ref_i]).returncode != 0:
        return 'REF-MKBOM-FAIL', '-i'
    if subprocess.run(subject + ['-i', listpath, out_i]).returncode != 0:
        return 'SUBJECT-FAIL', '-i'
    bad = _compare(ref_i, out_i, '-i')
    if bad:
        return bad
    if _LSBOM_SUBJECT is not None:
        for bom in (ref, out):
            badb = lsbom_battery(bom)
            if badb:
                st, detail = badb
                return 'LSBOM-BATTERY-FAIL', detail + ' %s' % bom
    return 'OK', ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--subject', nargs='+', default=[sys.executable, PROTO],
                    help='writer under test (default: python prototype)')
    ap.add_argument('--ref-mkbom', default='/usr/bin/mkbom',
                    help='reference mkbom (default: /usr/bin/mkbom)')
    ap.add_argument('--lsbom', nargs='+', default=None,
                    help='clean-room lsbom to conformance-test')
    ap.add_argument('--subject-lsbom', action='store_true',
                    help='run the lsbom flag battery (needs --lsbom)')
    args = ap.parse_args()
    global _LSBOM_SUBJECT
    _LSBOM_SUBJECT = args.lsbom if args.subject_lsbom else None
    trees = os.path.join(HERE, '.test_tmp', '_trees')
    if os.path.isdir(trees):
        shutil.rmtree(trees)
    os.makedirs(trees, exist_ok=True)
    mk_tree(trees)
    fixtures = sorted(os.listdir(trees))
    failures = 0
    for fx in fixtures:
        st, detail = run(fx, trees, args.subject, args.ref_mkbom)
        if st != 'OK':
            failures += 1
            line = '%-12s FAIL %-14s %s' % (fx, st, detail)
        else:
            line = '%-12s OK' % fx
        print(line)
    print('%d fixtures, %d failure(s)' % (len(fixtures), failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())