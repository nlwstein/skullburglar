# /// script
# requires-python = ">=3.11"
# dependencies = ["capstone>=5"]
# ///
"""Find code addresses referenced from data that codegen didn't register as functions.

Scans every 4-byte-aligned big-endian word *outside* the code range of the dumped image;
words pointing into code at a non-function-start are candidates. A candidate is "likely"
(a real function entry) only when all of these hold:

- the preceding instruction ends a function (blr / unconditional b / bctr) or is padding;
- no relative branch anywhere in code targets it (a branch target is a local label —
  typically an MSVC try/catch continuation that the EH tables also point at);
- its first instructions don't read a non-volatile GPR (r14–r31) before writing it —
  continuation code relies on an established frame (`addi r1,r31,N`, `addi r27,r27,-1`).

Also scans code for `lis/addi`-built pointers that escape (callback args, stores) — see below.
Rejected candidates are printed with the reason. Entries already in
config/data_funcs.toml are re-evaluated (not treated as known starts).

    uv run tools/find_data_func_ptrs.py > logs/data_func_ptrs.tsv
    awk -F'\t' '$2=="likely"{printf "0x%s = {}  # from %s\n",$1,$4}' logs/data_func_ptrs.tsv  # body of data_funcs.toml
"""
import bisect
import json
import re
import struct
import sys

from capstone import CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN, Cs

BASE = 0x82000000
CODE_BEGIN, CODE_END = 0x82310000, 0x83754C14  # from runtime log "Function table initialized"
img = open("dumps/default_image.bin", "rb").read()
ours = {int(m, 16) for m in re.findall(r"^0x([0-9A-Fa-f]+)", open("config/data_funcs.toml").read(), re.M)}
starts = {int(k, 16) for k in json.load(open("generated/default/codegen.partition.json"))["assignments"]} - ours

md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
md.detail = True


def word(a: int) -> int:
    return struct.unpack_from(">I", img, a - BASE)[0]


def ends_function(w: int) -> bool:
    if w in (0x4E800020, 0x4E800420, 0, 0x60000000):  # blr, bctr, pad, nop
        return True
    return (w >> 26) == 18 and (w & 3) == 0  # unconditional b (not bl/ba)


def reads_nonvolatile(a: int, n: int = 6) -> bool:
    written: set[str] = set()
    # Capstone's PPC backend has no regs_access(); approximate from operand position:
    # stores/compares/traps read every register operand, everything else writes the first.
    for ins in md.disasm(img[a - BASE : a - BASE + 4 * n], a):
        regs = re.findall(r"\br\d+\b", ins.op_str)
        all_read = ins.mnemonic.startswith(("st", "cmp", "tw", "dcb", "mt"))
        writes, reads = ([], regs) if all_read or not regs else (regs[:1], regs[1:])
        if ins.mnemonic.startswith(("lwzu", "lbzu", "lhzu", "ldu", "stwu", "stdu")):
            reads = regs  # update forms also read the base
        for name in reads:
            if re.fullmatch(r"r(1[4-9]|2\d|3[01])", name) and name not in written:
                return True
        written |= set(writes)
        if ins.mnemonic in ("blr", "b", "bctr", "bl"):
            break
    return False


sorted_starts = sorted(starts)


def func_of(a: int) -> int:
    return sorted_starts[bisect.bisect_right(sorted_starts, a) - 1]


n = len(img) // 4
words = struct.unpack(f">{n}I", img[: n * 4])

# Every relative branch target in code (b/bl/bc), i.e. labels and call targets.
branch_targets: set[int] = set()
for i in range((CODE_BEGIN - BASE) // 4, (CODE_END - BASE) // 4):
    w, site = words[i], BASE + i * 4
    op = w >> 26
    if op == 18 and not w & 2 and not w & 1:  # b (bl targets are function starts anyway)
        off = w & 0x03FFFFFC
        off -= 0x04000000 if off & 0x02000000 else 0
    elif op == 16 and not w & 2:  # bc
        off = w & 0xFFFC
        off -= 0x10000 if off & 0x8000 else 0
    else:
        continue
    branch_targets.add((site + off) & 0xFFFFFFFF)

refs: dict[int, list[int]] = {}
for i, w in enumerate(words):
    a = BASE + i * 4
    if CODE_BEGIN <= a < CODE_END:
        continue
    if CODE_BEGIN <= w < CODE_END and w % 4 == 0 and w not in starts:
        refs.setdefault(w, []).append(a)

# Code-built pointers: `lis rA,hi` + `addi/ori rD,rA,lo` yielding a code address. Kept only
# when rD then *escapes* — passed to a `bl` (callback arg, e.g. a qsort comparator), stored
# (`stw rD`), or moved to ctr — before any return/branch. MSVC catch funclets also build code
# addresses (`lis r3; addi r3; ... blr/b` = "resume here"); those return first and are dropped.


def escapes(j: int, rd: int) -> bool:
    for k in range(j + 1, min(j + 10, c1)):
        v = words[k]
        op = v >> 26
        if op == 18 and v & 1:  # bl
            return 3 <= rd <= 10
        if op == 36 and (v >> 21) & 31 == rd:  # stw rD
            return True
        if v == (0x7C0903A6 | rd << 21):  # mtctr rD
            return True
        if op in (16, 18) or v in (0x4E800020, 0x4E800420):  # any other branch / blr / bctr
            return False
        if (v >> 21) & 31 == rd and op not in (36, 38, 44, 52, 54, 11, 10):  # rD overwritten
            return False
    return False


c0, c1 = (CODE_BEGIN - BASE) // 4, (CODE_END - BASE) // 4
for i in range(c0, c1):
    w = words[i]
    if w >> 26 != 15 or (w >> 16) & 31:  # lis rA,hi
        continue
    ra, hi = (w >> 21) & 31, w & 0xFFFF
    for j in range(i + 1, min(i + 12, c1)):
        v = words[j]
        op, rd, rs, lo = v >> 26, (v >> 21) & 31, (v >> 16) & 31, v & 0xFFFF
        if op == 14 and rs == ra:  # addi
            t = ((hi << 16) + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF
        elif op == 24 and rd == ra:  # ori
            t = (hi << 16) | lo
        else:
            if rd == ra and op not in (36, 38, 44, 52, 54) or op == 18:  # clobbered / branch
                break
            continue
        site = BASE + j * 4
        # Skip intra-function targets: computed-jump tables (`lis/addi; add; mtctr; bctr`).
        if (CODE_BEGIN <= t < CODE_END and t % 4 == 0 and t not in starts
                and func_of(t) != func_of(site) and escapes(j, rd)):
            refs.setdefault(t, []).append(site)
        break

likely = 0
for t in sorted(refs):
    if word(t) >> 24 == 0x01 and word(t + 8) == 0x7D6903A6:  # XEX import thunk: [rec][rec] mtctr r11; bctr
        kind = "import"
    elif not ends_function(word(t - 4)):
        kind = "interior"
    elif t in branch_targets:
        kind = "label"
    elif reads_nonvolatile(t):
        kind = "frame"
    else:
        kind = "likely"
        likely += 1
    print(f"{t:08X}\t{kind}\t{len(refs[t])}\t{' '.join(f'{r:08X}' for r in refs[t][:4])}")
print(f"# {len(refs)} unregistered targets, {likely} look like function entries", file=sys.stderr)
