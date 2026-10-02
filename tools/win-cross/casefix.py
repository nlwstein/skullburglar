# Case-insensitive include fix-up: for every #include in the sources that only matches an xwin
# header case-insensitively, add a symlink with the spelling used.
import os, re, sys
roots = sys.argv[1:]
hdr_dirs = ["/w/xwin/crt/include"] + [os.path.join("/w/xwin/sdk/include", d) for d in os.listdir("/w/xwin/sdk/include")]
index = {}
for d in hdr_dirs:
    for dp, _, fs in os.walk(d):
        for f in fs:
            rel = os.path.relpath(os.path.join(dp, f), d)
            index.setdefault((d, rel.lower()), rel)
wanted = set()
for r in roots:
    for dp, _, fs in os.walk(r):
        for f in fs:
            if f.endswith((".h", ".hpp", ".c", ".cc", ".cpp", ".inl", ".hxx")):
                try: t = open(os.path.join(dp, f), errors="ignore").read()
                except OSError: continue
                wanted |= set(re.findall(r"#\s*include\s*[<\"]([^>\"]+)[>\"]", t))
n = 0
for w in wanted:
    for d in hdr_dirs:
        real = index.get((d, w.lower()))
        if real and real != w and not os.path.lexists(os.path.join(d, w)):
            dst = os.path.join(d, w)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            os.symlink(os.path.join(d, real), dst); n += 1
print("symlinks added:", n)
