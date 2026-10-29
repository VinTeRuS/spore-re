#!/usr/bin/env python3
"""Finds the statically linked MSVC helpers (INLINE_HELPERS of a game config) and the CRT setjmp/longjmp import
slots in a new image, using an already configured game as reference: each helper's bytes up to its first ret or jmp, with
the dwords the reference's base relocations touch masked out, are searched in the target's .text. Prints a config
snippet; a helper with no match or several matches is reported instead of guessed.

Usage: find_helpers.py <reference game.py> <target image>
"""
import sys

import pefile
import capstone

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
import recomp  # noqa: E402


def signature(pe, va):
    """(bytes, mask) of the helper at va, up to and including its first ret or jmp (relative call/jump targets
    masked: they move with the code)."""
    base = pe.OPTIONAL_HEADER.ImageBase
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    code = pe.get_data(va - base, 256)
    end = None
    rel = set()
    for ins in md.disasm(code, va):
        if ins.mnemonic in ("call", "jmp") and ins.size == 5 and ins.bytes[0] in (0xE8, 0xE9):
            rel.update(range(ins.address - va + 1, ins.address - va + 5))
        if ins.mnemonic.startswith("ret") or ins.mnemonic == "jmp":
            end = ins.address + ins.size - va
            break
    if end is None:
        raise ValueError(f"no ret in helper {va:#x}")
    relocated = set()
    for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
        for e in block.entries:
            rva = e.rva
            if va - base <= rva < va - base + end:
                relocated.update(range(rva - (va - base), rva - (va - base) + 4))
    data = code[:end]
    mask = bytes(0 if i in relocated or i in rel else 0xFF for i in range(end))
    return data, mask


def find(text, start, data, mask):
    hits = []
    first = next(i for i, m in enumerate(mask) if m)
    i = text.find(data[first:first + 1])
    while i != -1:
        s = i - first
        if s >= 0 and all(not m or text[s + k] == data[k] for k, m in enumerate(mask)):
            hits.append(start + s)
        i = text.find(data[first:first + 1], i + 1)
    return hits


def import_slots(pe, names):
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
    out = {}
    for dll in pe.DIRECTORY_ENTRY_IMPORT:
        for imp in dll.imports:
            if imp.name and imp.name.decode() in names:
                out[imp.name.decode()] = imp.address
    return out


def main():
    recomp.load_config(sys.argv[1])
    ref = pefile.PE(recomp.IMAGE, fast_load=True)
    ref.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]])
    tgt = pefile.PE(sys.argv[2], fast_load=True)
    text = tgt.sections[0]
    tbase = tgt.OPTIONAL_HEADER.ImageBase + text.VirtualAddress
    tdata = text.get_data()
    found = {}
    for va, kind in sorted(recomp.INLINE_HELPERS.items(), key=lambda kv: kv[1]):
        data, mask = signature(ref, va)
        hits = find(tdata, tbase, data, mask)
        if len(hits) == 1:
            found[hits[0]] = kind
        else:
            print(f"# {kind}: {len(hits)} matches ({', '.join(f'{h:#x}' for h in hits[:8])})")
    print("INLINE_HELPERS = {" + ", ".join(f'0x{a:08x}: "{k}"' for a, k in sorted(found.items())) + "}")
    slots = import_slots(tgt, {"_setjmp3", "_setjmp", "longjmp"})
    print("SETJMP_IAT = {" + ", ".join(f"0x{a:08x}" for n, a in slots.items() if "setjmp" in n) + "}")
    print("LONGJMP_IAT = {" + ", ".join(f"0x{a:08x}" for n, a in slots.items() if n == "longjmp") + "}")


if __name__ == "__main__":
    main()
