# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026, LibreDarwin
"""Conformance runner for the clean-room ditto reimplementation.

For every fixture tree built in a scratch dir it drives the *reference*
/usr/bin/ditto (the oracle) and the clean-room ditto under test (--subject)
with identical arguments and verifies that:

  create   - the produced archives are byte-identical;
  extract  - the extracted trees are equivalent (diff -r);
  rearchive- the archives re-created from extracted trees are structurally
             identical (directory mtimes that the extractor intentionally
             leaves at the wall clock are outside this check; see FORMAT.md
             §14);
  copy     - multi-source copy -v/-V stderr and exit status match;
  errors   - exit status, stdout and stderr match.

Usage: python3 run_ditto_tests.py [--subject DITTO]
"""
import argparse
import io
import os
import os.path
import shutil
import struct
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ORACLE = '/usr/bin/ditto'

# Fixed epoch used for every fixture entry's mtime so that creates are
# byte-reproducible regardless of when the battery runs.
T0 = 1700000000


def set_mtimes(base):
    """Pin every entry (files and dirs, including symlink targets) below base."""
    base_t = T0
    names = []
    for root, dirs, files in os.walk(base):
        for d in dirs:
            p = os.path.join(root, d)
            os.utime(p, (T0, T0))
            names.append(p)
        for f in files:
            p = os.path.join(root, f)
            st = os.lstat(p)
            os.utime(p, (T0 + (len(p) % 7), T0), follow_symlinks=not os.path.islink(p))
            names.append(p)
    # directories get distinct times by depth/name so no two sibling dirs
    # share an mtime unless we mean to (the oldroots fixture below).
    return base_t


def mk_fixtures(root):
    def w(n, lines):
        p = os.path.join(root, n)
        d = os.path.dirname(p)
        os.makedirs(d, exist_ok=True) if d else None
        with open(p, 'wb') as f:
            f.write(lines)
        return p

    basic = os.path.join(root, 'basic')
    os.makedirs(os.path.join(basic, 'sub/deep'), exist_ok=True)
    w('basic/sub/a.txt', b'hello world\n')
    w('basic/sub/deep/x2', b'x')
    w('basic/dir2/b.txt', b'longer file content line')
    w('basic/empty_f', b'')
    w('basic/uni-\xc3\xa9.txt', b'u')
    w('basic/.hidden', b'h')
    os.symlink('../sub/a.txt', os.path.join(basic, 'sub/lnk_rel'))
    os.symlink(os.path.join(basic, 'dir2/b.txt'), os.path.join(basic, 'sub/lnk_absl'))
    os.symlink('/definitely/not/here', os.path.join(basic, 'lnk_broken'))

    # Old roots: non-leaf dir 'sub' shares the root mtime (deferred mtime
    # restore is skipped for it per FORMAT.md §14), leaves are restored.
    old = os.path.join(root, 'oldroots')
    os.makedirs(os.path.join(old, 'sub/deep'), exist_ok=True)
    os.makedirs(os.path.join(old, 'd1a'), exist_ok=True)
    w('oldroots/sub/g.txt', b'g')
    w('oldroots/sub/deep/x2', b'd')
    w('oldroots/d1a/f.txt', b'f')
    for p in (old, os.path.join(old, 'sub'), os.path.join(old, 'sub/deep'),
              os.path.join(old, 'd1a')):
        os.utime(p, (1701388800, 1701388800))
    w('oldroots/sub/g.txt', b'g')
    w('oldroots/sub/deep/x2', b'd')
    w('oldroots/d1a/f.txt', b'f')
    for p in (os.path.join(old, 'sub/g.txt'), os.path.join(old, 'sub/deep/x2'),
              os.path.join(old, 'd1a/f.txt')):
        os.utime(p, (1701388800, 1701388800))

    hl = os.path.join(root, 'hardlinks')
    os.makedirs(hl, exist_ok=True)
    x = w('hardlinks/x', b'same')
    os.link(x, os.path.join(hl, 'x2'))
    os.link(x, os.path.join(hl, 'x3'))
    w('hardlinks/solo', b'other')

    names = os.path.join(root, 'names')
    os.makedirs(names, exist_ok=True)
    w('names/a b c.txt', b'sp')
    w('names/._leading', b'dot')
    w('names/trailing\n', b'newline')

    set_mtimes(root)
    return sorted([n for n in os.listdir(root)
                   if os.path.isdir(os.path.join(root, n))])


def run(o, args, cwd):
    cmd = (o if isinstance(o, list) else [o]) + args
    r = subprocess.run(cmd, capture_output=True, cwd=cwd)
    return r.returncode, r.stdout, r.stderr


def arch_parts(arch):
    """Parse an odc cpio archive into [(name, fielddict, data)] ignoring mtime."""
    d = open(arch, 'rb').read()
    out = []
    i = 0
    while i + 76 <= len(d) and d[i:i + 6] == b'070707':
        ns = int(d[i + 59:i + 65].strip(b' \0') or b'0', 8)
        sz = int(d[i + 65:i + 76].strip(b' \0') or b'0', 8)
        name = d[i + 76:i + 76 + ns - 1].decode('utf-8', 'replace')
        fields = {
            'dev': int(d[i + 6:i + 12], 8),
            'ino': int(d[i + 12:i + 18], 8),
            'mode': int(d[i + 18:i + 24], 8),
            'uid': int(d[i + 24:i + 30], 8),
            'gid': int(d[i + 30:i + 36], 8),
            'nlink': int(d[i + 36:i + 42], 8),
            'rdev': int(d[i + 42:i + 48], 8),
        }
        data = d[i + 76 + ns:i + 76 + ns + sz]
        out.append((name, fields, data))
        i += 76 + ns + sz
        if i > len(d):
            break
    return out


def structural_equal_cpio(a, b):
    A, B = arch_parts(a), arch_parts(b)
    if len(A) != len(B):
        return False, 'entry count %d != %d' % (len(A), len(B))
    for (an, af, ad), (bn, bf, bd) in zip(A, B):
        if an != bn:
            return False, 'name %r != %r' % (an, bn)
        for k in ('dev', 'ino', 'mode', 'uid', 'gid', 'nlink', 'rdev'):
            if af[k] != bf[k]:
                return False, '%s %s != %s (%r)' % (an, k, af[k], bf[k])
        if ad != bd:
            return False, 'data differs for %r' % an
    return True, None


def structural_equal_zip(a, b):
    with zipfile.ZipFile(a) as za, zipfile.ZipFile(b) as zb:
        A, B = za.infolist(), zb.infolist()
        if [x.filename for x in A] != [x.filename for x in B]:
            return False, 'member order differs'
        for x, y in zip(A, B):
            for k in ('create_system', 'compress_type', 'flag_bits',
                      'external_attr'):
                if getattr(x, k) != getattr(y, k):
                    return False, '%r %s differs' % (x.filename, k)
            if x.CRC != y.CRC or x.file_size != y.file_size:
                return False, '%r size/CRC differs' % x.filename
            if x.is_dir():
                continue
            if za.read(x.filename) != zb.read(y.filename):
                return False, '%r data differs' % x.filename
    return True, None


def tree_equal(a, b):
    r = subprocess.run(['diff', '-r', '--no-dereference', a, b],
                       capture_output=True)
    return r.returncode == 0, r.stdout + r.stderr


def report(filename, label, status):
    line = '%-16s %-20s %s' % (label, filename, status)
    print(line.encode('utf-8', 'replace').decode('utf-8', 'replace'))
    return status == 'OK'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--subject', default=None,
                    help='clean-room ditto under test (default: none)')
    args = ap.parse_args()
    if args.subject is None:
        print('need --subject path to the clean-room ditto')
        return 1
    subj = [os.path.abspath(args.subject)]
    scratch = os.path.join(HERE, '.test_tmp', 'ditto')
    if os.path.isdir(scratch):
        shutil.rmtree(scratch)
    os.makedirs(scratch, exist_ok=True)
    work = os.path.join(scratch, 'w')
    os.makedirs(work, exist_ok=True)
    trees = os.path.join(scratch, 'trees')
    fixtures = mk_fixtures(trees)

    failures = 0

    # --- create: byte-identical archives --------------------------------
    for fx in fixtures:
        src = os.path.join(trees, fx)
        for fmt, tail, extra in (('cpio', '.c.cpio', []),
                                 ('zip', '.c.zip', ['-k'])):
            oa = os.path.join(work, '%s.ora.%s' % (fx, tail))
            sa = os.path.join(work, '%s.our.%s' % (fx, tail))
            rr, _, er = run([ORACLE], ['-c'] + extra + [src, oa], work)
            sr, _, es = run(subj, ['-c'] + extra + [src, sa], work)
            if rr != sr or er != es or not os.path.exists(oa) or \
               not os.path.exists(sa):
                bad = 'CREATE-DIFF rc=%d/%d' % (rr, sr)
                if not report(fx, '%s-create' % fmt, bad):
                    failures += 1
                continue
            if open(oa, 'rb').read() != open(sa, 'rb').read():
                if not report(fx, '%s-create' % fmt, 'CREATE-BYTES-DIFF'):
                    failures += 1
                continue
            report(fx, '%s-create' % fmt, 'OK')

    # --- extract: equivalent trees --------------------------------------
    for fx in fixtures:
        for fmt, tail, extra in (('cpio', '.c.cpio', []),
                                 ('zip', '.c.zip', ['-k'])):
            arch = os.path.join(work, '%s.ora.%s' % (fx, tail))
            if not os.path.exists(arch):
                continue
            dx = os.path.join(work, '%s.ora.x' % fx)
            dy = os.path.join(work, '%s.our.x' % fx)
            shutil.rmtree(dx, ignore_errors=True)
            shutil.rmtree(dy, ignore_errors=True)
            rr, _, er = run([ORACLE], ['-x'] + extra + [arch, dx + '/'], work)
            sr, _, es = run(subj, ['-x'] + extra + [arch, dy + '/'], work)
            if rr != sr or er != es:
                if not report(fx, '%s-extract' % fmt,
                              'EXTRACT-DIFF rc=%d/%d' % (rr, sr)):
                    failures += 1
                continue
            ok, why = tree_equal(dx, dy)
            if not ok:
                if not report(fx, '%s-extract' % fmt, 'TREE-DIFF'):
                    failures += 1
                continue
            report(fx, '%s-extract' % fmt, 'OK')

    # --- rearchive: structural equality ---------------------------------
    for fx in fixtures:
        for fmt, tail in (('cpio', '.c.cpio'), ('zip', '.c.zip')):
            dy = os.path.join(work, '%s.our.x' % fx)
            if not os.path.isdir(dy):
                continue
            ra = os.path.join(work, '%s.ora.re.cpio' % fx)
            sa = os.path.join(work, '%s.our.re.cpio' % fx)
            extra = ['-k'] if fmt == 'zip' else []
            rr, _, er = run([ORACLE], ['-c'] + extra + [dy, ra], work)
            sr, _, es = run(subj, ['-c'] + extra + [dy, sa], work)
            if rr != sr or er != es:
                if not report(fx, '%s-rearch' % fmt, 'REARCH-DIFF rc=%d/%d' % (rr, sr)):
                    failures += 1
                continue
            fn = structural_equal_zip if fmt == 'zip' else structural_equal_cpio
            ok, why = fn(ra, sa)
            if not ok:
                if not report(fx, '%s-rearch' % fmt, 'STRUCT-DIFF: %s' % why):
                    failures += 1
                continue
            report(fx, '%s-rearch' % fmt, 'OK')

    # --- multi-archive extraction ----------------------------------------
    s2 = os.path.join(work, 'm1')
    s3 = os.path.join(work, 'm2')
    shutil.rmtree(s2, ignore_errors=True)
    shutil.rmtree(s3, ignore_errors=True)
    os.makedirs(s2, exist_ok=True)
    os.makedirs(s3, exist_ok=True)
    with open(os.path.join(s2, 'onlyA'), 'wb') as f:
        f.write(b'AAA')
    with open(os.path.join(s3, 'onlyB'), 'wb') as f:
        f.write(b'BBB')
    m1a, m1b = os.path.join(work, 'm1.cpio'), os.path.join(work, 'm2.cpio')
    run([ORACLE], ['-c', s2, m1a], work)
    run([ORACLE], ['-c', s3, m1b], work)
    da = os.path.join(work, 'mda')
    db = os.path.join(work, 'mdb')
    shutil.rmtree(da, ignore_errors=True)
    shutil.rmtree(db, ignore_errors=True)
    rr, _, er = run([ORACLE], ['-x', m1a, m1b, da + '/'], work)
    sr, _, es = run(subj, ['-x', m1a, m1b, db + '/'], work)
    okx = rr == sr and er == es and tree_equal(da, db)[0]
    if okx:
        report('multi-arch', 'create+extract', 'OK')
    else:
        report('multi-arch', 'create+extract', 'FAIL rc=%d/%d' % (rr, sr))
        failures += 1
    shutil.rmtree(da, ignore_errors=True)
    shutil.rmtree(db, ignore_errors=True)
    rr, _, er = run([ORACLE], ['-V', '-x', m1a, m1b, da + '/'], work)
    sr, _, es = run(subj, ['-V', '-x', m1a, m1b, db + '/'], work)
    if rr == sr and er == es:
        report('multi-arch', '-V verbose', 'OK')
    else:
        report('multi-arch', '-V verbose', 'FAIL')
        failures += 1

    # --- copy: multi-source -v / -V -------------------------------------
    for v in ('-v', '-V'):
        csrc = os.path.join(work, 'csrc')
        csrc2 = os.path.join(work, 'csrc2')
        shutil.rmtree(csrc, ignore_errors=True)
        shutil.rmtree(csrc2, ignore_errors=True)
        os.makedirs(os.path.join(csrc, 'd'), exist_ok=True)
        os.makedirs(csrc2, exist_ok=True)
        with open(os.path.join(csrc, 'f.txt'), 'wb') as f:
            f.write(b'x')
        with open(os.path.join(csrc2, 'g.txt'), 'wb') as f:
            f.write(b'y')
        cda = os.path.join(work, 'cda')
        cdb = os.path.join(work, 'cdb')
        shutil.rmtree(cda, ignore_errors=True)
        shutil.rmtree(cdb, ignore_errors=True)
        rr, _, er = run([ORACLE], [v, csrc, csrc2, cda], work)
        sr, _, es = run(subj, [v, csrc, csrc2, cdb], work)
        if rr == sr and er == es and tree_equal(cda, cdb)[0]:
            report('copy', '%s 2-src' % v, 'OK')
        else:
            report('copy', '%s 2-src' % v, 'FAIL rc=%d/%d' % (rr, sr))
            failures += 1

    # --- error battery: rc + stdout + stderr -----------------------------
    sw = os.path.join(work, 'cw')
    shutil.rmtree(sw, ignore_errors=True)
    os.makedirs(sw, exist_ok=True)
    with open(os.path.join(sw, 'f'), 'wb') as f:
        f.write(b'f')
    os.symlink('f', os.path.join(sw, 'ln'))
    src_dir = os.path.join(work, 'csrc')
    bad_zip = os.path.join(work, 'bad.zip')
    with open(bad_zip, 'wb') as f:
        f.write(b'PK\x03\x04junk')
    errs = [
        [],
        ['-c', sw, os.path.join(work, 'e.cpio')],
        ['-c', os.path.join(sw, 'ln'), os.path.join(work, 'e2.cpio')],
        ['-c', os.path.join(sw, 'ln'), os.path.join(work, 'e3.zip'), '-k'],
        ['-z', '-c', os.path.join(sw, 'ln'), os.path.join(work, 'e4.cpio.gz')],
        ['-c', os.path.join(work, 'missing-src'), os.path.join(work, 'e5.cpio')],
        ['-c', src_dir, src_dir, os.path.join(work, 'e6.cpio')],
        ['-x', os.path.join(work, 'missing-arch'), os.path.join(work, 'x0') + '/'],
        ['-x', bad_zip, os.path.join(work, 'x1') + '/'],
        ['-x', '-k', m1a, os.path.join(work, 'x2') + '/'],
        ['--arch', 'bogus', src_dir, os.path.join(work, 'x3')],
        ['-k', '-c'],  # -k without -c/-x context; expected usage error
        ['-x', src_dir],  # extraction with a dir as archive
        ['-V', '-c'],
    ]
    for idx, eargs in enumerate(errs):
        rr, rso, ser = run([ORACLE], eargs, work)
        sr, sso, ses = run(subj, eargs, work)
        ok = rr == sr and rso == sso and ser == ses
        if not ok:
            failures += 1
            report('err-%02d' % idx, ' '.join(eargs)[:32], 'FAIL')
        else:
            report('err-%02d' % idx, ' '.join(eargs)[:32], 'OK')

    print('%d fixtures, %d failure(s)' % (len(fixtures), failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())