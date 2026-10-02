#!/usr/bin/env python3
"""Make dumps/upk/*.xxx (decompressed by upk_decompress.py) readable by UELib: the on-disk
summary still says CompressionFlags=2 with a chunk table that overlaps the name table, so rewrite
the summary tail as the uncompressed form (flags 0, no chunks, PackageSource 0, no
AdditionalPackagesToCook) — it then ends exactly at NameOffset. Output: dumps/upk-plain/.
Then: cd tools/uedecomp && dotnet run -c Release -- ../../dumps/upk-plain/C1Game.xxx ../../dumps/script/C1Game
"""
import glob
import os
import struct

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
os.makedirs("dumps/upk-plain", exist_ok=True)
for f in sorted(glob.glob("dumps/upk/*.xxx")):
    d = bytearray(open(f, "rb").read())
    o = 12
    n = struct.unpack(">i", d[o:o + 4])[0]
    o += 4 + n + 4                                  # FolderName, PackageFlags
    name_off = struct.unpack(">I", d[o + 4:o + 8])[0]
    o += 28 + 16                                    # counts/offsets, GUID
    gc = struct.unpack(">I", d[o:o + 4])[0]
    o += 4 + 12 * gc + 8                            # generations, engine + cooker version
    assert o + 16 == name_off, (f, o, name_off)
    d[o:o + 16] = bytes(16)
    open(os.path.join("dumps/upk-plain", os.path.basename(f)), "wb").write(d)
    print(f, "-> dumps/upk-plain")
