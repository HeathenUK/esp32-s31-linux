import subprocess, os, sys, glob
RE = sys.argv[1]
T  = sys.argv[2]
roots = sys.argv[3:]

libdirs = [os.path.join(T, d) for d in ("lib", "usr/lib")]

def find_lib(name):
    for d in libdirs:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return os.path.realpath(p)
    hits = glob.glob(os.path.join(T, "usr/lib/**", name), recursive=True)
    return os.path.realpath(hits[0]) if hits else None

def needed(path):
    out = subprocess.run([RE, "-dW", path], capture_output=True, text=True).stdout
    return [l.split("[")[1].split("]")[0] for l in out.splitlines() if "(NEEDED)" in l]

def segs(path):
    out = subprocess.run([RE, "-lW", path], capture_output=True, text=True).stdout
    text = data = 0
    for line in out.splitlines():
        s = line.split()
        if len(s) >= 8 and s[0] == "LOAD":
            try: fs = int(s[4], 16)
            except ValueError: continue
            if "E" in "".join(s[6:8]): text += fs
            else: data += fs
    return text, data

closure, queue = {}, []
for r in roots:
    p = os.path.join(T, r)
    if os.path.exists(p): queue.append(os.path.realpath(p))
    else: print("missing root:", r)
while queue:
    p = queue.pop()
    if p in closure: continue
    t, d = segs(p)
    closure[p] = (t, d)
    for n in needed(p):
        lp = find_lib(n)
        if lp and lp not in closure: queue.append(lp)
        elif not lp: print("  unresolved:", n)

rows = sorted(((t, d, os.path.basename(p)) for p, (t, d) in closure.items()), reverse=True)
tt = sum(r[0] for r in rows); td = sum(r[1] for r in rows)
print("%-32s %10s %10s" % ("object", "text", "data"))
for t, d, n in rows[:20]:
    print("%-32s %10d %10d" % (n, t, d))
print("-" * 54)
print("%d objects   text %.2f MB   data %.2f MB" % (len(rows), tt/1048576, td/1048576))
