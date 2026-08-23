#!/usr/bin/env python3
"""Turn a Buildroot build into an opkg feed.

Runs inside the build container. Buildroot does not produce packages - it
produces one monolithic root filesystem - but it does record which package
installed which file, for its legal-info and graphing support. That plus the
version in each build directory name is everything a .ipk needs:

    $(O)/build/packages-file-list.txt    package -> files, as "pkg,./path"
    $(O)/build/<pkg>-<version>/          package -> version

Dependencies come from "make show-info" when it is available; the feed is still
usable without them, just without automatic dependency resolution.

Usage: mkfeed.py <buildroot-output-dir> <feed-output-dir> [pkg ...]
"""
import json, os, re, shutil, subprocess, sys, tarfile

ARCH = 'esp32s31'


def read_file_list(o):
    """package -> [paths], from Buildroot's own record."""
    pkgs = {}
    path = os.path.join(o, 'build', 'packages-file-list.txt')
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line or ',' not in line:
                continue
            pkg, f = line.split(',', 1)
            pkgs.setdefault(pkg, []).append(f.lstrip('.').lstrip('/'))
    return pkgs


def read_versions(o):
    """package -> version, from the build directory names."""
    vers, bdir = {}, os.path.join(o, 'build')
    for d in os.listdir(bdir):
        if not os.path.isdir(os.path.join(bdir, d)):
            continue
        m = re.match(r'^(.+?)-([0-9][^-]*(?:-.*)?)$', d)
        if m:
            vers.setdefault(m.group(1), m.group(2))
    return vers


def read_deps(src):
    """package -> [deps], from make show-info. Optional."""
    try:
        out = subprocess.run(['make', '-C', src, 'show-info'],
                             capture_output=True, text=True, timeout=900)
        info = json.loads(out.stdout)
    except Exception as e:
        print('  (no dependency data: %s)' % e)
        return {}
    return {k: v.get('dependencies', []) for k, v in info.items()
            if isinstance(v, dict)}


def build_ipk(pkg, version, files, deps, target, stage_root, feed):
    stage = os.path.join(stage_root, pkg)
    shutil.rmtree(stage, ignore_errors=True)
    data = 0
    for rel in files:
        src = os.path.join(target, rel)
        if not os.path.exists(src) and not os.path.islink(src):
            continue                      # staging/host file, not on target
        dst = os.path.join(stage, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.islink(src):
            link = os.readlink(src)
            if not os.path.lexists(dst):
                os.symlink(link, dst)
        elif os.path.isdir(src):
            os.makedirs(dst, exist_ok=True)
        else:
            shutil.copy2(src, dst)
            data += os.path.getsize(src)
    if not data:
        return None                       # nothing lands on the target

    ctl = os.path.join(stage, 'CONTROL')
    os.makedirs(ctl, exist_ok=True)
    dep = [d for d in deps.get(pkg, []) if d in deps and not d.startswith('host-')]
    with open(os.path.join(ctl, 'control'), 'w') as fh:
        fh.write('Package: %s\n' % pkg)
        fh.write('Version: %s\n' % version)
        fh.write('Architecture: %s\n' % ARCH)
        fh.write('Maintainer: esp32-s31-linux\n')
        fh.write('Section: base\n')
        if dep:
            fh.write('Depends: %s\n' % ', '.join(sorted(set(dep))))
        fh.write('Description: %s, built from Buildroot for the ESP32-S31\n' % pkg)
    r = subprocess.run(['opkg-build', '-Z', 'gzip', '-o', 'root', '-g', 'root',
                        stage, feed], capture_output=True, text=True)
    return None if r.returncode else pkg


def main():
    o, feed = sys.argv[1], sys.argv[2]
    want = set(sys.argv[3:])
    target = os.path.join(o, 'target')
    os.makedirs(feed, exist_ok=True)
    stage_root = os.path.join(o, 'ipkstage')
    shutil.rmtree(stage_root, ignore_errors=True)

    files = read_file_list(o)
    vers = read_versions(o)
    print('%d packages recorded' % len(files))
    deps = read_deps('/src')

    made, skipped = [], []
    for pkg in sorted(files):
        if want and pkg not in want:
            continue
        v = vers.get(pkg, '0')
        r = build_ipk(pkg, v, files[pkg], deps, target, stage_root, feed)
        (made if r else skipped).append(pkg)

    print('built %d ipk, skipped %d (nothing on target)' % (len(made), len(skipped)))
    idx = subprocess.run('opkg-make-index %s > %s/Packages' % (feed, feed),
                         shell=True, capture_output=True, text=True)
    if idx.returncode:
        print('index failed:', idx.stderr[-300:]); return 1
    subprocess.run('gzip -kf %s/Packages' % feed, shell=True)
    print('index: %d bytes' % os.path.getsize(os.path.join(feed, 'Packages')))
    return 0


if __name__ == '__main__':
    sys.exit(main())
