# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026, LibreDarwin

#!/usr/bin/env python3
"""Clean-room dir-mode BOM writer prototype.

Replicates Apple /usr/bin/mkbom's dir-mode output for a directory tree:
fixed blocks 1-10, (PathRecord, File, PathInfoIndex) triplets, hard-link
group trailers, block index, vars index. Physical offsets differ from mkbom
(contiguous in index order, no free list, pool 2730); block indices,
contents and lengths match, so /usr/bin/lsbom output is byte-identical.

Usage: mkbom_proto.py DIR OUT.bom
"""
import os
import stat
import struct
import sys

from crc import cksum

BE = '>'
u32 = lambda v: struct.pack(BE + 'I', v & 0xffffffff)
u16 = lambda v: struct.pack(BE + 'H', v)
POOL = 2730
MAGIC = b'BOMStore'
FIXED_START = 0x200


def ifmt(m):
    t = m & 0xf000
    return {0x8000: 'reg', 0x4000: 'dir', 0xa000: 'lnk'}.get(t, 'special')


class Node:
    __slots__ = ('pid', 'parent_pid', 'name', 'st', 'typ', 'path')

    def __init__(self, pid, parent_pid, name, st, path):
        self.pid = pid
        self.parent_pid = parent_pid
        self.name = name
        self.st = st
        self.typ = ifmt(st.st_mode)
        self.path = path


def scan_dir(root):
    st = os.lstat(root)
    if not stat.S_ISDIR(st.st_mode):
        return None
    nodes = [Node(1, 0, '.', st, root)]

    def walk(path, parent_pid):
        st = os.lstat(path)
        pid = len(nodes) + 1
        nodes.append(Node(pid, parent_pid, os.path.basename(path), st, path))
        if st.st_mode & 0xf000 == stat.S_IFDIR:
            with os.scandir(path) as it:
                for e in it:
                    walk(e.path, pid)

    with os.scandir(root) as it:
        for e in it:
            walk(e.path, 1)
    return nodes


def relpath(nodes, pid):
    parts = []
    cur = pid
    while cur > 1:
        n = nodes[cur - 1]
        parts.append(n.name)
        cur = n.parent_pid
    parts.reverse()
    return './' + '/'.join(parts)


def build_pr(n):
    st = n.st
    if n.typ == 'reg':
        with open(n.path, 'rb') as f:
            data = f.read()
        ck = cksum(data)
        lnklen = 0
        tail = b'\x00' * 4      # total 35 bytes for regular files
    elif n.typ == 'lnk':
        target = os.readlink(n.path)
        ck = cksum(target.encode('utf-8'))
        lnklen = len(target) + 1
        tail = target.encode('utf-8') + b'\x00' + b'\x00' * 8
    else:  # dir
        ck = 0
        lnklen = 0
        tail = b''
    t = {'reg': 1, 'dir': 2, 'lnk': 3}[n.typ]
    base = (bytes([t]) + b'\x01' + u16(0x000f)
            + u16(st.st_mode & 0xffff)
            + u32(st.st_uid) + u32(st.st_gid)
            + u32(int(st.st_mtime))
            + u32(st.st_size)
            + b'\x01'
            + u32(ck)
            + u32(lnklen))
    return base + tail


def main(root, out):
    nodes = scan_dir(root)
    if nodes is None:
        sys.exit('root not a directory: %s' % root)

    # group detection by (dev, ino) over non-special non-dir nodes
    byino = {}
    for n in nodes:
        if n.typ in ('reg', 'lnk'):
            byino.setdefault((n.st.st_dev, n.st.st_ino), []).append(n)
    groups = {k: v for k, v in byino.items() if len(v) >= 2}
    members_list = {gid: v for gid, v in groups.items()}
    rank = {}
    for gid, members in groups.items():
        for k, m in enumerate(members):
            rank[m.pid] = k + 1
    pid_to_gid = {}
    for gid, members in groups.items():
        for m in members:
            pid_to_gid[m.pid] = gid

    # ---- simulation: assign block indices in pid (readdir DFS) order
    idx = 11
    pr_blk = {}     # pid -> PR block (first members hold shared PR)
    file_blk = {}   # pid -> File block
    pii_blk = {}    # pid -> PII block
    trailer = {}    # gid -> dict(tree, paths, prptr, treeptr)
    str_blk = {}    # pid -> string block (group members)
    empty_blk = {}  # pid -> empty block (group members)
    for n in nodes:
        pid, r = n.pid, rank.get(n.pid, 0)
        if n.typ == 'special':
            continue
        if r in (0, 1):
            pr_blk[pid], file_blk[pid], pii_blk[pid] = idx, idx + 1, idx + 2
            idx += 3
        elif r == 2:
            gid = pid_to_gid[pid]
            file_blk[pid], pii_blk[pid] = idx, idx + 1
            idx += 2
            trailer[gid] = dict(tree=idx, paths=idx + 1, prptr=idx + 2, treeptr=idx + 3)
            idx += 4
            for m in members_list[gid]:
                if rank[m.pid] <= 2:
                    str_blk[m.pid], empty_blk[m.pid] = idx, idx + 1
                    idx += 2
        else:  # r >= 3
            file_blk[pid], pii_blk[pid] = idx, idx + 1
            idx += 2
            str_blk[pid], empty_blk[pid] = idx, idx + 1
            idx += 2
    nob = idx - 1

    # ---- content emission (blocks dict: index -> bytes)
    blocks = {}

    def put(i, b):
        blocks[i] = b

    for n in nodes:
        pid = n.pid
        if n.typ == 'special':
            continue
        if pid in pr_blk:
            put(pr_blk[pid], build_pr(n))
        put(file_blk[pid], u32(n.parent_pid) + n.name.encode('utf-8') + b'\x00')
        put(pii_blk[pid], u32(pid) + u32(0))  # PR index patched below

    def pr_index(pid):
        if pid in pr_blk:
            return pr_blk[pid]
        gid = pid_to_gid[pid]
        return pr_blk[members_list[gid][0].pid]

    for n in nodes:
        if n.typ != 'special':
            put(pii_blk[n.pid], u32(n.pid) + u32(pr_index(n.pid)))

    for gid, members in members_list.items():
        t = trailer[gid]
        shared = pr_blk[members[0].pid]
        entries = sorted(((empty_blk[m.pid], str_blk[m.pid], relpath(nodes, m.pid))
                          for m in members),
                         key=lambda x: x[2].encode('utf-8'))
        paths = u16(1) + u16(len(members)) + u32(0) + u32(0)
        for eb, sb, _ in entries:
            paths += u32(eb) + u32(sb)
        put(t['tree'], b'tree' + u32(1) + u32(t['paths']) + u32(64)
            + u32(len(members)) + b'\x00')
        put(t['paths'], paths.ljust(64, b'\x00'))
        put(t['prptr'], u32(shared))
        put(t['treeptr'], u32(t['tree']))
        for m in members:
            if m.pid in str_blk:
                put(str_blk[m.pid], relpath(nodes, m.pid).encode('utf-8') + b'\x00')
                put(empty_blk[m.pid], b'')

    # ---- fixed blocks 1-10
    non_special = [n for n in nodes if n.typ != 'special']
    ninfo = 1 if any(n.typ != 'dir' for n in non_special) else 0
    c = 0
    seen = set()
    for n in non_special:
        if n.typ == 'dir':
            continue
        key = (n.st.st_dev, n.st.st_ino)
        if key in seen:
            continue
        seen.add(key)
        c += n.st.st_size
    put(1, u32(1) + u32(len(nodes) + 1) + u32(ninfo))
    if ninfo:
        put(1, blocks[1] + u32(0) + u32(0) + u32(c) + u32(0))

    main_entries = sorted(
        ((pii_blk[n.pid], file_blk[n.pid], n.parent_pid, n.name)
         for n in non_special),
        key=lambda x: (x[2], x[3].encode('utf-8')))
    main_paths = u16(1) + u16(len(non_special)) + u32(0) + u32(0)
    for pii, f, _, _ in main_entries:
        main_paths += u32(pii) + u32(f)
    put(2, b'tree' + u32(1) + u32(3) + u32(0x1000) + u32(len(non_special)) + b'\x00')
    put(3, main_paths.ljust(0x1000, b'\x00'))

    if groups:
        hl_paths = u16(1) + u16(len(groups)) + u32(0) + u32(0)
        for gid, members in sorted(members_list.items(), key=lambda kv: kv[1][0].pid):
            t = trailer[gid]
            hl_paths += u32(t['treeptr']) + u32(t['prptr'])
    else:
        hl_paths = u16(1) + u16(0) + u32(0) + u32(0)
    put(4, b'tree' + u32(1) + u32(5) + u32(0x1000) + u32(len(groups)) + b'\x00')
    put(5, hl_paths.ljust(0x1000, b'\x00'))

    put(6, u32(1) + u32(7) + u32(0) + b'\x00')
    put(7, b'tree' + u32(1) + u32(8) + u32(0x80) + u32(0) + b'\x00')
    put(8, (u16(1) + u16(0) + u32(0) + u32(0)).ljust(0x80, b'\x00'))
    put(9, b'tree' + u32(1) + u32(10) + u32(0x1000) + u32(0) + b'\x00')
    put(10, (u16(1) + u16(0) + u32(0) + u32(0)).ljust(0x1000, b'\x00'))

    # ---- assemble file: contiguous blocks in index order
    order = sorted(blocks)
    offsets = {}
    off = FIXED_START
    for i in order:
        offsets[i] = off
        off += len(blocks[i])
    for i in range(1, nob + 1):
        if i not in blocks:
            blocks[i] = b''
            offsets[i] = 0

    idxoff = off
    index = u32(POOL) + u32(0) + u32(0)  # pool count + null slot 0
    for i in range(1, nob + 1):
        index += u32(offsets[i]) + u32(len(blocks[i]))
    for _ in range(POOL - 1 - nob):
        index += u32(0) + u32(0)
    index += u32(0)  # free list count

    varoff = idxoff + len(index)
    varsb = u32(5)
    for name, blk in (('BomInfo', 1), ('Paths', 2), ('HLIndex', 4),
                      ('VIndex', 6), ('Size64', 9)):
        varsb += u32(blk) + bytes([len(name)]) + name.encode('utf-8')

    header = (MAGIC + u32(1) + u32(nob) + u32(idxoff) + u32(len(index))
              + u32(varoff) + u32(len(varsb)))

    with open(out, 'wb') as f:
        f.write(header)
        f.write(b'\x00' * (FIXED_START - len(header)))
        for i in order:
            f.write(blocks[i])
        f.write(index)
        f.write(varsb)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('usage: writer.py DIR OUT.bom')
    main(sys.argv[1], sys.argv[2])