"""List UClass exports (name : super) from a decompressed BotS package (see upk_decompress.py).

    python3 tools/upk_classes.py dumps/upk/C1Game.xxx
"""
import struct
import sys


def main(path: str) -> None:
    d = open(path, "rb").read()
    o = 12
    o += 4 + struct.unpack_from(">i", d, o)[0] + 4  # folder name, package flags
    nc, no, ec, eo, ic, io_, _ = struct.unpack_from(">7i", d, o)

    names, p = [], no
    for _ in range(nc):
        n = struct.unpack_from(">i", d, p)[0]
        names.append(d[p + 4 : p + 3 + n].decode("latin1"))
        p += 4 + n + 8

    imports = []
    for k in range(ic):
        imports.append(names[struct.unpack_from(">i", d, io_ + k * 28 + 20)[0]])

    exports, p = [], eo
    for _ in range(ec):
        cls, sup, _outer, on = struct.unpack_from(">iiii", d, p)
        p += 24 + 8 + 8  # + archetype, object flags, serial size/offset
        p += 4 + struct.unpack_from(">i", d, p)[0] * 12 + 4  # component map, export flags
        p += 4 + struct.unpack_from(">i", d, p)[0] * 4 + 16 + 4  # net counts, guid, pkg flags
        exports.append((cls, sup, names[on]))

    def ref(i: int) -> str:
        return exports[i - 1][2] if i > 0 else imports[-i - 1] if i < 0 else "None"

    for cls, sup, name in sorted(exports, key=lambda e: e[2].lower()):
        if cls == 0:
            print(f"{name} : {ref(sup)}")


if __name__ == "__main__":
    main(sys.argv[1])
