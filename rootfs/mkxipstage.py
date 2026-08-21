"""Stage the XIP-able part of a binary's dependency closure.

libc is deliberately excluded: the ELF interpreter path is baked into every
binary, so the loader itself always comes from the original rootfs and putting
a second copy in flash would not be used.
"""
import subprocess, os, sys, glob, shutil
RE, T, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
roots = sys.argv[4:]
libdirs = [os.path.join(T, d) for d in ("lib", "usr/lib")]

def find_lib(name):
    for d in libdirs:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return os.path.realpath(p)
    h = glob.glob(os.path.join(T, "usr/lib/**", name), recursive=True)
    return os.path.realpath(h[0]) if h else None

def needed(p):
    o = subprocess.run([RE, "-dW", p], capture_output=True, text=True).stdout
    return [l.split("[")[1].split("]")[0] for l in o.splitlines() if "(NEEDED)" in l]

closure, queue = set(), []
for r in roots:
    p = os.path.join(T, r)
    if os.path.exists(p):
        queue.append(os.path.realpath(p))
while queue:
    p = queue.pop()
    if p in closure:
        continue
    closure.add(p)
    for n in needed(p):
        lp = find_lib(n)
        if lp and lp not in closure:
            queue.append(lp)

libdir = os.path.join(OUT, "usr/lib")
os.makedirs(libdir, exist_ok=True)
staged = 0
for p in sorted(closure):
    b = os.path.basename(p)
    if b.startswith("libc.so") or "ld-musl" in b:
        print("  skip (interpreter):", b)
        continue
    shutil.copy2(p, os.path.join(libdir, b))
    staged += os.path.getsize(p)
    # recreate the SONAME symlinks the loader looks for
    parts = b.split(".so")
    if len(parts) == 2 and parts[1]:
        vers = parts[1].lstrip(".").split(".")
        base = parts[0] + ".so"
        for i in range(len(vers)):
            link = base + "." + ".".join(vers[:i + 1])
            lp = os.path.join(libdir, os.path.basename(link))
            if os.path.basename(link) != b and not os.path.exists(lp):
                os.symlink(b, lp)
        if not os.path.exists(os.path.join(libdir, os.path.basename(base))):
            os.symlink(b, os.path.join(libdir, os.path.basename(base)))
print("staged %d objects, %.2f MB" % (len(os.listdir(libdir)), staged / 1048576))
