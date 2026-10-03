# /// script
# requires-python = ">=3.11"
# dependencies = ["capstone>=5"]
# ///
"""List the engine's key FName globals: the function that builds them (default sub_8253BA50) loads each
key's name (UTF-16BE) into r4, makes an FName and stores it with `std` to an 8-byte global.

    uv run tools/key_names.py [function] [name-regex]   → <global address> <key name>
"""
import re
import struct
import sys

from capstone import CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN, Cs

BASE = 0x82000000
IMG = "dumps/default_image.bin"


def wide(data: bytes, addr: int) -> str:
    i = addr - BASE
    out = []
    while i + 1 < len(data):
        (c,) = struct.unpack_from(">H", data, i)
        if not c or len(out) > 64:
            break
        out.append(chr(c))
        i += 2
    return "".join(out)


def main() -> None:
    data = open(IMG, "rb").read()
    func = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x8253BA50
    rx = re.compile(sys.argv[2], re.I) if len(sys.argv) > 2 else None
    md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
    md.detail = True
    hi = {}
    name = made = None  # the name in r4; the name of the FName the last call made (stored after the next load)
    start = func - BASE
    for ins in md.disasm(data[start:start + 0x8000], func):
        ops = ins.op_str.replace(" ", "").split(",")
        if ins.mnemonic == "lis":
            hi[ops[0]] = (int(ops[1], 0) & 0xFFFF) << 16
        elif ins.mnemonic == "addi" and ops[0] == "r4" and ops[1] in hi:
            name = wide(data, (hi[ops[1]] + int(ops[2], 0)) & 0xFFFFFFFF)
        elif ins.mnemonic == "bl":
            made = name
        elif ins.mnemonic == "std" and made is not None:
            m = re.match(r"(-?0x[0-9a-f]+|-?\d+)\((r\d+)\)", ops[1])
            if m and m.group(2) in hi:
                addr = (hi[m.group(2)] + int(m.group(1), 0)) & 0xFFFFFFFF
                if not rx or rx.search(made):
                    print(f"{addr:08X} {made}")
                made = None
        elif ins.mnemonic in ("blr",):
            break


if __name__ == "__main__":
    main()
