"""Minimal 32-bit minidump reader for crash triage without a debugger.

Usage: python -I tools/minidump.py <file.dmp> [--stack N]

Prints the exception (code, address, faulting module+offset), the faulting thread's
registers, and a heuristic call stack: every dword on the faulting thread's stack that
points into a loaded module's code, mapped to module+offset (static VA for GTAIV.exe so
it can be fed to dumpbin /disasm). Heuristic = may include stale return addresses.
"""
import struct
import sys

STREAM_THREAD_LIST = 3
STREAM_MODULE_LIST = 4
STREAM_EXCEPTION = 6
STREAM_MEMORY_LIST = 5
STREAM_MEMORY64_LIST = 9


def read_str(data, rva):
    n = struct.unpack_from("<I", data, rva)[0]
    return data[rva + 4 : rva + 4 + n].decode("utf-16-le", errors="replace")


def main():
    path = sys.argv[1]
    max_stack = int(sys.argv[sys.argv.index("--stack") + 1]) if "--stack" in sys.argv else 40
    d = open(path, "rb").read()
    sig, ver, nstreams, dir_rva = struct.unpack_from("<IIII", d, 0)
    assert sig == 0x504D444D, "not a minidump"
    streams = {}
    for i in range(nstreams):
        st, size, rva = struct.unpack_from("<III", d, dir_rva + 12 * i)
        streams[st] = (rva, size)

    # Modules
    mods = []
    rva, _ = streams[STREAM_MODULE_LIST]
    n = struct.unpack_from("<I", d, rva)[0]
    for i in range(n):
        off = rva + 4 + 108 * i
        base, size = struct.unpack_from("<QI", d, off)
        name_rva = struct.unpack_from("<I", d, off + 20)[0]
        mods.append((base, size, read_str(d, name_rva).split("\\")[-1]))

    def where(addr):
        for base, size, name in mods:
            if base <= addr < base + size:
                if name.lower() == "gtaiv.exe":
                    return f"{name}+0x{addr - base:X} (static VA 0x{addr - base + 0x400000:08X})"
                return f"{name}+0x{addr - base:X}"
        return None

    # Memory ranges (for the stack)
    ranges = []
    if STREAM_MEMORY_LIST in streams:
        rva, _ = streams[STREAM_MEMORY_LIST]
        n = struct.unpack_from("<I", d, rva)[0]
        for i in range(n):
            start, dsize, drva = struct.unpack_from("<QII", d, rva + 4 + 16 * i)
            ranges.append((start, dsize, drva))
    if STREAM_MEMORY64_LIST in streams:
        rva, _ = streams[STREAM_MEMORY64_LIST]
        n, base_rva = struct.unpack_from("<QQ", d, rva)
        cur = base_rva
        for i in range(n):
            start, dsize = struct.unpack_from("<QQ", d, rva + 16 + 16 * i)
            ranges.append((start, dsize, cur))
            cur += dsize

    def read_mem(addr, size):
        for start, dsize, drva in ranges:
            if start <= addr and addr + size <= start + dsize:
                o = drva + addr - start
                return d[o : o + size]
        return None

    # Exception
    rva, _ = streams[STREAM_EXCEPTION]
    tid = struct.unpack_from("<I", d, rva)[0]
    code, flags = struct.unpack_from("<II", d, rva + 8)
    addr = struct.unpack_from("<Q", d, rva + 8 + 16)[0]
    nparams = struct.unpack_from("<I", d, rva + 8 + 24)[0]
    params = struct.unpack_from("<15Q", d, rva + 8 + 32)
    ctx_size, ctx_rva = struct.unpack_from("<II", d, rva + 8 + 152)
    print(f"exception 0x{code:08X} at 0x{addr:08X} in {where(addr) or '??'}  thread {tid}")
    if code == 0xC0000005 and nparams >= 2:
        kind = {0: "read", 1: "write", 8: "execute"}.get(params[0], str(params[0]))
        print(f"  access violation: {kind} of 0x{params[1]:08X}")

    # x86 CONTEXT: Edi..Eax at 0x9C, Ebp 0xB4, Eip 0xB8, Esp 0xC4
    names = ["edi", "esi", "ebx", "edx", "ecx", "eax", "ebp", "eip"]
    regs = dict(zip(names, struct.unpack_from("<8I", d, ctx_rva + 0x9C)))
    regs["esp"] = struct.unpack_from("<I", d, ctx_rva + 0xC4)[0]
    print("  " + "  ".join(f"{k}={v:08X}" for k, v in regs.items()))

    # Heuristic stack walk
    print(f"stack from esp=0x{regs['esp']:08X} (code pointers only):")
    shown = 0
    for i in range(4096):
        a = regs["esp"] + 4 * i
        w = read_mem(a, 4)
        if w is None:
            break
        v = struct.unpack("<I", w)[0]
        loc = where(v)
        if loc and not loc.lower().startswith(("kernelbase", "ntdll")) or (loc and shown < 3):
            print(f"  [esp+0x{4*i:04X}] 0x{v:08X}  {loc}")
            shown += 1
            if shown >= max_stack:
                break


if __name__ == "__main__":
    main()
