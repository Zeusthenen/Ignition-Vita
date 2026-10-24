#!/usr/bin/env python3
"""check_vpk.py -- inspects an Ignition Vita VPK without installing it.

Usage: python3 scripts/check_vpk.py IgnitionVita_v1.1.vpk [--extract DIR]

Checks: the exact package layout, the LiveArea PNGs (dimensions, 8-bit indexed palette = PNG colour type 3,
not interlaced), template.xml references, and prints the size and SHA-256/SHA-1/MD5 of the package and
SHA-256 of every member. Exit code 0 = all checks passed.
"""
import hashlib, struct, sys, zipfile, os

REQUIRED = {
    "sce_sys/icon0.png": (128, 128),
    "sce_sys/livearea/contents/bg0.png": (840, 500),
    "sce_sys/livearea/contents/startup.png": (280, 158),
}
OTHER = ["eboot.bin", "sce_sys/param.sfo", "sce_sys/livearea/contents/template.xml"]


def digests(b):
    return hashlib.sha256(b).hexdigest(), hashlib.sha1(b).hexdigest(), hashlib.md5(b).hexdigest()


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[1]
    extract = argv[argv.index("--extract") + 1] if "--extract" in argv else None
    data = open(path, "rb").read()
    s256, s1, m5 = digests(data)
    print("package : %s" % os.path.basename(path))
    print("size    : %d bytes" % len(data))
    print("SHA-256 : %s\nSHA-1   : %s\nMD5     : %s\n" % (s256, s1, m5))
    z = zipfile.ZipFile(path)
    names = z.namelist()
    ok = True
    for n in list(REQUIRED) + OTHER:
        present = n in names
        ok &= present
        print("%-48s %s" % (n, "present" if present else "MISSING"))
    extra = [n for n in names if n not in REQUIRED and n not in OTHER]
    print("unexpected members: %s" % (extra or "none"))
    ok &= not extra
    print()
    for n, (w, h) in REQUIRED.items():
        if n not in names:
            continue
        d = z.read(n)
        sig = d[:8] == b"\x89PNG\r\n\x1a\n" and d[12:16] == b"IHDR"
        ww, hh, bd, ct, _, _, il = struct.unpack(">IIBBBBB", d[16:29])
        good = sig and (ww, hh) == (w, h) and bd == 8 and ct == 3 and il == 0
        ok &= good
        print("%-40s %dx%d bit_depth=%d colour_type=%d interlace=%d -> %s" % (n, ww, hh, bd, ct, il, "OK" if good else "FAILED (need %dx%d, 8-bit, colour type 3, no interlace)" % (w, h)))
    if "sce_sys/livearea/contents/template.xml" in names:
        t = z.read("sce_sys/livearea/contents/template.xml").decode("utf-8")
        refs = "<image>bg0.png</image>" in t and "<startup-image>startup.png</startup-image>" in t
        ok &= refs
        print("template.xml references bg0.png and startup.png (no paths): %s" % refs)
    print("\nmember SHA-256:")
    for n in names:
        print("  %-48s %s" % (n, hashlib.sha256(z.read(n)).hexdigest()))
    if extract:
        z.extractall(extract)
        print("\nextracted to %s" % extract)
    print("\nRESULT: %s" % ("OK" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
