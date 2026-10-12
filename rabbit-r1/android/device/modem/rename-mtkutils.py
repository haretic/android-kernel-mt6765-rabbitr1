#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Give the stock libmtkutils.so (MediaTek's private copy of an Android 12
libutils) private symbol names, in it and in the blobs that link it.

Both libmtkutils and our libutils end up in mtkfusionrild's global symbol
lookup group, so whichever loads first interposes the other: our libbinder
then runs the old SharedBuffer code, or libmtk-ril runs the new Thread::run
that requires an existing strong reference. Renaming the clashing symbols
keeps each side on its own copy, as on stock where both copies matched.

Names are rewritten in place with names of the same length ("android" ->
"mtkandr" in the namespace, else the first three characters -> "mtk"), so no
string, symbol or relocation moves (the blobs use packed relocations that
patchelf --rename-dynamic-symbols can't renumber). The GNU hash table is
rebuilt with one bucket and a full bloom filter, which is valid for any
symbol order; a DT_HASH table is rebuilt as well.

usage: rename-mtkutils.py LIBUTILS_SO LIB_DIR
  LIBUTILS_SO  our vendor libutils.so, which defines the clashing names
  LIB_DIR      the blob lib64 directory, patched in place
"""

import os
import struct
import sys

DT_NULL, DT_HASH, DT_STRTAB, DT_SYMTAB, DT_STRSZ = 0, 4, 5, 6, 10
DT_NEEDED = 1
DT_GNU_HASH = 0x6FFFFEF5
PT_LOAD, PT_DYNAMIC = 1, 2
SHN_UNDEF = 0


class Elf:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            self.b = bytearray(f.read())
        b = self.b
        if b[:4] != b'\x7fELF' or b[4] != 2 or b[5] != 1:
            raise ValueError(f'{path}: not a little-endian ELF64 file')
        phoff, = struct.unpack_from('<Q', b, 0x20)
        phentsize, phnum = struct.unpack_from('<HH', b, 0x36)
        self.loads, dyn = [], None
        for i in range(phnum):
            p_type, _, p_offset, p_vaddr, _, p_filesz, _, _ = \
                struct.unpack_from('<IIQQQQQQ', b, phoff + i * phentsize)
            if p_type == PT_LOAD:
                self.loads.append((p_vaddr, p_offset, p_filesz))
            elif p_type == PT_DYNAMIC:
                dyn = (p_offset, p_filesz)
        self.dyn = {}
        self.needed = []
        for off in range(dyn[0], dyn[0] + dyn[1], 16):
            tag, val = struct.unpack_from('<qQ', b, off)
            if tag == DT_NULL:
                break
            if tag == DT_NEEDED:
                self.needed.append(val)
            else:
                self.dyn[tag] = val
        self.strtab = self.off(self.dyn[DT_STRTAB])
        self.symtab = self.off(self.dyn[DT_SYMTAB])
        self.needed = [self.cstr(self.strtab + v) for v in self.needed]
        self.nsyms = self.count_syms()

    def off(self, vaddr):
        for v, o, sz in self.loads:
            if v <= vaddr < v + sz:
                return vaddr - v + o
        raise ValueError(f'{self.path}: vaddr {vaddr:#x} not in a PT_LOAD')

    def cstr(self, off):
        return bytes(self.b[off:self.b.index(0, off)]).decode()

    def count_syms(self):
        if DT_HASH in self.dyn:
            return struct.unpack_from('<I', self.b, self.off(self.dyn[DT_HASH]) + 4)[0]
        h = self.off(self.dyn[DT_GNU_HASH])
        nb, symoff, bloom, _ = struct.unpack_from('<IIII', self.b, h)
        buckets = h + 16 + bloom * 8
        last = max(struct.unpack_from(f'<{nb}I', self.b, buckets))
        if last < symoff:
            return symoff
        chain = buckets + nb * 4
        while not struct.unpack_from('<I', self.b, chain + (last - symoff) * 4)[0] & 1:
            last += 1
        return last + 1

    def syms(self):
        """(index, name offset in the file, name, defined)"""
        for i in range(self.nsyms):
            st_name, _, _, shndx = struct.unpack_from('<IBBH', self.b, self.symtab + i * 24)
            yield i, self.strtab + st_name, self.cstr(self.strtab + st_name), shndx != SHN_UNDEF

    def rebuild_hashes(self):
        names = [n for _, _, n, _ in self.syms()]
        if DT_GNU_HASH in self.dyn:
            h = self.off(self.dyn[DT_GNU_HASH])
            _, symoff, bloom, shift = struct.unpack_from('<IIII', self.b, h)
            struct.pack_into('<IIII', self.b, h, 1, symoff, bloom, shift)
            struct.pack_into(f'<{bloom}Q', self.b, h + 16, *([(1 << 64) - 1] * bloom))
            bucket = h + 16 + bloom * 8
            struct.pack_into('<I', self.b, bucket, symoff if self.nsyms > symoff else 0)
            for i in range(symoff, self.nsyms):
                v = gnu_hash(names[i]) & ~1
                if i == self.nsyms - 1:
                    v |= 1
                struct.pack_into('<I', self.b, bucket + 4 + (i - symoff) * 4, v)
        if DT_HASH in self.dyn:
            h = self.off(self.dyn[DT_HASH])
            nb, nc = struct.unpack_from('<II', self.b, h)
            buckets, chains = [0] * nb, [0] * nc
            for i in range(1, nc):
                b = sysv_hash(names[i]) % nb
                chains[i], buckets[b] = buckets[b], i
            struct.pack_into(f'<{nb}I{nc}I', self.b, h + 8, *buckets, *chains)

    def write(self):
        with open(self.path, 'wb') as f:
            f.write(self.b)


def gnu_hash(name):
    h = 5381
    for c in name.encode():
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def sysv_hash(name):
    h = 0
    for c in name.encode():
        h = (h << 4) + c
        g = h & 0xF0000000
        if g:
            h ^= g >> 24
        h &= ~g
    return h


def private_name(name):
    if '7android' in name:
        return name.replace('7android', '7mtkandr')
    start = 0
    if name.startswith('_Z'):
        start = 2
        while name[start].isdigit():
            start += 1
    return name[:start] + 'mtk' + name[start + 3:]


def rename(elf, mapping):
    """Rename in place; return the names renamed."""
    targets = {(off, name) for _, off, name, _ in elf.syms() if name in mapping}
    for off, name in targets:
        new = mapping[name].encode()
        elf.b[off:off + len(new)] = new
    return {name for _, name in targets}


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    aosp = {n for _, _, n, d in Elf(sys.argv[1]).syms() if d}
    libdir = sys.argv[2]
    mtk = Elf(os.path.join(libdir, 'libmtkutils.so'))
    clash = sorted({n for _, _, n, d in mtk.syms() if d} & aosp)
    mapping = {n: private_name(n) for n in clash}
    every = {n for _, _, n, _ in mtk.syms()}
    if len(set(mapping.values())) != len(mapping) or set(mapping.values()) & (every | aosp):
        sys.exit('rename-mtkutils: private names collide')

    for lib in sorted(os.listdir(libdir)):
        path = os.path.join(libdir, lib)
        if not lib.endswith('.so') or os.path.islink(path):
            continue
        elf = Elf(path)
        if lib != 'libmtkutils.so' and 'libmtkutils.so' not in elf.needed:
            continue
        before = [n for _, _, n, _ in elf.syms()]
        renamed = rename(elf, mapping)
        after = [n for _, _, n, _ in elf.syms()]
        expect = [mapping.get(n, n) for n in before]
        if after != expect:
            sys.exit(f'rename-mtkutils: {lib}: a renamed string is shared with another symbol')
        if renamed:
            elf.rebuild_hashes()
            elf.write()
        print(f'{lib}: {len(renamed)} symbols renamed')


if __name__ == '__main__':
    main()
