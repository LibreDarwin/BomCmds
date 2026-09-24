# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026, LibreDarwin

import struct

def make_tab():
    tab=[0]*256
    for i in range(1,256):
        c=i<<24
        for _ in range(8):
            c=((c<<1)^0x04c11db7) if (c&0x80000000) else (c<<1)
        tab[i]=c&0xffffffff
    return tab
TAB=make_tab()

def cksum(data):
    crc=0
    for b in data:
        crc=(crc<<8)^TAB[((crc>>24)^b)&0xff]
        crc&=0xffffffff
    n=len(data)
    while n:
        crc=(crc<<8)^TAB[((crc>>24)^n)&0xff]
        n>>=8
    return (~crc)&0xffffffff

if __name__=='__main__':
    tests=[(b'hello',0xc3f5812d),(b'a.txt',1632437005),(b'#!/bin/sh\necho prog\n',0x9bbd446f),(b'readme.txt',0x64f28fdf)]
    for d,e in tests:
        print(d, hex(cksum(d)), 'expected', hex(e), 'OK' if cksum(d)==e else 'FAIL')
