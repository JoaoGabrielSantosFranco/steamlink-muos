"""AArch64 string xref helper for a stripped ELF.

Usage:
  uv run --with pyelftools --with capstone python -I tools/xref.py <elf> index   # builds <elf>.xrefs (one-time)
  uv run --with pyelftools --with capstone python -I tools/xref.py <elf> str "<substring>"
  uv run --with pyelftools --with capstone python -I tools/xref.py <elf> addr 0x1234
  uv run --with pyelftools --with capstone python -I tools/xref.py <elf> func 0x1234 [max_insns]

`index` resolves every adrp+add / adrp+ldr pair to a target address.
`str` finds strings containing the substring and lists the code that references them.
`func` disassembles the function containing an address, annotating referenced strings.
"""
import pathlib
import re
import sys

from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs
from elftools.elf.elffile import ELFFile


class Binary:
    def __init__(self, path):
        self.path = pathlib.Path(path)
        self.raw = self.path.read_bytes()
        elf = ELFFile(open(path, "rb"))
        self.segments = [
            (s["p_vaddr"], s["p_offset"], s["p_filesz"])
            for s in elf.iter_segments() if s["p_type"] == "PT_LOAD"
        ]
        text = elf.get_section_by_name(".text")
        self.text_addr, self.text_off, self.text_size = text["sh_addr"], text["sh_offset"], text["sh_size"]
        self.md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)

    def off_to_va(self, off):
        for va, fo, sz in self.segments:
            if fo <= off < fo + sz:
                return va + off - fo
        return None

    def va_to_off(self, va):
        for sva, fo, sz in self.segments:
            if sva <= va < sva + sz:
                return fo + va - sva
        return None

    def cstr(self, va, limit=200):
        off = self.va_to_off(va)
        if off is None:
            return None
        end = self.raw.find(b"\0", off, off + limit)
        s = self.raw[off:end if end != -1 else off + limit]
        if len(s) < 3 or not all(32 <= c < 127 or c in (9, 10) for c in s):
            return None
        return s.decode()

    def insns(self, start, end):
        code = self.raw[self.text_off + start - self.text_addr:self.text_off + end - self.text_addr]
        return self.md.disasm_lite(code, start)


IMM = re.compile(r"#(-?0x[0-9a-f]+|-?\d+)")


def build_index(b):
    """Return list of (insn_addr, target) for adrp+add and adrp+ldr pairs."""
    pages = {}
    out = []
    for addr, _, mnem, ops in b.insns(b.text_addr, b.text_addr + b.text_size):
        if mnem == "adrp":
            reg, imm = ops.split(", ")
            pages[reg] = int(imm.lstrip("#"), 16)
            continue
        if mnem in ("add", "ldr", "ldrb", "ldrh", "ldrsw", "str") and pages:
            parts = [p.strip() for p in ops.replace("[", "").replace("]", "").split(",")]
            if mnem == "add" and len(parts) == 3 and parts[1] in pages and parts[2].startswith("#"):
                out.append((addr, pages[parts[1]] + int(IMM.match(parts[2]).group(1), 0)))
            elif mnem != "add" and len(parts) >= 2 and parts[1] in pages:
                imm = int(IMM.match(parts[2]).group(1), 0) if len(parts) > 2 and parts[2].startswith("#") else 0
                out.append((addr, pages[parts[1]] + imm))
        if mnem in ("bl", "b", "ret", "br", "blr"):
            pages.clear()
    return out


def load_index(b):
    idx_path = b.path.with_name(b.path.name + ".xrefs")
    if not idx_path.exists():
        sys.exit("run `index` first")
    pairs = [tuple(int(x, 16) for x in line.split()) for line in idx_path.read_text().splitlines()]
    by_target = {}
    for a, t in pairs:
        by_target.setdefault(t, []).append(a)
    return pairs, by_target


def func_start(b, addr):
    """Walk backwards to the nearest prologue that follows a ret/b/brk."""
    a = addr
    lo = max(b.text_addr, addr - 0x8000)
    best = None
    while a > lo:
        a -= 4
        insn = next(b.insns(a, a + 4), None)
        if insn is None:
            continue
        _, _, mnem, ops = insn
        if mnem == "paciasp" or (mnem == "stp" and ops.startswith("x29, x30, [sp, #-")) or (
                mnem == "sub" and ops.startswith("sp, sp, #")):
            best = a
            prev = next(b.insns(a - 4, a), None)
            if prev and prev[2] in ("ret", "b", "brk", "nop"):
                return a
    return best or addr


def cmd_index(b):
    pairs = build_index(b)
    b.path.with_name(b.path.name + ".xrefs").write_text("".join("%x %x\n" % p for p in pairs))
    print("%d refs indexed" % len(pairs))


def cmd_str(b, needle):
    _, by_target = load_index(b)
    for m in re.finditer(re.escape(needle.encode()), b.raw):
        s = m.start()
        while s > 0 and b.raw[s - 1] != 0:
            s -= 1
        va = b.off_to_va(s)
        print("0x%x %r" % (va, b.cstr(va)))
        for r in by_target.get(va, []):
            print("    ref @0x%x  func~0x%x" % (r, func_start(b, r)))


def cmd_func(b, addr, max_insns):
    pairs, _ = load_index(b)
    ref_at = dict(pairs)
    start = func_start(b, addr)
    for a, _, mnem, ops in b.insns(start, start + max_insns * 4):
        note = ""
        if a in ref_at:
            s = b.cstr(ref_at[a])
            note = "    ; 0x%x %s" % (ref_at[a], repr(s) if s else "")
        print("%x\t%s\t%s%s" % (a, mnem, ops, note))


def main():
    b = Binary(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == "index":
        cmd_index(b)
    elif cmd == "str":
        cmd_str(b, sys.argv[3])
    elif cmd == "func":
        cmd_func(b, int(sys.argv[3], 16), int(sys.argv[4]) if len(sys.argv) > 4 else 600)
    elif cmd == "addr":
        _, by_target = load_index(b)
        for r in by_target.get(int(sys.argv[3], 16), []):
            print("ref @0x%x  func~0x%x" % (r, func_start(b, r)))


if __name__ == "__main__":
    main()
