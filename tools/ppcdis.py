# /// script
# requires-python = ">=3.11"
# dependencies = ["capstone>=5"]
# ///
"""Disassemble the dumped XEX image (flat, based at 0x82000000).

    uv run tools/ppcdis.py 0x82360A48 [count]          # disassemble
    uv run tools/ppcdis.py --refs 0x82360A48           # find 32-bit BE pointers to address
"""
import struct
import sys

from capstone import CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN, Cs

BASE = 0x82000000
IMG = "dumps/default_image.bin"


def main() -> None:
    data = open(IMG, "rb").read()
    if sys.argv[1] == "--refs":
        target = int(sys.argv[2], 16)
        needle = struct.pack(">I", target)
        i = data.find(needle)
        while i != -1:
            print(f"{BASE + i:08X}")
            i = data.find(needle, i + 1)
        return
    addr = int(sys.argv[1], 16)
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 24
    md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
    off = addr - BASE
    for ins in md.disasm(data[off : off + count * 4], addr):
        print(f"{ins.address:08X}  {ins.bytes.hex()}  {ins.mnemonic} {ins.op_str}")


if __name__ == "__main__":
    main()
