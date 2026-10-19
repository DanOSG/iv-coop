"""Find IDA-style byte patterns in a PE file and print their virtual addresses.

Usage: python -I tools/find_pattern.py <exe> "8B 35 ? ? ? ? 85 F6" ["..."]

Used to locate engine functions/globals in GTAIV.exe for static analysis (pair with
`dumpbin /disasm /range:<va>,<va+N>`). Runtime code uses its own scanner in src/game/pattern.*.
"""
import re
import struct
import sys


def load_sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    image_base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    secs = []
    off = pe + 24 + opt_size
    for i in range(nsec):
        name = data[off:off + 8].rstrip(b"\0").decode(errors="replace")
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, off + 8)
        secs.append((name, va, vsize, raw, rsize))
        off += 40
    return image_base, secs


def to_regex(pat):
    out = b""
    for tok in pat.split():
        out += b"." if tok in ("?", "??") else re.escape(bytes([int(tok, 16)]))
    return re.compile(out, re.DOTALL)


def main():
    path, pats = sys.argv[1], sys.argv[2:]
    data = open(path, "rb").read()
    base, secs = load_sections(data)
    for pat in pats:
        rx = to_regex(pat)
        hits = []
        for name, va, vsize, raw, rsize in secs:
            for m in rx.finditer(data, raw, raw + rsize):
                hits.append((name, base + va + (m.start() - raw)))
        print(f"{pat}  ->  {len(hits)} hit(s)")
        for name, addr in hits[:10]:
            print(f"    {name} 0x{addr:08X}")


if __name__ == "__main__":
    main()
