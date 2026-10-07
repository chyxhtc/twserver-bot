#!/usr/bin/env python3
"""Read a WSL/ELF core dump and name the module frames on each thread's stack.

WSL keeps one crash dump per crash in %LOCALAPPDATA%\\Temp\\wsl-crashes\\ (the folder is
auto-cleaned, so only the most recent crash survives). The file is a plain ELF core, not a
Windows minidump, so the thread register sets are in NT_PRSTATUS and the mapped files (with
their load addresses) are in NT_FILE. That is enough to walk each thread's stack for return
addresses that land inside libTortoiseBots.so, then resolve them with addr2line against the
exact .so that was deployed at the time (keep those: modules/build-out/prev/libTortoiseBots-*.so).

Usage:
    python tools/wsl-core-dump-stack.py <core.dmp> <path/to/matching/libTortoiseBots.so>

Output is the candidate frames per thread, innermost first, already symbolized. If the crash
was an uncaught C++ exception the innermost frames belong to libstdc++/glibc; the module frame
right above them is the one that threw.
"""
import os
import struct
import subprocess
import sys

PT_LOAD = 1
PT_NOTE = 4
NT_PRSTATUS = 1
NT_FILE = 0x46494c45


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    core_path, module_path = sys.argv[1], sys.argv[2]
    # The local TDM-GCC addr2line cannot read the DWARF5 the build image emits
    # ("invalid abstract instance DIE ref", and every answer comes back as ??:?), so the
    # same toolchain that produced the .so has to do the lookup. Set ADDR2LINE_IMAGE to a
    # docker image (e.g. tortoise-build-env:latest) and the module file's folder is bind
    # mounted read-only at /art for the lookup.
    addr2line_image = os.environ.get("ADDR2LINE_IMAGE", "").strip()

    def addr2line_cmd(offset: int):
        if not addr2line_image:
            return ["addr2line", "-f", "-C", "-e", module_path, "0x%x" % offset]
        folder = os.path.dirname(os.path.abspath(module_path))
        name = os.path.basename(module_path)
        return ["docker", "run", "--rm", "-v", "%s:/art:ro" % folder, addr2line_image,
                "addr2line", "-f", "-C", "-e", "/art/%s" % name, "0x%x" % offset]

    with open(core_path, "rb") as fh:
        header = fh.read(64)
        if header[:4] != b"\x7fELF":
            print("not an ELF core:", header[:8])
            return 1
        phoff = struct.unpack_from("<Q", header, 0x20)[0]
        phentsize = struct.unpack_from("<H", header, 0x36)[0]
        phnum = struct.unpack_from("<H", header, 0x38)[0]

        loads, notes = [], []
        fh.seek(phoff)
        for _ in range(phnum):
            ph = fh.read(phentsize)
            p_type = struct.unpack_from("<I", ph, 0)[0]
            p_offset, p_vaddr, _p_paddr, p_filesz, p_memsz, _flags = struct.unpack_from(
                "<QQQQQQ", ph, 8)
            if p_type == PT_LOAD:
                loads.append((p_vaddr, p_filesz, p_offset))
            elif p_type == PT_NOTE:
                notes.append((p_offset, p_filesz))

        def vaddr_to_offset(vaddr):
            for va, fsz, off in loads:
                if va <= vaddr < va + fsz:
                    return off + (vaddr - va)
            return None

        def read_vaddr(vaddr, size):
            off = vaddr_to_offset(vaddr)
            if off is None:
                return None
            with open(core_path, "rb") as reader:
                reader.seek(off)
                return reader.read(size)

        def parse_notes(data):
            i = 0
            while i + 12 <= len(data):
                namesz, descsz, ntype = struct.unpack_from("<III", data, i)
                i += 12
                name = data[i:i + namesz].rstrip(b"\x00")
                i += (namesz + 3) & ~3
                desc = data[i:i + descsz]
                i += (descsz + 3) & ~3
                yield ntype, name, desc

        pstatus, psinfo, nt_file = [], [], None
        for off, size in notes:
            fh.seek(off)
            for ntype, _name, desc in parse_notes(fh.read(size)):
                if ntype == NT_PRSTATUS:
                    pstatus.append(desc)
                elif ntype == 3:  # NT_PRPSINFO
                    psinfo.append(desc)
                elif ntype == NT_FILE:
                    nt_file = desc

    if not nt_file:
        print("no NT_FILE note; cannot locate the module")
        return 1

    count, page_size = struct.unpack_from("<QQ", nt_file, 0)
    cursor = 16
    ranges = []
    for _ in range(count):
        start, end, file_off = struct.unpack_from("<QQQ", nt_file, cursor)
        cursor += 24
        ranges.append((start, end, file_off))
    names = nt_file[cursor:].split(b"\x00")

    module_ranges = []
    for (start, end, file_off), raw in zip(ranges, names):
        if not raw:
            continue
        base = os.path.basename(raw.decode("utf-8", "replace"))
        # The dump records the deployed name (libTortoiseBots.so); the copy kept for
        # analysis carries its build stamp (libTortoiseBots-1005-0608.so).
        if base.startswith("libTortoiseBots"):
            module_ranges.append((start, end, file_off * page_size))
    if not module_ranges:
        print("module %s is not mapped in this core" % module_path)
        return 1
    print("module mappings: %s" % ", ".join("0x%x-0x%x(+0x%x)" % r for r in module_ranges))

    # x86_64 elf_prstatus: register block starts at byte 112 (27 * 8 bytes of gregs).
    order = ["r15", "r14", "r13", "r12", "rbp", "rbx", "r11", "r10", "r9", "r8", "rax",
             "rcx", "rdx", "rsi", "rdi", "orig_rax", "rip", "cs", "eflags", "rsp", "ss"]
    threads = []
    for idx, desc in enumerate(pstatus):
        regs_off = 112
        if len(desc) < regs_off + len(order) * 8:
            continue
        regs = struct.unpack_from("<" + "Q" * len(order), desc, regs_off)
        rip, rsp = regs[16], regs[19]
        if not (0x400000 <= rip < 0x800000000000):
            regs_off = 120  # some layouts pad the pid block
            regs = struct.unpack_from("<" + "Q" * len(order), desc, regs_off)
            rip, rsp = regs[16], regs[19]
        comm = ""
        if idx < len(psinfo):
            comm = psinfo[idx][40:56].split(b"\x00")[0].decode("utf-8", "replace")
        # pr_info.si_signo is the first int of elf_prstatus: the crashing thread is the one
        # that carries the fault signal, which is a far more reliable marker than guessing.
        signo = struct.unpack_from("<i", desc, 0)[0] if len(desc) >= 4 else 0
        threads.append((idx, comm, rip, rsp, signo))

    def resolve(addr):
        for start, end, file_off in module_ranges:
            if start <= addr < end:
                offset = addr - start + file_off
                out = subprocess.run(addr2line_cmd(offset), capture_output=True, text=True)
                return out.stdout.strip().replace("\n", " ")
        return None

    print("threads: %d  crashing candidates: %s"
          % (len(threads), [t[0] for t in threads if t[4] not in (0, 5)]))
    for idx, comm, rip, rsp, signo in threads:
        stack = read_vaddr(rsp, 262144) if rsp else None
        frames = []
        if stack:
            for offset in range(0, len(stack) - 8, 8):
                value = struct.unpack_from("<Q", stack, offset)[0]
                for start, end, _file_off in module_ranges:
                    if start <= value < end:
                        frames.append((offset, value))
                        break
        if not frames:
            continue
        print("\nthread %d (%s) rip=0x%x rsp=0x%x  module frames: %d"
              % (idx, comm or "?", rip, rsp, len(frames)))
        seen = set()
        for offset, value in frames[:40]:
            where = resolve(value)
            if where in seen:
                continue
            seen.add(where)
            print("   rsp+0x%05x  0x%x  %s" % (offset, value, where))
    return 0


if __name__ == "__main__":
    sys.exit(main())
