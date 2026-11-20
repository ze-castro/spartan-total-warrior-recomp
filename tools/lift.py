#!/usr/bin/env python3
"""x86-32 -> C lifter, integer subset (prototype).

lift_function() turns one discovered function into a C function operating on
the CPU register file (see runtime/rt.h). Anything unsupported raises Unsup
and the function is reported as not (yet) liftable.

CLI: .venv/bin/python -I tools/lift.py game/default.xbe analysis/functions.json build/lifted.c
"""
import collections
import json
import struct
import sys

import capstone
from capstone import x86_const as X

R32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
R16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx",
       "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
R8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
R8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}
UT = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}
ST = {1: "int8_t", 2: "int16_t", 4: "int32_t"}
MASK = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}

COND = {
    "o": "OF", "no": "!OF", "b": "CF", "ae": "!CF", "e": "ZF", "ne": "!ZF",
    "be": "(CF|ZF)", "a": "(!CF&&!ZF)", "s": "SF", "ns": "!SF",
    "p": "PF", "np": "!PF", "l": "(SF!=OF)", "ge": "(SF==OF)",
    "le": "(ZF||(SF!=OF))", "g": "(!ZF&&(SF==OF))",
}


class Unsup(Exception):
    pass


md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True


def regparse(name):
    if name in R32:
        return name, 4, 0
    if name in R16:
        return R16[name], 2, 0
    if name in R8L:
        return R8L[name], 1, 0
    if name in R8H:
        return R8H[name], 1, 8
    raise Unsup("reg:" + name)


def rd_reg(name):
    full, sz, sh = regparse(name)
    if sz == 4:
        return full
    if sz == 2:
        return f"(uint16_t){full}"
    return f"(uint8_t)({full}>>{sh})" if sh else f"(uint8_t){full}"


def wr_reg(name, expr):
    full, sz, sh = regparse(name)
    if sz == 4:
        return f"{full}=(uint32_t)({expr});"
    if sz == 2:
        return f"{full}=({full}&0xffff0000u)|(uint16_t)({expr});"
    if sh:
        return f"{full}=({full}&0xffff00ffu)|((uint32_t)(uint8_t)({expr})<<8);"
    return f"{full}=({full}&0xffffff00u)|(uint8_t)({expr});"


def mem_addr(o):
    m = o.mem
    if m.segment not in (0, X.X86_REG_FS):
        raise Unsup("segment")
    parts = []
    if m.segment == X.X86_REG_FS:               # KPCR/TIB lives at a fixed guest address
        parts.append("FS_BASE")
    if m.base:
        parts.append(md.reg_name(m.base))
    if m.index:
        parts.append(f"{md.reg_name(m.index)}*{m.scale}u")
    d = m.disp & 0xFFFFFFFF
    if d or not parts:
        parts.append(f"0x{d:x}u")
    for p in parts:
        if p in ("fs", "gs", "es", "cs", "ds", "ss"):
            raise Unsup("segment")
    return "(uint32_t)(" + "+".join(parts) + ")"


CMP_PRED = {"eq": 0, "lt": 1, "le": 2, "unord": 3, "neq": 4, "nlt": 5, "nle": 6, "ord": 7}
SSE_BIN = {"add": "add", "sub": "sub", "mul": "mul", "div": "div", "min": "min", "max": "max"}
SSE_UN = {"sqrt": "sqrt", "rcp": "rcp", "rsqrt": "rsqrt"}
SSE_BIT = {"andps": "and", "andnps": "andn", "orps": "or", "xorps": "xor"}



MMX_BIN = {n: "mm_" + n for n in (
    "paddb paddw paddd psubb psubw psubd paddsb paddsw psubsb psubsw paddusb paddusw psubusb psubusw "
    "pmullw pmulhw pmulhuw pcmpeqb pcmpeqw pcmpeqd pcmpgtb pcmpgtw pcmpgtd pavgb pavgw pminub pmaxub "
    "pminsw pmaxsw pand pandn por pxor pmaddwd punpcklbw punpckhbw punpcklwd punpckhwd punpckldq punpckhdq "
    "packsswb packuswb packssdw psllw pslld psllq psrlw psrld psrlq psraw psrad").split()}


def is_mm(o):
    return o.type == X.X86_OP_REG and md.reg_name(o.reg).startswith("mm")


def is_xmm(o):
    return o.type == X.X86_OP_REG and md.reg_name(o.reg).startswith("xmm")


CODE_RANGE = (0, 0)   # image VA range that may hold executable code
OVERRIDES = set()     # library entry points replaced by runtime handlers (lib_<Name>): never lifted, always called via the runtime
DATA_RANGES = []      # initialised-data sections scanned for function pointers
LIFT_RANGES = []      # [lo, hi) ranges that are lifted: .text plus CPU-only XDK library sections


def plausible_start(img, base, a):
    """A .text address that begins right after inter-function padding (int3/nop) or a return."""
    o = a - base
    return o > 0 and img[o - 1] in (0xCC, 0x90, 0xC3) or (o > 1 and img[o - 3] == 0xC2)


def prologue_at(img, base, a):
    """push ebp; mov ebp, esp: a function entry even when jump-table data (not padding) precedes it."""
    o = a - base
    return img[o:o + 3] == b"\x55\x8b\xec"


def in_code(a):
    return any(lo <= a < hi for lo, hi in LIFT_RANGES)
JT = {}          # jmp address -> list of target addresses (resolved switch tables)


class Fn:
    """Lifts one function."""

    def __init__(self, start, insns, funcstarts):
        self.start, self.insns, self.funcstarts = start, insns, funcstarts
        self.calls = set()
        self.ext = set()
        self.cur = 0
        self.lines = []
        self.used_x = set()
        self.used_f = False
        self.transcendental = False

    # --- SSE operand helpers ----------------------------------------------
    def xr(self, o):
        n = md.reg_name(o.reg)
        self.used_x.add(int(n[3:]))
        return n

    def x128(self, o):
        return self.xr(o) if is_xmm(o) else f"LDX({mem_addr(o)})"

    def xf(self, o):
        return f"{self.xr(o)}.f[0]" if is_xmm(o) else f"LDF({mem_addr(o)})"

    # --- MMX ---------------------------------------------------------------
    def mmr(self, o):
        return f"c->mm[{int(md.reg_name(o.reg)[2:])}]"

    def mm64(self, o):
        return self.mmr(o) if is_mm(o) else f"LD64({mem_addr(o)})"

    def mmx(self, ins, m, ops, e):
        """MMX instructions on the mm0-mm7 file in the CPU struct. Returns True if handled."""
        if m == "emms":
            return True
        if not (any(is_mm(o) for o in ops) or m == "movntq"):
            return False
        if m == "movq":
            if is_mm(ops[0]):
                e(f"{self.mmr(ops[0])}={self.mm64(ops[1])};")
            else:
                e(f"ST64({mem_addr(ops[0])},{self.mmr(ops[1])});")
        elif m == "movntq":
            e(f"ST64({mem_addr(ops[0])},{self.mmr(ops[1])});")
        elif m == "movd":
            if is_mm(ops[0]):
                e(f"{self.mmr(ops[0])}=(uint64_t){self.rd(ops[1], 4)};")
            else:
                e(self.wr(ops[0], 4, f"(uint32_t){self.mmr(ops[1])}"))
        elif m in MMX_BIN:
            src = self.mm64(ops[1]) if ops[1].type != X.X86_OP_IMM else f"{ops[1].imm & 0xFF}u"
            e(f"{self.mmr(ops[0])}={MMX_BIN[m]}({self.mmr(ops[0])},{src});")
        elif m == "pshufw":
            e(f"{self.mmr(ops[0])}=mm_pshufw({self.mm64(ops[1])},{ops[2].imm & 0xFF}u);")
        elif m == "pmovmskb":
            e(self.wr(ops[0], 4, f"mm_pmovmskb({self.mmr(ops[1])})"))
        else:
            raise Unsup(m)
        return True

    # --- x87 ---------------------------------------------------------------
    @staticmethod
    def sti(o):
        import re
        mm = re.fullmatch(r"st\((\d)\)", md.reg_name(o.reg))
        if not mm:
            raise Unsup("x87-reg")
        return int(mm.group(1))

    def fmem(self, o, integer=False):
        a = mem_addr(o)
        if integer:
            return {2: f"((double)(int16_t)LD16({a}))", 4: f"((double)(int32_t)LD32({a}))",
                    8: f"((double)(int64_t)LD64({a}))"}[o.size]
        if o.size == 4:
            return f"((double)LDF({a}))"
        if o.size == 8:
            return f"bd(LD64({a}))"
        if o.size == 10:
            return f"xd_ld80({a})"
        raise Unsup("x87-tbyte")

    def x87(self, ins, m, ops, e):
        """x87 instructions. Returns True if handled."""
        if m == "wait" or m == "fwait":
            return True
        if not (m.startswith("f") and m != "fs"):
            return False
        self.used_f = True
        OPS = {"add": "xd_add", "sub": "xd_sub", "mul": "xd_mul", "div": "xd_div"}
        base = m[1:]
        integer = base.startswith("i") and base not in ("ist", "istp")
        arith = base[1:] if integer else base
        pop = arith.endswith("p") and arith[:-1] in ("add", "sub", "subr", "mul", "div", "divr")
        core = arith[:-1] if pop else arith
        if core in ("add", "sub", "subr", "mul", "div", "divr"):
            rev = core.endswith("r") and core != "subr" or core in ("subr", "divr")
            op = OPS[core[:-1] if core in ("subr", "divr") else core]
            if ops and ops[0].type == X.X86_OP_MEM:
                dst, src = "ST(0)", self.fmem(ops[0], integer)
            elif len(ops) == 2:
                dst, src = f"ST({self.sti(ops[0])})", f"ST({self.sti(ops[1])})"
            elif len(ops) == 1 and pop:
                dst, src = f"ST({self.sti(ops[0])})", "ST(0)"
            elif len(ops) == 1:
                dst, src = "ST(0)", f"ST({self.sti(ops[0])})"
            else:
                raise Unsup("x87-arith-form")
            call = f"{op}(s,d)" if rev else f"{op}(d,s)"
            e(f"{{double s={src};double d={dst};{dst}={call};" + ("FPOP();" if pop else "") + "}")
            return True
        if m == "fld":
            if ops[0].type == X.X86_OP_MEM:
                e(f"FPUSH({self.fmem(ops[0])});")
            else:
                e(f"FPUSH(ST({self.sti(ops[0])}));")
        elif m in ("fld1", "fldz", "fldpi", "fldl2e", "fldl2t", "fldlg2", "fldln2"):
            c_ = {"fld1": "1.0", "fldz": "0.0", "fldpi": "3.14159265358979323846",
                  "fldl2e": "1.44269504088896340736", "fldl2t": "3.32192809488736234787",
                  "fldlg2": "0.301029995663981195214", "fldln2": "0.693147180559945309417"}[m]
            e(f"FPUSH({c_});")
        elif m == "fild":
            e(f"FPUSH({self.fmem(ops[0], True)});")
        elif m in ("fst", "fstp"):
            o = ops[0]
            if o.type == X.X86_OP_MEM:
                if o.size == 4:
                    e(f"ST32({mem_addr(o)},fb((float)ST(0)));")
                elif o.size == 8:
                    e(f"ST64({mem_addr(o)},db(ST(0)));")
                elif o.size == 10:
                    e(f"xd_st80({mem_addr(o)},ST(0));")
                else:
                    raise Unsup("x87-tbyte")
            else:
                i = self.sti(o)
                if i != 0:
                    e(f"ST({i})=ST(0);")
            if m == "fstp":
                e("FPOP();")
        elif m in ("fist", "fistp"):
            o = ops[0]
            bits = o.size * 8
            e(f"{{uint64_t v=xd_fist(ST(0),{bits},(c->fcw>>10)&3u);ST{bits}({mem_addr(o)},v);}}")
            if m == "fistp":
                e("FPOP();")
        elif m == "fxch":
            i = self.sti(ops[-1]) if ops else 1
            e(f"{{double t=ST(0);ST(0)=ST({i});ST({i})=t;}}")
        elif m in ("fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp"):
            if ops and ops[0].type == X.X86_OP_MEM:
                src = self.fmem(ops[0])
            else:
                src = f"ST({self.sti(ops[0]) if ops else 1})"
            e(f"fcc=xd_cc(ST(0),{src});")
            npop = 2 if m.endswith("pp") else 1 if m.endswith("p") else 0
            e("FPOP();" * npop)
        elif m == "ftst":
            e("fcc=xd_cc(ST(0),0.0);")
        elif m in ("fcomi", "fcompi", "fucomi", "fucompi"):
            e(f"{{uint32_t k=xd_cc(ST(0),ST({self.sti(ops[0])}));ZF=!!(k&0x4000);PF=!!(k&0x400);"
              "CF=!!(k&0x100);OF=0;SF=0;AF=0;}")
            if m.endswith("pi"):
                e("FPOP();")
        elif m == "fabs":
            e("ST(0)=bd(db(ST(0))&~XD_SIGN);")
        elif m == "fchs":
            e("ST(0)=bd(db(ST(0))^XD_SIGN);")
        elif m == "fsqrt":
            e("ST(0)=xd_sqrt(ST(0));")
        elif m == "frndint":
            e("ST(0)=xd_fix1(ST(0),xd_round(ST(0),(c->fcw>>10)&3u));")
        elif m == "fscale":
            e("ST(0)=xd_scale(ST(0),ST(1));")
        elif m in ("fsin", "fcos"):
            self.transcendental = True
            e(f"{{double x=ST(0);fcc=(__builtin_fabs(x)>=9223372036854775808.0)?(fcc|0x400):(fcc&~0x400u);"
              f"ST(0)=xd_{m[1:]}(x);}}")
        elif m == "fsincos":
            self.transcendental = True
            e("{double x=ST(0);fcc=(__builtin_fabs(x)>=9223372036854775808.0)?(fcc|0x400):(fcc&~0x400u);"
              "ST(0)=xd_sin(x);FPUSH(xd_cos(x));}")
        elif m == "fptan":
            self.transcendental = True
            e("{double x=ST(0);fcc=(__builtin_fabs(x)>=9223372036854775808.0)?(fcc|0x400):(fcc&~0x400u);"
              "ST(0)=xd_tan(x);FPUSH(1.0);}")
        elif m == "f2xm1":
            self.transcendental = True
            e("ST(0)=xd_f2xm1(ST(0));")
        elif m in ("fnclex", "fclex"):
            pass
        elif m == "fxam":
            e("{double x=ST(0);uint64_t u=db(x);uint32_t k;"
              "if(x!=x)k=0x100;else if(__builtin_isinf(x))k=0x500;else if(x==0)k=0x4000;"
              "else if(((u>>52)&0x7FF)==0)k=0x4400;else k=0x400;"
              "fcc=k|((u>>63)?0x200u:0);}")
        elif m == "frstor":
            e(f"{{uint32_t a={mem_addr(ops[0])};c->fcw=LD16(a);uint32_t sw=LD16(a+4);ftop=(sw>>11)&7;"
              "fcc=sw&0x4700;for(int i=0;i<8;i++)ST(i)=xd_ld80(a+28+10*i);}")
        elif m in ("fnsave", "fsave"):
            a_ = mem_addr(ops[0])
            e(f"{{uint32_t a={a_};memset(G+a,0,108);ST16(a,(uint16_t)c->fcw);ST16(a+4,(uint16_t)(fcc|(ftop<<11)));"
              "ST16(a+8,0xFFFF);for(int i=0;i<8;i++)xd_st80(a+28+10*i,ST(i));"
              "c->fcw=0x37F;ftop=0;fcc=0;}")
        elif m == "fpatan":
            self.transcendental = True
            e("ST(1)=xd_atan2(ST(1),ST(0));FPOP();")
        elif m == "fyl2x":
            self.transcendental = True
            e("ST(1)=xd_yl2x(ST(1),ST(0));FPOP();")
        elif m == "fnstsw":
            if ops[0].type == X.X86_OP_REG:
                e(wr_reg("ax", "(fcc|(ftop<<11))"))
            else:
                e(f"ST16({mem_addr(ops[0])},(uint16_t)(fcc|(ftop<<11)));")
        elif m in ("fnstcw", "fstcw"):
            e(f"ST16({mem_addr(ops[0])},(uint16_t)c->fcw);")
        elif m == "fldcw":
            e(f"c->fcw=LD16({mem_addr(ops[0])});")
        else:
            self.used_f = False
            return False
        return True

    def sse(self, ins, m, ops, e):
        """SSE1 instructions. Returns True if handled."""
        if m in ("movaps", "movups"):
            if is_xmm(ops[0]):
                e(f"{self.xr(ops[0])}={self.x128(ops[1])};")
            else:
                e(f"STX({mem_addr(ops[0])},{self.xr(ops[1])});")
        elif m == "movntps":
            e(f"STX({mem_addr(ops[0])},{self.xr(ops[1])});")
        elif m == "movss":
            if is_xmm(ops[0]) and is_xmm(ops[1]):
                e(f"{self.xr(ops[0])}.u[0]={self.xr(ops[1])}.u[0];")
            elif is_xmm(ops[0]):
                e(f"{{XMM t;t.q[0]=0;t.q[1]=0;t.u[0]=LD32({mem_addr(ops[1])});{self.xr(ops[0])}=t;}}")
            else:
                e(f"ST32({mem_addr(ops[0])},{self.xr(ops[1])}.u[0]);")
        elif m in ("movlps", "movhps"):
            lane = 0 if m == "movlps" else 1
            if is_xmm(ops[0]):
                e(f"{self.xr(ops[0])}.q[{lane}]=LD64({mem_addr(ops[1])});")
            else:
                e(f"ST64({mem_addr(ops[0])},{self.xr(ops[1])}.q[{lane}]);")
        elif m == "movlhps":
            e(f"{self.xr(ops[0])}.q[1]={self.xr(ops[1])}.q[0];")
        elif m == "movhlps":
            e(f"{self.xr(ops[0])}.q[0]={self.xr(ops[1])}.q[1];")
        elif m == "movmskps":
            e(self.wr(ops[0], 4, f"p_movmsk({self.x128(ops[1])})"))
        elif m in SSE_BIT:
            d = self.xr(ops[0])
            e(f"{d}=p_{SSE_BIT[m]}({d},{self.x128(ops[1])});")
        elif m[:-2] in SSE_BIN and m.endswith(("ss", "ps")):
            d = self.xr(ops[0])
            op = SSE_BIN[m[:-2]]
            if m.endswith("ss"):
                e(f"{d}=s_{op}({d},{self.xf(ops[1])});")
            else:
                e(f"{d}=p_{op}({d},{self.x128(ops[1])});")
        elif m[:-2] in SSE_UN and m.endswith(("ss", "ps")):
            d = self.xr(ops[0])
            op = SSE_UN[m[:-2]]
            if m.endswith("ss"):
                e(f"{d}=s_{op}({d},{self.xf(ops[1])});")
            else:
                e(f"{d}=p_{op}({self.x128(ops[1])});")
        elif m in ("comiss", "ucomiss"):
            e(f"{{float a={self.xf(ops[0])};float b={self.xf(ops[1])};"
              "if(a!=a||b!=b){ZF=1;PF=1;CF=1;}else if(a>b){ZF=0;PF=0;CF=0;}"
              "else if(a<b){ZF=0;PF=0;CF=1;}else{ZF=1;PF=0;CF=0;}OF=0;SF=0;AF=0;}")
        elif m.startswith("cmp") and m.endswith(("ss", "ps")) and \
                (m[3:-2] in CMP_PRED or (m[3:-2] == "" and len(ops) == 3)):
            pred = CMP_PRED[m[3:-2]] if m[3:-2] else ops[2].imm & 7
            d = self.xr(ops[0])
            if m.endswith("ss"):
                e(f"{d}=s_cmp({pred},{d},{self.xf(ops[1])});")
            else:
                e(f"{d}=p_cmp({pred},{d},{self.x128(ops[1])});")
        elif m == "cvtsi2ss":
            d = self.xr(ops[0])
            e(f"{d}=s_cvtsi({d},(int32_t){self.rd(ops[1], 4)});")
        elif m == "cvttss2si":
            e(self.wr(ops[0], 4, f"x_cvtt({self.xf(ops[1])})"))
        elif m == "cvtss2si":
            e(self.wr(ops[0], 4, f"x_cvt({self.xf(ops[1])})"))
        elif m == "shufps":
            d = self.xr(ops[0])
            e(f"{d}=p_shuf({d},{self.x128(ops[1])},{ops[2].imm & 0xFF});")
        elif m in ("unpcklps", "unpckhps"):
            d = self.xr(ops[0])
            e(f"{d}=p_{m}({d},{self.x128(ops[1])});")
        elif m in ("sfence", "lfence", "mfence"):               # x86 ordering: make them real fences on ARM
            e("__atomic_thread_fence(__ATOMIC_SEQ_CST);")
        elif m in ("prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2"):
            pass
        else:
            return False
        return True

    # --- operand helpers --------------------------------------------------
    def rd(self, o, size):
        if o.type == X.X86_OP_REG:
            return rd_reg(md.reg_name(o.reg))
        if o.type == X.X86_OP_IMM:
            return f"(({UT[size]})0x{o.imm & MASK[size]:x}u)"
        if o.type == X.X86_OP_MEM:
            return f"LD{size*8}({mem_addr(o)})"
        raise Unsup("operand")

    def wr(self, o, size, expr):
        if o.type == X.X86_OP_REG:
            return wr_reg(md.reg_name(o.reg), expr)
        if o.type == X.X86_OP_MEM:
            return f"ST{size*8}({mem_addr(o)},{expr});"
        raise Unsup("write-operand")

    def target(self, t):
        if t in self.insns or t == self.start:
            # a backward jump may be a poll loop on memory another thread or a device changes:
            # force the compiler to re-read guest memory on every iteration
            if t <= self.cur:
                return f"{{POLL();goto L_{t:08x};}}"
            return f"goto L_{t:08x};"
        if t in self.funcstarts and t not in OVERRIDES:
            self.calls.add(t)
            return f"{{TSPILL();f_{t:08x}(c);return;}}"
        if CODE_RANGE[0] <= t < CODE_RANGE[1]:          # tail jump into static-lib / mid-function code
            self.ext.add(t)
            return f"{{TSPILL();call_ind(c,0x{t:x}u);return;}}"
        raise Unsup("branch-target")

    # --- instruction emitters ---------------------------------------------
    def zsp(self, B):
        return f"ZF=(r==0);SF=(r>>{B-1})&1;PF=PAR(r);"

    def lift(self, ins):
        m = ins.mnemonic
        ops = ins.operands
        a = ins.address
        self.cur = a
        nxt = a + ins.size
        out = []
        e = out.append

        rep = ""
        cc = None
        if m.startswith("rep "):
            rep, m = "rep", m[4:]
        elif m.startswith(("repne ", "repe ")):
            cc, m = m.split(" ", 1)

        if cc and m in ("cmpsb", "cmpsw", "cmpsd", "scasb", "scasw", "scasd"):
            sz = {"b": 1, "w": 2, "d": 4}[m[-1]]
            B, T = sz * 8, UT[sz]
            step = f"(DF?-{sz}u:{sz}u)"
            if m.startswith("cmps"):
                a_, adv = f"LD{B}(esi)", f"esi+={step};edi+={step};"
            else:
                a_, adv = {1: "(uint8_t)eax", 2: "(uint16_t)eax", 4: "eax"}[sz], f"edi+={step};"
            stop = "!ZF" if cc == "repe" else "ZF"
            e(f"while(ecx){{{T} a={a_};{T} b=LD{B}(edi);{T} r=({T})(a-b);CF=(a<b);"
              f"OF=(((a^b)&(a^r))>>{B-1})&1;AF=((a^b^r)>>4)&1;{self.zsp(B)}{adv}ecx--;if({stop})break;}}")
            return out
        if cc:
            raise Unsup("repcc")

        if m in ("movsb", "movsw", "movsd", "stosb", "stosw", "stosd") and \
                ops and ops[0].type == X.X86_OP_MEM:
            sz = {"b": 1, "w": 2, "d": 4}[m[-1]]
            n = "ecx" if rep else "1u"
            step = f"(DF?-{sz}u:{sz}u)"
            if m.startswith("movs"):
                body = f"ST{sz*8}(edi,LD{sz*8}(esi));esi+={step};edi+={step};"
            else:
                v = {1: "(uint8_t)eax", 2: "(uint16_t)eax", 4: "eax"}[sz]
                body = f"ST{sz*8}(edi,{v});edi+={step};"
            e(f"{{uint32_t n={n};while(n--){{{body}}}" + ("ecx=0;" if rep else "") + "}")
            return out
        if rep:
            raise Unsup("rep:" + m)

        # ---------------- data movement ----------------
        if m == "mov":
            sz = ops[0].size
            e(self.wr(ops[0], sz, self.rd(ops[1], sz)))
        elif m == "movzx":
            e(self.wr(ops[0], ops[0].size, f"(uint32_t){self.rd(ops[1], ops[1].size)}"))
        elif m == "movsx":
            e(self.wr(ops[0], ops[0].size,
                      f"(uint32_t)(int32_t)({ST[ops[1].size]}){self.rd(ops[1], ops[1].size)}"))
        elif m == "lea":
            e(self.wr(ops[0], ops[0].size, mem_addr(ops[1])))
        elif m == "cmpxchg":                                    # one guest lock serialises everything: plain compare + exchange
            sz = ops[0].size
            B, T = sz * 8, UT[sz]
            acc = {1: "al", 2: "ax", 4: "eax"}[sz]
            e(f"{{{T} a=({T})({rd_reg(acc)});{T} b={self.rd(ops[0], sz)};{T} r=({T})(a-b);CF=(a<b);OF=(((a^b)&(a^r))>>{B-1})&1;"
              f"AF=((a^b^r)>>4)&1;{self.zsp(B)}if(a==b){{{self.wr(ops[0], sz, self.rd(ops[1], sz))}}}else{{{wr_reg(acc, 'b')}}}}}")
        elif m == "xadd":
            sz = ops[0].size
            B, T = sz * 8, UT[sz]
            e(f"{{{T} a={self.rd(ops[0], sz)};{T} b={self.rd(ops[1], sz)};uint64_t w=(uint64_t)a+b;{T} r=({T})w;CF=(w>>{B})&1;"
              f"OF=(((a^r)&(b^r))>>{B-1})&1;AF=((a^b^r)>>4)&1;{self.zsp(B)}{self.wr(ops[1], sz, 'a')}{self.wr(ops[0], sz, 'r')}}}")
        elif m == "xchg":
            sz = ops[0].size
            e(f"{{{UT[sz]} x={self.rd(ops[0], sz)};{UT[sz]} y={self.rd(ops[1], sz)};"
              f"{self.wr(ops[0], sz, 'y')}{self.wr(ops[1], sz, 'x')}}}")
        elif m == "push":
            if ops[0].size != 4:
                raise Unsup("push16")
            e(f"{{uint32_t v={self.rd(ops[0], 4)};esp-=4;ST32(esp,v);}}")
        elif m == "pop":
            if ops[0].type == X.X86_OP_REG and ops[0].size == 4 and md.reg_name(ops[0].reg) == "esp":
                e("esp=LD32(esp);")
                return out
            if ops[0].size != 4:
                raise Unsup("pop-form")
            e(f"{{uint32_t v=LD32(esp);esp+=4;{self.wr(ops[0], 4, 'v')}}}")
        elif m == "leave":
            e("esp=ebp;ebp=LD32(esp);esp+=4;")
        elif m == "nop":
            pass
        elif m == "cld":
            e("DF=0;")
        elif m == "std":
            e("DF=1;")
        elif m == "lahf":
            e("eax=(eax&0xffff00ffu)|((uint32_t)((SF<<7)|(ZF<<6)|(AF<<4)|(PF<<2)|2|CF)<<8);")
        elif m == "sahf":
            e("{uint32_t h=(eax>>8)&0xffu;SF=(h>>7)&1;ZF=(h>>6)&1;AF=(h>>4)&1;PF=(h>>2)&1;CF=h&1;}")
        elif m == "cdq":
            e("edx=(uint32_t)((int32_t)eax>>31);")
        elif m == "cwde":
            e("eax=(uint32_t)(int32_t)(int16_t)eax;")
        elif m == "bswap":
            e(self.wr(ops[0], 4, f"__builtin_bswap32({self.rd(ops[0], 4)})"))

        # ---------------- ALU ----------------
        elif m in ("add", "adc", "sub", "sbb", "cmp"):
            sz = ops[0].size
            B, T = sz * 8, UT[sz]
            s = f"{{{T} a={self.rd(ops[0], sz)};{T} b={self.rd(ops[1], sz)};"
            if m == "add":
                s += f"uint64_t w=(uint64_t)a+b;{T} r=({T})w;CF=(w>>{B})&1;"
                s += f"OF=(((a^r)&(b^r))>>{B-1})&1;"
            elif m == "adc":
                s += f"uint64_t w=(uint64_t)a+b+CF;{T} r=({T})w;CF=(w>>{B})&1;"
                s += f"OF=(((a^r)&(b^r))>>{B-1})&1;"
            elif m in ("sub", "cmp"):
                s += f"{T} r=({T})(a-b);CF=(a<b);OF=(((a^b)&(a^r))>>{B-1})&1;"
            else:
                s += f"{T} r=({T})((uint64_t)a-b-CF);CF=((uint64_t)a<(uint64_t)b+CF);"
                s += f"OF=(((a^b)&(a^r))>>{B-1})&1;"
            s += f"AF=((a^b^r)>>4)&1;{self.zsp(B)}"
            if m != "cmp":
                s += self.wr(ops[0], sz, "r")
            e(s + "}")
        elif m in ("and", "or", "xor", "test"):
            sz = ops[0].size
            B, T = sz * 8, UT[sz]
            op = {"and": "&", "or": "|", "xor": "^", "test": "&"}[m]
            s = f"{{{T} a={self.rd(ops[0], sz)};{T} b={self.rd(ops[1], sz)};{T} r=({T})(a{op}b);"
            s += f"CF=0;OF=0;AF=0;{self.zsp(B)}"
            if m != "test":
                s += self.wr(ops[0], sz, "r")
            e(s + "}")
        elif m in ("inc", "dec"):
            sz = ops[0].size
            B, T = sz * 8, UT[sz]
            d = "+" if m == "inc" else "-"
            of = f"(r==({T})(1u<<{B-1}))" if m == "inc" else f"(a==({T})(1u<<{B-1}))"
            e(f"{{{T} a={self.rd(ops[0], sz)};{T} r=({T})(a{d}1);OF={of};AF=((a^1^r)>>4)&1;"
              f"{self.zsp(B)}{self.wr(ops[0], sz, 'r')}}}")
        elif m == "neg":
            sz = ops[0].size
            B, T = sz * 8, UT[sz]
            e(f"{{{T} a={self.rd(ops[0], sz)};{T} r=({T})(0-a);CF=(a!=0);OF=(a==({T})(1u<<{B-1}));"
              f"AF=((a^r)>>4)&1;{self.zsp(B)}{self.wr(ops[0], sz, 'r')}}}")
        elif m == "not":
            sz = ops[0].size
            e(f"{{{UT[sz]} a={self.rd(ops[0], sz)};{self.wr(ops[0], sz, f'({UT[sz]})~a')}}}")
        elif m in ("shl", "sal", "shr", "sar", "rol", "ror"):
            sz = ops[0].size
            B, T = sz * 8, UT[sz]
            if len(ops) == 1:
                cnt, const = "1u", 1
            elif ops[1].type == X.X86_OP_IMM:
                const = ops[1].imm & 31
                cnt = f"{const}u"
                if const == 0:
                    return out
            else:
                cnt, const = "(ecx&31u)", None
            body =f"{T} a={self.rd(ops[0], sz)};uint32_t n={cnt};"
            if m in ("shl", "sal"):
                body += (f"uint64_t w=(uint64_t)a<<n;{T} r=({T})w;CF=(w>>{B})&1;"
                         f"OF=((r>>{B-1})&1)^CF;{self.zsp(B)}")
            elif m == "shr":
                body += (f"{T} r=({T})(a>>n);CF=(a>>(n-1))&1;OF=(a>>{B-1})&1;{self.zsp(B)}")
            elif m == "sar":
                body += (f"{ST[sz]} s=({ST[sz]})a;{T} r=({T})(s>>n);CF=(s>>(n-1))&1;OF=0;"
                         f"{self.zsp(B)}")
            elif m == "rol":
                body += (f"uint32_t k=n%{B}u;{T} r=k?({T})((a<<k)|(a>>({B}u-k))):a;CF=r&1;"
                         f"OF=((r>>{B-1})&1)^CF;")
            else:
                body += (f"uint32_t k=n%{B}u;{T} r=k?({T})((a>>k)|(a<<({B}u-k))):a;"
                         f"CF=(r>>{B-1})&1;OF=((r>>{B-1})^(r>>{B-2}))&1;")
            body += self.wr(ops[0], sz, "r")
            # variable count (cl): count 0 leaves value and flags untouched
            e("{" + (f"if({cnt}!=0u){{{body}}}" if const is None else body) + "}")
        elif m in ("rcl", "rcr"):
            sz = ops[0].size
            B, T = sz * 8, UT[sz]
            if len(ops) == 1:
                cnt = "1u"
            elif ops[1].type == X.X86_OP_IMM:
                cnt = f"{ops[1].imm & 31}u"
            else:
                cnt = "(ecx&31u)"
            if m == "rcr":
                rot = f"((v>>k)|(v<<({B+1}u-k)))"
                of = f"OF=((r>>{B-1})^(r>>{B-2}))&1;"
            else:
                rot = f"((v<<k)|(v>>({B+1}u-k)))"
                of = f"OF=((r>>{B-1})&1)^CF;"
            e(f"{{uint32_t n={cnt}%{B+1}u;if(n){{{T} a={self.rd(ops[0], sz)};"
              f"uint64_t v=((uint64_t)CF<<{B})|a;uint32_t k=n;"
              f"uint64_t q={rot}&((1ull<<{B+1})-1);CF=(q>>{B})&1;{T} r=({T})q;{of}"
              f"{self.wr(ops[0], sz, 'r')}}}}}")
        elif m in ("shld", "shrd"):
            if ops[0].size != 4:
                raise Unsup(m)
            cnt = f"{ops[2].imm & 31}u" if ops[2].type == X.X86_OP_IMM else "(ecx&31u)"
            if m == "shld":
                core = "uint32_t r=(a<<n)|(b>>(32u-n));CF=(a>>(32u-n))&1;"
            else:
                core = "uint32_t r=(a>>n)|(b<<(32u-n));CF=(a>>(n-1))&1;"
            e(f"{{uint32_t n={cnt};if(n){{uint32_t a={self.rd(ops[0], 4)};uint32_t b={self.rd(ops[1], 4)};"
              f"{core}OF=((r^a)>>31)&1;{self.zsp(32)}{self.wr(ops[0], 4, 'r')}}}}}")
        elif m == "imul":
            if len(ops) == 1:
                if ops[0].size == 1:
                    e(f"{{int32_t w=(int32_t)(int8_t)eax*(int32_t)(int8_t){self.rd(ops[0], 1)};"
                      f"{wr_reg('ax', 'w')}CF=OF=(w!=(int8_t)w);}}")
                    return out
                if ops[0].size == 2:
                    e(f"{{int32_t w=(int32_t)(int16_t)eax*(int32_t)(int16_t){self.rd(ops[0], 2)};"
                      f"{wr_reg('ax', 'w')}{wr_reg('dx', '(uint32_t)w>>16')}CF=OF=(w!=(int16_t)w);}}")
                    return out
                if ops[0].size != 4:
                    raise Unsup("imul1-size")
                e(f"{{int64_t w=(int64_t)(int32_t)eax*(int64_t)(int32_t){self.rd(ops[0], 4)};"
                  f"eax=(uint32_t)w;edx=(uint32_t)((uint64_t)w>>32);CF=OF=(w!=(int64_t)(int32_t)eax);}}")
            else:
                sz = ops[0].size
                if sz == 1:
                    raise Unsup("imul8")
                src1, src2 = (ops[0], ops[1]) if len(ops) == 2 else (ops[1], ops[2])
                S = ST[sz]
                if src2.type == X.X86_OP_IMM:
                    b = f"(int64_t)({S})0x{src2.imm & MASK[sz]:x}"
                else:
                    b = f"(int64_t)({S}){self.rd(src2, sz)}"
                e(f"{{int64_t w=(int64_t)({S}){self.rd(src1, sz)}*{b};{UT[sz]} r=({UT[sz]})w;"
                  f"CF=OF=(w!=(int64_t)({S})r);{self.zsp(sz*8)}{self.wr(ops[0], sz, 'r')}}}")
        elif m == "mul":
            if ops[0].size == 1:
                e(f"{{uint32_t w=(uint32_t)(uint8_t)eax*(uint32_t){self.rd(ops[0], 1)};"
                  f"{wr_reg('ax', 'w')}CF=OF=((w>>8)!=0);}}")
                return out
            if ops[0].size == 2:
                e(f"{{uint32_t w=(uint32_t)(uint16_t)eax*(uint32_t){self.rd(ops[0], 2)};"
                  f"{wr_reg('ax', 'w')}{wr_reg('dx', 'w>>16')}CF=OF=((w>>16)!=0);}}")
                return out
            if ops[0].size != 4:
                raise Unsup("mul-size")
            e(f"{{uint64_t w=(uint64_t)eax*(uint64_t){self.rd(ops[0], 4)};eax=(uint32_t)w;"
              f"edx=(uint32_t)(w>>32);CF=OF=(edx!=0);}}")
        elif m in ("div", "idiv"):
            if ops[0].size != 4:
                raise Unsup("div-size")
            if m == "div":
                e(f"{{uint64_t n=((uint64_t)edx<<32)|eax;uint32_t d={self.rd(ops[0], 4)};"
                  f"eax=(uint32_t)(n/d);edx=(uint32_t)(n%d);}}")
            else:
                e(f"{{int64_t n=(int64_t)(((uint64_t)edx<<32)|eax);int32_t d=(int32_t){self.rd(ops[0], 4)};"
                  f"eax=(uint32_t)(int32_t)(n/d);edx=(uint32_t)(int32_t)(n%d);}}")
        elif m in ("bsf", "bsr"):
            sz = ops[0].size
            if sz != 4:
                raise Unsup("bs-size")
            f = "__builtin_ctz(v)" if m == "bsf" else "(31^__builtin_clz(v))"
            e(f"{{uint32_t v={self.rd(ops[1], 4)};ZF=(v==0);if(v){{{self.wr(ops[0], 4, f)}}}}}")

        # ---------------- conditionals ----------------
        elif m.startswith("set") and m[3:] in COND:
            e(self.wr(ops[0], 1, f"({COND[m[3:]]})?1:0"))
        elif m.startswith("cmov") and m[4:] in COND:
            sz = ops[0].size
            e(f"{{{UT[sz]} v={self.rd(ops[1], sz)};if({COND[m[4:]]}){{{self.wr(ops[0], sz, 'v')}}}}}")

        # ---------------- control flow ----------------
        elif m == "ret":
            extra = ops[0].imm if ops else 0
            e(f"esp+={4 + extra}u;SPILL();return;")
        elif m == "call":
            o = ops[0]
            if o.type != X.X86_OP_IMM:
                e(f"{{uint32_t t={self.rd(o, 4)};esp-=4;ST32(esp,0x{nxt:x}u);SPILL();call_ind(c,t);RELOAD();}}")
                return out
            t = o.imm & 0xFFFFFFFF
            if t == nxt:                      # call $+5 (get-pc idiom)
                e(f"esp-=4;ST32(esp,0x{nxt:x}u);")
            elif t in self.funcstarts and t not in OVERRIDES:
                self.calls.add(t)
                e(f"esp-=4;ST32(esp,0x{nxt:x}u);SPILL();f_{t:08x}(c);RELOAD();")
            else:                           # static-lib / mid-function entry: resolved by the runtime
                self.ext.add(t)
                e(f"esp-=4;ST32(esp,0x{nxt:x}u);SPILL();call_ind(c,0x{t:x}u);RELOAD();")
        elif m == "jmp":
            o = ops[0]
            if o.type != X.X86_OP_IMM:
                tg = JT.get(ins.address)
                if tg is not None:
                    cases = "".join(f"case 0x{t:x}u:{self.target(t)}" for t in sorted(set(tg)))
                    e(f"switch({self.rd(o, 4)}){{{cases}default:__builtin_trap();}}")
                else:
                    e(f"{{uint32_t t={self.rd(o, 4)};TSPILL();call_ind(c,t);return;}}")
                return out
            e(self.target(o.imm & 0xFFFFFFFF))
        elif m.startswith("j") and m[1:] in COND:
            o = ops[0]
            if o.type != X.X86_OP_IMM:
                raise Unsup("jcc-indirect")
            e(f"if({COND[m[1:]]}){{{self.target(o.imm & 0xFFFFFFFF)}}}")
        elif m in ("in", "out"):
            port = lambda o: "(uint16_t)edx" if o.type == X.X86_OP_REG else f"0x{o.imm & 0xFFFF:x}u"
            if m == "in":
                sz = ops[0].size
                e(self.wr(ops[0], sz, f"rt_in({sz},{port(ops[1])})"))
            else:
                sz = ops[1].size
                e(f"rt_out({sz},{port(ops[0])},{self.rd(ops[1], sz)});")
        elif m in ("cli", "sti", "wbinvd", "invd"):
            pass
        elif m == "jecxz":
            o = ops[0]
            e(f"if(ecx==0){{{self.target(o.imm & 0xFFFFFFFF)}}}")
        elif m == "rdtsc":
            e("{uint64_t t=rt_rdtsc();eax=(uint32_t)t;edx=(uint32_t)(t>>32);}")
        elif m == "pushfd":
            e("{uint32_t f=CF|(PF<<2)|(AF<<4)|(ZF<<6)|(SF<<7)|(DF<<10)|(OF<<11)|0x202u;esp-=4;ST32(esp,f);}")
        elif m == "popfd":
            e("{uint32_t f=LD32(esp);esp+=4;CF=f&1;PF=(f>>2)&1;AF=(f>>4)&1;ZF=(f>>6)&1;SF=(f>>7)&1;DF=(f>>10)&1;OF=(f>>11)&1;}")
        elif m in ("int3", "ud2"):
            e("__builtin_trap();")
        elif self.mmx(ins, m, ops, e):
            pass
        elif self.x87(ins, m, ops, e):
            pass
        elif self.sse(ins, m, ops, e):
            pass
        else:
            raise Unsup(m)
        return out


def resolve_switch_local(insns, ins, img, base, text, start):
    """jmp [idx*4+table] -> list of targets, mirroring discover.py's resolver, for functions
    discovery never analysed. Bound = last `cmp r, imm` before the jmp; an optional byte-index
    table (movzx idx, byte [r+tbl2]) maps the index to a table slot."""
    o = ins.operands[0]
    table = o.mem.disp & 0xFFFFFFFF
    try:
        idxfam = regparse(md.reg_name(o.mem.index))[0]
    except Unsup:
        return None
    hist = [insns[x] for x in sorted(insns) if x < ins.address][-24:]
    rd8 = lambda va: img[va - base] if base <= va < base + len(img) else None
    rd32 = lambda va: struct.unpack_from("<I", img, va - base)[0] if base <= va and va + 4 <= base + len(img) else None
    bound = None
    for h in reversed(hist):
        if h.mnemonic == "cmp" and len(h.operands) == 2 and h.operands[1].type == X.X86_OP_IMM:
            bound = (h.operands[1].imm & 0xFFFFFFFF) + 1
            break
    byte_tbl = None
    for h in reversed(hist):
        wrote = False
        for op in h.operands[:1]:
            if op.type == X.X86_OP_REG:
                try:
                    wrote = regparse(md.reg_name(op.reg))[0] == idxfam
                except Unsup:
                    pass
        if wrote:
            if h.mnemonic in ("movzx", "mov") and len(h.operands) == 2 and h.operands[1].type == X.X86_OP_MEM \
                    and h.operands[1].size == 1 and base <= (h.operands[1].mem.disp & 0xFFFFFFFF) < base + len(img):
                byte_tbl = h.operands[1].mem.disp & 0xFFFFFFFF
            break
    if bound is not None and not (1 <= bound <= 1024):
        return None
    if byte_tbl is not None and bound is not None:
        vals = [rd8(byte_tbl + i) for i in range(bound)]
        if None in vals:
            return None
        count = max(vals) + 1
    else:
        count = bound if bound is not None else 256      # unbounded: scan while entries stay plausible
    out = []
    for i in range(count):
        v = rd32(table + 4 * i)
        if v is None or not in_code(v):
            break
        if bound is None and abs(v - start) > 0x10000:
            break
        out.append(v)
    return out or None


def build_cfg(start, img, base, text, funcstarts):
    tva, tend = text
    insns = {}
    work = [start]
    while work:
        a = work.pop()
        while in_code(a) and a not in insns:
            if a != start and a in funcstarts:
                break                                   # falls into another function
            ins = next(md.disasm(img[a - base:a - base + 15], a, 1), None)
            if ins is None:
                raise Unsup("decode")
            insns[a] = ins
            m = ins.mnemonic
            nxt = a + ins.size
            if m == "ret" or m in ("int3", "ud2"):
                break
            if m == "jmp":
                o = ins.operands[0]
                if o.type == X.X86_OP_IMM:
                    t = o.imm & 0xFFFFFFFF
                    if t not in funcstarts and t not in insns:
                        work.append(t)
                elif ins.operands[0].type == X.X86_OP_MEM and ins.operands[0].mem.index and \
                        ins.operands[0].mem.scale == 4 and not ins.operands[0].mem.base:
                    if a not in JT:
                        r = resolve_switch_local(insns, ins, img, base, text, start)
                        if r:
                            JT[a] = r
                    if a in JT:
                        work.extend(t for t in JT[a] if t not in insns)
                break
            if ins.group(X.X86_GRP_JUMP) and ins.operands[0].type == X.X86_OP_IMM:
                t = ins.operands[0].imm & 0xFFFFFFFF
                if t not in funcstarts and t not in insns:
                    work.append(t)
            a = nxt
    return insns


def lift_function(start, img, base, text, funcstarts):
    """Returns (c_source, called_function_set). Raises Unsup."""
    insns = build_cfg(start, img, base, text, funcstarts)
    fn = Fn(start, insns, funcstarts)
    for ins in insns.values():                  # code addresses used as values (callbacks) in library code
        for op in ins.operands:
            if op.type == X.X86_OP_IMM:
                v = op.imm & 0xFFFFFFFF
                if in_code(v) and (not (text[0] <= v < text[1]) or
                                   (v not in funcstarts and plausible_start(img, base, v))):
                    fn.ext.add(v)
    body = [f"goto L_{start:08x};"]    # blocks are emitted in address order; some precede the entry
    addrs = sorted(insns)
    for i, a in enumerate(addrs):
        ins = insns[a]
        body.append(f"L_{a:08x}:;")
        body.extend(fn.lift(ins))
        nxt = a + ins.size
        m = ins.mnemonic
        falls = not (m in ("ret", "jmp", "int3", "ud2"))
        if falls:
            nxt_in_order = addrs[i + 1] if i + 1 < len(addrs) else None
            if nxt_in_order != nxt:
                if nxt in insns or nxt == start:
                    body.append(f"goto L_{nxt:08x};")
                elif nxt in funcstarts and nxt not in OVERRIDES:
                    fn.calls.add(nxt)
                    body.append(f"{{TSPILL();f_{nxt:08x}(c);return;}}")
                else:
                    raise Unsup(f"falls-off:{a:#x}->{nxt:#x}")
    body.append("__builtin_trap();")
    text_body = "\n".join(body)
    # Only the XMM registers this function touches are loaded, spilled and reloaded;
    # untouched ones stay in the CPU struct for callers/callees.
    xs = sorted(fn.used_x)
    xload = "".join(f"XMM xmm{i}=c->xmm[{i}];" for i in xs)
    xspill = "".join(f"c->xmm[{i}]=xmm{i};" for i in xs)
    xreload = "".join(f"xmm{i}=c->xmm[{i}];" for i in xs)
    if fn.used_f:
        xload += "double fs[8];uint32_t ftop=c->ftop,fcc=c->fcc;memcpy(fs,c->fst,sizeof fs);"
        xspill += "XSPILL();"
        xreload += "XRELOAD();"
    text_body = text_body.replace("TSPILL();", "SPILL();FSPILL();")
    text_body = text_body.replace("SPILL();", "SPILL();" + xspill)
    text_body = text_body.replace("RELOAD();", "RELOAD();" + xreload)
    src = (f"void f_{start:08x}(CPU *c) {{\n"
           f"if (__builtin_expect(rt_trace_fn, 0)) rt_trace_enter(c, 0x{start:x}u);\n"
           "uint32_t eax=c->eax,ecx=c->ecx,edx=c->edx,ebx=c->ebx,esp=c->esp,ebp=c->ebp,"
           "esi=c->esi,edi=c->edi;\n"
           + xload + "\n"
           "uint32_t CF=c->flags&1,PF=(c->flags>>2)&1,AF=(c->flags>>4)&1,ZF=(c->flags>>6)&1,"
           "SF=(c->flags>>7)&1,OF=(c->flags>>11)&1;\n"
           "uint32_t DF=0;(void)DF;(void)CF;(void)ZF;(void)SF;(void)OF;(void)PF;(void)AF;\n"
           + text_body + "\n}\n")
    return src, fn.calls, fn.transcendental, fn.ext


def lift_all(img, base, text, starts):
    """Lift every function; return (results, blockers) where results maps
    start -> (src, calls) for functions whose whole call tree is liftable."""
    funcstarts = set(starts)
    raw, why = {}, {}
    pending = list(starts)
    for lo, hi in DATA_RANGES:                  # function pointers in data (vtables, callback tables)
        for a in range(lo, hi - 3, 4):
            v = struct.unpack_from("<I", img, a - base)[0]
            if in_code(v) and v not in funcstarts and (not (text[0] <= v < text[1]) or plausible_start(img, base, v) or prologue_at(img, base, v)):
                pending.append(v)
                funcstarts.add(v)
    while pending:
        for s in pending:
            if s in OVERRIDES:
                continue
            try:
                raw[s] = lift_function(s, img, base, text, funcstarts)
            except Unsup as u:
                why[s] = str(u)
            except Exception as ex:                  # lifter bug: report, don't crash
                why[s] = "lifter-bug:" + type(ex).__name__ + ":" + str(ex)[:60]
        # call targets inside .text that discovery missed become functions of their own
        found = {t for s in pending if s in raw for t in raw[s][3] if in_code(t)}
        pending = sorted(found - funcstarts)
        funcstarts |= set(pending)
    # closure: a function is OK only if every callee is OK
    ok = set(raw)
    blame = {}
    changed = True
    while changed:
        changed = False
        for s in list(ok):
            bad = [c for c in raw[s][1] if c not in ok]
            if bad:
                ok.discard(s)
                why[s] = "callee-not-liftable"
                blame[s] = bad[0]
                changed = True
    def root(s):
        while s in blame:
            s = blame[s]
        return s
    global ROOTS, BLAME
    BLAME = blame
    ROOTS = collections.Counter(root(s) for s in blame)
    ROOTS = [(r, n, why.get(r)) for r, n in ROOTS.most_common(25)]
    return {s: raw[s] for s in ok}, why


def load_overrides():
    """Addresses of library functions that have a hand-written runtime handler (lib_<Name>)."""
    import glob
    import os
    import re
    impl = set()
    for fn in glob.glob("runtime/*.c") + glob.glob("runtime/*.m") + glob.glob("runtime/*.mm"):
        impl |= set(re.findall(r"^uint32_t lib_(\w+)\(", open(fn).read(), re.M))
    out = {}
    if os.path.exists("analysis/libsyms.json"):
        for a, cands in json.load(open("analysis/libsyms.json")).items():
            names = [c["name"] for c in cands]
            if len(names) == 1 and names[0] in impl:      # ambiguous matches are never overridden automatically
                out[int(a, 16)] = names[0]
    for line in open("runtime/data/libnames.txt"):
        f = line.split("#")[0].split()
        if len(f) >= 2 and f[1] in impl:
            out[int(f[0], 16)] = f[1]
    return set(out)


def apply_patches(img, base):
    """runtime/data/patches.txt: `address old new` (hex) rewrites one dword of the game's code before it is translated, so a single
    instruction can read a different address (used for the widescreen aspect ratio). `old` is checked so a different executable is refused."""
    import os
    path = "runtime/data/patches.txt"
    if not os.path.exists(path):
        return
    for line in open(path):
        line = line.split("#")[0].split()
        if not line:
            continue
        a, old, new = (int(x, 16) for x in line[:3])
        o = a - base
        if struct.unpack_from("<I", img, o)[0] != old:
            raise SystemExit(f"patches.txt: dword at {a:#x} is not {old:#x}: wrong executable?")
        struct.pack_into("<I", img, o, new)


def load(xbe_path, json_path):
    sys.path.insert(0, "tools")
    import importlib.util
    spec = importlib.util.spec_from_file_location("disc", "tools/discover.py")
    disc = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(disc)
    base, img, secs, entry, thunk = disc.load_xbe(xbe_path)
    img = bytearray(img)
    apply_patches(img, base)
    d = json.load(open(json_path))
    tva, tvs = d["text"]
    starts = sorted(int(k, 16) for k in d["functions"])
    OVERRIDES.clear()
    OVERRIDES.update(load_overrides())
    global CODE_RANGE
    CODE_RANGE = (tva, base + len(img))
    # CPU-only library sections are lifted like .text; D3D / DSOUND / XPP drive hardware and are
    # replaced by the runtime instead (override with STW_LIFT_SECTIONS=A,B,...).
    import os
    want = os.environ.get("STW_LIFT_SECTIONS", "XGRPH,D3DX,XMV,D3D,XPP,DSOUND").split(",")
    DATA_RANGES[:] = [(secs[n][0], secs[n][0] + secs[n][1]) for n in (".rdata", ".data") if n in secs]
    LIFT_RANGES[:] = [(tva, tva + tvs)] + [(secs[n][0], secs[n][0] + secs[n][1]) for n in want if n in secs]
    for jt in d["jump_tables"]:
        tb, n = int(jt["table"], 16), jt["entries"]
        JT[int(jt["jmp"], 16)] = [struct.unpack_from("<I", img, tb - base + 4 * i)[0] for i in range(n)]
    return base, img, secs, (tva, tva + tvs), starts


def main():
    xbe, js, out = sys.argv[1:4]
    base, img, secs, text, starts = load(xbe, js)
    ok, why = lift_all(img, base, text, starts)
    reasons = collections.Counter(why.values())
    print(f"functions: {len(starts)}  fully liftable: {len(ok)} "
          f"({100*len(ok)/len(starts):.1f}%)")
    print("top blockers:")
    for r, n in reasons.most_common(30):
        print(f"  {n:6d}  {r}")
    print("top root blockers (func, dependents, reason):")
    for r, n, w in ROOTS:
        print(f"  {r:#x} {n:5d} {w}")
    with open(out, "w") as f:
        f.write('#include "rt.h"\n#include <stdint.h>\n')
        for s in sorted(ok):
            f.write(f"void f_{s:08x}(CPU *c);\n")
        for s in sorted(ok):
            f.write(ok[s][0])
        f.write("typedef struct { uint32_t addr; void (*fn)(CPU *); } FuncEnt;\n")
        f.write("const FuncEnt FUNCS[] = {\n")
        for s in sorted(ok):
            f.write(f"  {{0x{s:x}u, f_{s:08x}}},\n")
        f.write("};\n")
        f.write(f"const int NFUNCS = {len(ok)};\n")
        f.write("void call_ind(CPU *c, uint32_t t) {\n"
                "  int lo = 0, hi = NFUNCS - 1;\n"
                "  while (lo <= hi) { int m = (lo + hi) >> 1;\n"
                "    if (FUNCS[m].addr == t) { FUNCS[m].fn(c); return; }\n"
                "    if (FUNCS[m].addr < t) lo = m + 1; else hi = m - 1; }\n"
                "  rt_bad_indirect(c, t);\n}\n")
    json.dump({"liftable": sorted(ok), "transcendental": sorted(k for k, v in ok.items() if v[2]),
               "blame": {hex(k): hex(v) for k, v in BLAME.items()},
               "ext_targets": sorted({t for v in ok.values() for t in v[3]}),
               "why": {hex(k): v for k, v in why.items()}},
              open(out + ".json", "w"))


if __name__ == "__main__":
    main()
