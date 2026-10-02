"""Find gotos to labels not defined in the same generated function (codegen boundary bugs).

    python3 tools/check_labels.py generated/default
Prints: function, missing label, and the function that does define that label (if any).
"""
import re
import sys
from pathlib import Path

FUNC = re.compile(r"^DEFINE_REX_FUNC\((\w+)\)")
GOTO = re.compile(r"goto (loc_[0-9A-F]+);")
LABEL = re.compile(r"^(loc_[0-9A-F]+):")


def main(root: str) -> int:
    owner, missing = {}, []
    for f in sorted(Path(root).glob("*_recomp.*.cpp")):
        fn, gotos, labels = None, set(), set()

        def flush():
            for lab in gotos - labels:
                missing.append((fn, lab, f.name))

        for line in f.open(errors="replace"):
            m = FUNC.match(line)
            if m:
                if fn:
                    flush()
                fn, gotos, labels = m.group(1), set(), set()
                continue
            if fn is None:
                continue
            lm = LABEL.match(line)
            if lm:
                labels.add(lm.group(1))
                owner[lm.group(1)] = fn
            gotos.update(GOTO.findall(line))
        if fn:
            flush()
    for fn, lab, fname in sorted(missing):
        print(f"{fn}\t{lab}\tdefined_in={owner.get(lab, '-')}\t{fname}")
    print(f"# {len(missing)} missing labels in {len({m[0] for m in missing})} functions", file=sys.stderr)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
