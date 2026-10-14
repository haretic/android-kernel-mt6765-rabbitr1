#!/usr/bin/env python3
"""trim-bootanimation.py <lineage bootanimation.zip> <out.zip>

End Lineage's outro on its finished logo. The stock part2 holds the logo and
then fades it to black, which only lengthens boot and blanks the screen before
the UI appears; the last frame drawn stays on screen until SystemUI replaces it.
"""
import sys
import zipfile

LAST_OUTRO_FRAME = 280  # teal logo fully drawn; later frames hold, then fade out

src, dst = sys.argv[1], sys.argv[2]
kept = dropped = 0
with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, 'w', zipfile.ZIP_STORED) as zout:
    for info in zin.infolist():
        part, _, name = info.filename.partition('/')
        if part == 'part2' and name.endswith('.png') and int(name[:-4]) > LAST_OUTRO_FRAME:
            dropped += 1
            continue
        # bootanimation needs stored (uncompressed) entries.
        out = zipfile.ZipInfo(info.filename, date_time=(2008, 1, 1, 0, 0, 0))
        out.compress_type = zipfile.ZIP_STORED
        zout.writestr(out, zin.read(info))
        kept += 1
assert dropped, 'no outro frames past the logo; did the stock animation change?'
print(f'kept {kept} entries, dropped {dropped} outro frames')
