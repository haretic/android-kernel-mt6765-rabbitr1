#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Convert the stock aw87xxx_acf.bin to the mainline aw87390_acf.bin.

Both use the Awinic ACF container (hdr_version 1). The stock vendor driver
numbers its "Off" profile 11, past the mainline profile table (Music..Receiver,
0..10); mainline rejects the whole file for it. Mainline powers the amplifier
down by itself, so that entry is dropped. Section data and CRCs are unchanged.

usage: aw87xxx-acf-to-aw87390.py <stock aw87xxx_acf.bin> <out aw87390_acf.bin>
"""
import struct
import sys

ACF_FILE_ID = 0x0A15F908
HDR = struct.Struct("<I24s16s4sIIIII12s")      # struct aw_cfg_hdr
DDE = struct.Struct("<I16sHHHHIIII20s")        # struct aw_cfg_dde
PROFILE_MAX = 11                               # AW88395_PROFILE_MAX


def crc8_lsb(data, poly=0x8C):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ poly if crc & 1 else crc >> 1
    return crc


def main(src, dst):
    blob = open(src, "rb").read()
    (fid, proj, cust, ver, author, ddt_size, ddt_num, hdr_off, hdr_ver,
     rsv) = HDR.unpack_from(blob, 0)
    if fid != ACF_FILE_ID or hdr_ver != 1:
        sys.exit(f"{src}: not an ACF v1 file (id {fid:#x}, version {hdr_ver})")
    if ddt_size != ddt_num * DDE.size:
        sys.exit(f"{src}: unexpected DDT size {ddt_size}")

    keep = []
    for i in range(ddt_num):
        dde = list(DDE.unpack_from(blob, hdr_off + i * DDE.size))
        name = dde[1].rstrip(b"\0").decode()
        profile, size, offset, crc = dde[5], dde[7], dde[8], dde[9]
        data = blob[offset:offset + size]
        if crc8_lsb(data) != crc:
            sys.exit(f"{src}: section {i} CRC mismatch")
        verdict = "drop" if profile >= PROFILE_MAX else "keep"
        print(f"{verdict} {name} profile {profile} type {dde[6]} size {size}")
        if verdict == "keep":
            keep.append((dde, data))

    out_ddt = b""
    out_data = b""
    data_off = hdr_off + len(keep) * DDE.size
    for dde, data in keep:
        dde[8] = data_off + len(out_data)
        out_ddt += DDE.pack(*dde)
        out_data += data

    hdr = HDR.pack(fid, proj, cust, ver, author, len(keep) * DDE.size,
                   len(keep), hdr_off, hdr_ver, rsv)
    out = hdr + blob[HDR.size:hdr_off] + out_ddt + out_data
    open(dst, "wb").write(out)
    print(f"wrote {dst}: {len(out)} bytes, {len(keep)} sections")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
