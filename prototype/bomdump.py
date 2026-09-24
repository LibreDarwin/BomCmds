# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026, LibreDarwin

import struct, sys, os

BE = '>'

def be(b):
    return struct.unpack(BE + 'I', b)[0]

def parse(path):
    bom = open(path, 'rb').read()
    n = len(bom)
    def g(o):
        return struct.unpack(BE + 'I', bom[o:o+4])[0]
    magic = bom[0:8]
    version, nob = g(8), g(12)
    idxoff, idxlen = g(16), g(20)
    varoff, varlen = g(24), g(28)

    # vars
    o = varoff
    nvars = g(o); o += 4
    vars_blocks = {}
    for _ in range(nvars):
        bi, nl = struct.unpack(BE + 'IB', bom[o:o+5])
        name = bom[o+5:o+5+nl].decode('ascii')
        o += 5 + nl
        vars_blocks[name] = bi
    vars_bytes = o - varoff

    # block table
    pool = g(idxoff)
    t0 = idxoff + 4

    def ent(i):
        off, l = struct.unpack(BE + 'II', bom[t0+8*i:t0+8*i+8])
        return off, l, bom[off:off+l]

    blk = {i: ent(i) for i in range(1, nob+1)}

    # Paths tree
    pt = vars_blocks['Paths']
    off, l, tr = blk[pt]
    print("Path tree block:", pt, "len", l)
    print("  magic", tr[0:4], "version", be(tr[4:8]), "block_paths_index", be(tr[8:12]),
          "block_size", be(tr[12:16]), "path_count", be(tr[16:20]), "a", tr[20])
    bpi = be(tr[8:12])
    off, l, paths = blk[bpi]
    ip, count = struct.unpack(BE + 'HH', paths[0:4])
    nxt, prv = struct.unpack(BE + 'II', paths[4:12])
    print("Paths block:", bpi, "len", l, "is_path_info", ip, "count", count, "next", nxt, "prev", prv)

    rows = {}
    e = 12
    for _ in range(count):
        bi, fi = struct.unpack(BE + 'II', paths[e:e+8])
        e += 8
        # File record
        fo, fl, fname = blk[fi]
        parent = be(fname[0:4])
        name = fname[4:].rstrip(b'\x00').decode('utf-8', 'replace')
        # PathInfoIndex
        po, pl, pii = blk[bi]
        pid, pref = struct.unpack(BE + 'II', pii[:8])
        # PathRecord
        ro, rl, pr = blk[pref]
        pt_ = pr[0]
        arch = struct.unpack(BE + 'H', pr[2:4])[0]
        mode = struct.unpack(BE + 'H', pr[4:6])[0]
        uid, gid = struct.unpack(BE + 'II', pr[6:14])
        mtime = be(pr[14:18])
        size = be(pr[18:22])
        b_ = pr[22]
        ck = be(pr[23:27])
        llen = be(pr[27:31])
        link_name = pr[31:31+llen] if llen else b''
        rows[pid] = dict(name=name, parent=parent, record=pref, type=pt_, arch=arch,
                         mode=mode, uid=uid, gid=gid, mtime=mtime, size=size,
                         cksum=ck, link=link_name)

    names = {r['name']: pid for pid, r in rows.items()}

    def fullpath(pid):
        parts = []
        cur = pid
        seen = set()
        while cur in rows and cur not in seen:
            seen.add(cur)
            r = rows[cur]
            parts.append(r['name'])
            cur = r['parent']
        return '/'.join(parts)

    print("Blocks:")
    for pid in sorted(rows):
        r = rows[pid]
        tstr = {1: 'file', 2: 'dir', 3: 'symlink'}.get(r['type'], r['type'])
        print(f"  pid={pid} {fullpath(pid)!r} parent={r['parent']} rec={r['record']} "
              f"type={tstr} mode={r['mode']:o} uid={r['uid']} gid={r['gid']} "
              f"size={r['size']} cksum={r['cksum']:#010x} mtime={r['mtime']} link={r['link']!r}")
    return vars_bytes

if __name__ == '__main__':
    for p in sys.argv[1:]:
        print("=====", p)
        vars_bytes = parse(p)
        print("vars bytes:", vars_bytes)