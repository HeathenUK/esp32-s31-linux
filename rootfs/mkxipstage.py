"""Stage the XIP-able part of a binary's dependency closure, at its real paths.

Two rules matter here, and both were learned by getting them wrong:

  * **Keep the layout.** An earlier version flattened everything into
    /usr/lib/<basename>, so /usr/bin/weston landed at /usr/lib/weston and
    /usr/libexec/weston-desktop-shell at /usr/lib/weston-desktop-shell. The
    bytes were in flash but at paths nothing loads, so the overlay could not
    serve them and the compositor kept faulting its own text off the SD card.
    Objects are now staged at their path relative to the target root.

  * **DT_NEEDED is not the whole closure.** Weston dlopen()s its backend and
    shell by absolute path - libweston-15/drm-backend.so, weston/desktop-shell.so
    - so a walker that only follows NEEDED misses precisely the objects that run
    every frame. Roots may therefore be glob patterns, and whatever they match
    is treated as a root and walked for its own NEEDED.

libc is staged: /lib/ld-musl-riscv32-sf.so.1 is a symlink to ../usr/lib/libc.so,
so the interpreter follows the overlay even though its path is baked into every
binary.

Set EXCLUDE_DIR to a previously staged directory and anything already present
there is skipped. That is how the second flash image is built without carrying
a duplicate copy of libc, freetype and everything else the first one holds:
overlayfs merges both lower layers, so an object only has to exist in one.

    mkxipstage.py <readelf> <target-dir> <out-dir> <root> [root...]
"""
import glob
import os
import shutil
import subprocess
import sys

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
    matches = glob.glob(os.path.join(T, r.lstrip("/")), recursive=True)
    if not matches:
        print("  WARNING: root matched nothing:", r)
    for m in matches:
        if os.path.isfile(m):
            queue.append(os.path.realpath(m))

while queue:
    p = queue.pop()
    if p in closure:
        continue
    closure.add(p)
    for n in needed(p):
        lp = find_lib(n)
        if lp and lp not in closure:
            queue.append(lp)

exclude = os.environ.get("EXCLUDE_DIR", "")
# Objects to leave on the SD card even though they are in the closure.
#
# Being NEEDED by a root is not the same as being hot. libasound is 943,548
# bytes and lvdesk links it for the volume mixer and the odd PCM write - which
# happens when a human moves a slider, not per frame. Flash is the scarcest
# thing on this board, and that is 943 KB not being spent on bluetoothd, which
# never exits. The library still loads, from the SD lower layer of the same
# overlay; only its residency changes.
skip = set(f for f in os.environ.get("XIP_SKIP", "").split() if f)
staged = 0
count = 0
skipped = 0
for p in sorted(closure):
    rel = os.path.relpath(p, T)
    if exclude and os.path.exists(os.path.join(exclude, rel)):
        skipped += 1
        continue
    if rel in skip or os.path.basename(rel) in skip:
        skipped += 1
        continue
    if rel.startswith(".."):
        print("  skip (outside target):", p)
        continue
    dest = os.path.join(OUT, rel)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    shutil.copy2(p, dest)
    staged += os.path.getsize(p)
    count += 1

    # Recreate the SONAME symlinks the loader looks for, beside the object.
    b = os.path.basename(p)
    d = os.path.dirname(dest)
    parts = b.split(".so")
    if len(parts) == 2 and parts[1]:
        vers = parts[1].lstrip(".").split(".")
        base = parts[0] + ".so"
        for i in range(len(vers)):
            link = base + "." + ".".join(vers[:i + 1])
            lp = os.path.join(d, link)
            if link != b and not os.path.exists(lp):
                os.symlink(b, lp)
        if not os.path.exists(os.path.join(d, base)):
            os.symlink(b, os.path.join(d, base))

    # And any ALIAS the target already has for this object.
    #
    # The block above can only invent names derived from the object's own
    # basename, so libXaw.so.7 -> libXaw7.so.7 -> libXaw7.so.7.0.0 lost its
    # first hop: the file was staged under the name nothing asks for, and
    # xcalc - whose DT_NEEDED says libXaw.so.7 - would not start from XIP at
    # all. The alias is right there in the target directory; use it.
    srcdir = os.path.dirname(p)
    for name in os.listdir(srcdir):
        lp = os.path.join(srcdir, name)
        if not os.path.islink(lp):
            continue
        if os.path.realpath(lp) != os.path.realpath(p):
            continue
        dl = os.path.join(d, name)
        if not os.path.lexists(dl):
            os.symlink(b, dl)

print("staged %d objects, %.2f MB (skipped %d already in EXCLUDE_DIR)"
      % (count, staged / 1048576, skipped))
# Mark what was actually staged. This printed the whole CLOSURE unannotated,
# so an object held by the other image - libc, every time - appeared in both
# listings and read as a 690 KB duplicate that was never there.
for p in sorted(closure):
    rel = os.path.relpath(p, T)
    dup = exclude and os.path.exists(os.path.join(exclude, rel))
    off = rel in skip or os.path.basename(rel) in skip
    print("    %-6s %s" % ("(skip)" if dup else "(SD)" if off else "", rel))
