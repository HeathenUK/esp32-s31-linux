import subprocess, os, sys, glob
RE = sys.argv[1]
T  = sys.argv[2]
rows, seen = [], set()
files = []
for pat in ("usr/lib/**/*.so*", "lib/*.so*", "usr/bin/*", "bin/*", "usr/libexec/*"):
    files += glob.glob(os.path.join(T, pat), recursive=True)
for f in files:
    if os.path.islink(f) or not os.path.isfile(f):
        continue
    rp = os.path.realpath(f)
    if rp in seen:
        continue
    seen.add(rp)
    try:
        out = subprocess.run([RE, "-lW", f], capture_output=True, text=True, timeout=30).stdout
    except Exception:
        continue
    text = data = 0
    for line in out.splitlines():
        s = line.split()
        if len(s) >= 8 and s[0] == "LOAD":
            try:
                filesz = int(s[4], 16)
            except ValueError:
                continue
            flags = "".join(s[6:8]) if len(s) > 7 else ""
            if "E" in flags:
                text += filesz
            else:
                data += filesz
    if text or data:
        rows.append((text, data, os.path.basename(f)))
rows.sort(reverse=True)
tt = sum(r[0] for r in rows); td = sum(r[1] for r in rows)
print("%-32s %10s %10s" % ("object", "text(RE)", "data(RW)"))
for t, d, n in rows[:16]:
    print("%-32s %10d %10d" % (n, t, d))
print("-" * 54)
print("%d objects   text %.2f MB   data %.2f MB" % (len(rows), tt/1048576, td/1048576))
