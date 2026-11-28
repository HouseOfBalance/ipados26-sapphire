#!/usr/bin/env python3
"""Offline arm64e -> ARMv8.0 (Sapphire Cortex-A73) De-PAC & ISA Auditor/Rewriter.

Rewrites non-NOP ARMv8.3 Pointer Authentication (PAC) instructions in Mach-O
executables, dylibs, bundles, and dyld_shared_cache_arm64e subcaches into
1-to-1 32-bit ARMv8.0 instructions while keeping Mach-O structure, offsets,
and CPU_SUBTYPE_ARM64E headers intact for dyld/XNU shared-cache compatibility.

Instruction rewrites (all exact 4-byte replacements, zero branch displacement):
  - RETAA / RETAB                       -> RET (0xD65F03C0)
  - BLRAA / BLRAB / BLRAAZ / BLRABZ     -> BLR Xn (0xD63F0000 | Rn<<5)
  - BRAA / BRAB / BRAAZ / BRABZ         -> BR Xn  (0xD61F0000 | Rn<<5)
  - ERETAA / ERETAB                     -> ERET   (0xD69F03E0)
  - PACIA/IB/DA/DB/IZA/IZB/DZA/DZB      -> NOP    (0xD503201F)
  - AUTIA/IB/DA/DB/IZA/IZB/DZA/DZB      -> NOP    (0xD503201F)
  - XPACI / XPACD / XPACLRI             -> NOP    (0xD503201F; preserves TBI [63:56] & negative sentinels -1/-2!)
  - DC CVAP / DC CVADP Xt               -> DC CVAC Xt (ARMv8.2/v8.5 persistence flush -> native ARMv8.0 PoC, 0 traps!)
  - SB (0xD50330FF)                     -> ISB (0xD5033FDF) (ARMv8.5 speculation barrier -> native ARMv8.0 ISB, prevents SIGILL!)
  - LDAPR / LDAPRB / LDAPRH             -> LDAR / LDARB / LDARH (native ARMv8.0 acquire, 0 traps!)
  - LDRAA / LDRAB Xt, [Xn, #simm10*8]   -> LDR Xt, [Xn, #uimm] (if W=0, off>=0)
                                        -> LDUR Xt, [Xn, #simm9] (if W=0, -256<=off<0)
                                        (W=1 writeback or off<-256 left for XNU sleh.c trap)
  - HINT-space PACIASP/AUTIASP/BTI/etc. -> already hardware NOP on ARMv8.0 Cortex-A73
  - SSBB (0xD503309F) / PSSBB (0xD503349F) -> DSB #0 / DSB #4 aliases, already execute as DSB SY on ARMv8.0

Audit telemetry (--audit) also counts kernel-emulated ARMv8.1-v8.5 ISA usage (safe via XNU sleh.c):
  - FEAT_DotProd (SDOT, UDOT vector & indexed)
  - FEAT_FCMA    (FCADD, FCMLA vector & indexed)
  - FEAT_FHM     (FMLAL, FMLSL, FMLAL2, FMLSL2 vector & indexed)
  - FEAT_FP16    (Scalar H0..H31 & ASIMDHP .4H/.8H, excluding native ARMv8.0 FCVT/FCVTL/FCVTN)
  - FEAT_RDM     (SQRDMLAH, SQRDMLSH vector & scalar, 3-same & indexed)
  - FJCVTZS, LSE Atomics (LDADD..LDUMIN/SWP/CAS/CASP), signed LDAPRSB/H/W, writeback LDRAA/B, PACGA, FlagM

Usage:
  ipados26_depac.py --selftest
  ipados26_depac.py --audit <file_or_dir> [...]
  ipados26_depac.py --rewrite <file_or_dir> [...]
  ipados26_depac.py --rewrite-dsc <dyld_shared_cache_arm64e_prefix>
"""
import glob
import hashlib
import os
import re
import struct
import sys
import tempfile
from collections import Counter, defaultdict

MH_MAGIC_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
FAT_CIGAM = 0xBEBAFECA
FAT_MAGIC_64 = 0xCAFEBABF
FAT_CIGAM_64 = 0xBFBAFECA

CPU_TYPE_ARM64 = 0x0100000C
LC_SEGMENT_64 = 0x19
LC_CODE_SIGNATURE = 0x1D
VM_PROT_EXECUTE = 0x04
S_ATTR_PURE_INSTRUCTIONS = 0x80000000
S_ATTR_SOME_INSTRUCTIONS = 0x00000400
DYLD_CACHE_MAPPING_TEXT_STUBS = 0x20

CSMAGIC_EMBEDDED_SIGNATURE = 0xFADE0CC0
CSMAGIC_CODEDIRECTORY = 0xFADE0C02
CS_SUPPORTSSCATTER = 0x20100
CS_HASHTYPE_SHA1 = 1
CS_HASHTYPE_SHA256 = 2
CS_HASHTYPE_SHA256_TRUNCATED = 3
CS_HASHTYPE_SHA384 = 4

INSN_NOP = 0xD503201F
INSN_RET = 0xD65F03C0
INSN_ERET = 0xD69F03E0
INSN_ISB = 0xD5033FDF
# Because all PAC* and AUT* are rewritten to NOP and dyld runs with ptrauth_disabled=1,
# pointers never carry PAC signatures in [63:48]. Rewriting XPACI, XPACD, and XPACLRI to NOP
# preserves both Darwin TBI [63:56] AND negative sentinel pointers (-1, -2 = 0xfffffffffffffffe
# in libdispatch), preventing Clang -fptrauth-auth-traps (autda + xpacd + cmp + brk #0xc472) traps!

PAC_HINT_IMM7 = frozenset((8, 10, 12, 14, 24, 25, 26, 27, 28, 29, 30, 31))
FP16_SCALAR_1SRC_OPCS = frozenset((0, 1, 2, 3, 8, 9, 10, 11, 12, 14, 15))

# Categories for --audit reporting:
UNREWRITTEN_FATAL_KINDS = (
    'retab', 'eretab', 'braaz', 'blraaz', 'braa', 'blraa',
    'pac_aut', 'xpaci', 'xpacd', 'xpaclri', 'ldraa_ldr', 'ldraa_ldur', 'sb_isb',
)
UNREWRITTEN_OPT_KINDS = ('ldapr_ldar', 'dc_cvap_cvac')
KERNEL_EMULATED_KINDS = (
    'feat_dotprod', 'feat_fcma', 'feat_fhm', 'feat_fp16', 'feat_rdm',
    'fjcvtzs', 'lse_atomic', 'ldraa_trap_left', 'ldaprs_trap_left',
    'pacga_trap_left', 'feat_flagm',
)
HARDWARE_NOP_KINDS = ('pac_hint_nop',)

_ITER_U32_LE = struct.Struct('<I').iter_unpack


def _classify_simd_fp_insn(insn):
    """Identify kernel-emulated ARMv8.1-v8.3 SIMD/FP instructions (safe via XNU sleh.c)."""
    # 1. ARMv8.3 FJCVTZS Wd, Dn:
    if (insn & 0xFFFFFC00) == 0x1E7E0000:
        return 'fjcvtzs'

    # 2. FEAT_DotProd: SDOT / UDOT (vector 0x0E809400/0x2E809400 & indexed 0x0F80E000/0x2F80E000):
    if (insn & 0x9FE0FC00) == 0x0E809400 or (insn & 0x9FC0F400) == 0x0F80E000:
        return 'feat_dotprod'

    # 3. FEAT_FCMA: FCADD (vector) & FCMLA (vector + indexed):
    if (insn & 0xBF20EC00) == 0x2E00E400 or (insn & 0xBF20E400) == 0x2E00C400:
        sz = (insn >> 22) & 3
        if sz in (1, 2) or (sz == 3 and ((insn >> 30) & 1) == 1):
            return 'feat_fcma'
    if (insn & 0xBF009400) == 0x2F001000:
        sz = (insn >> 22) & 3
        if sz == 1 or (sz == 2 and ((insn >> 30) & 1) == 1):
            return 'feat_fcma'

    # 4. FEAT_FHM: FMLAL / FMLSL (0x0E20EC00) & FMLAL2 / FMLSL2 (0x2E20CC00) vector + indexed (0x0F800000 / 0x2F808000):
    if (insn & 0xBF60FC00) in (0x0E20EC00, 0x2E20CC00) or (insn & 0xBFC0B400) in (0x0F800000, 0x2F808000):
        return 'feat_fhm'

    # 5. FEAT_RDM: SQRDMLAH / SQRDMLSH (vector & scalar, 3-same & indexed):
    if ((insn & 0xBF20F400) == 0x2E008400 or (insn & 0xFF20F400) == 0x7E008400 or
            (insn & 0xBF00D400) == 0x2F00D000 or (insn & 0xFF00D400) == 0x7F00D000):
        if ((insn >> 22) & 3) in (1, 2):
            return 'feat_rdm'

    # 6. FEAT_FP16 (Scalar H0..H31 + Vector ASIMDHP .4H/.8H; excludes native ARMv8.0 FCVT/FCVTL/FCVTN):
    # 6a. Vector ASIMDHP 3-same (.4H/.8H: bits[15:14]==00, bit[22]==1, bit[21]==0, bit[10]==1), FMOV #imm8, 2-reg misc:
    if ((insn & 0x9F60C400) == 0x0E400400 or
            (insn & 0xBFF8FC00) == 0x0F00FC00 or
            (insn & 0x9F7E0C00) == 0x0E780800):
        return 'feat_fp16'
    # 6b. Vector ASIMDHP by-element (.4H/.8H: FMLA, FMLS, FMUL, FMULX):
    if (insn & 0x9FC00400) == 0x0F000000:
        opc = (insn >> 12) & 0xF
        ubit = (insn >> 29) & 1
        if (ubit == 0 and opc in (1, 5, 9)) or (ubit == 1 and opc == 9):
            return 'feat_fp16'
    # 6c. Scalar SIMD FP16 3-same / by-element / 2-reg misc:
    if (insn & 0xDF60C400) == 0x5E400400 or (insn & 0xDF7E0C00) == 0x5E780800:
        return 'feat_fp16'
    if (insn & 0xDFC00400) == 0x5F000000 and ((insn >> 12) & 0xF) in (1, 5, 9):
        return 'feat_fp16'
    # 6d. Scalar FP16 3-source (FMADD / FMSUB / FNMADD / FNMSUB Hd):
    if (insn & 0xFFC00000) == 0x1FC00000:
        return 'feat_fp16'
    # 6e. Scalar FP16 data-processing (type == 11 -> 0x1EE0... / 0x9EE0... / 0x1EC0... / 0x9EC0...):
    if (insn & 0x7FC00000) == 0x1EC00000:
        if (insn & 0x00200000) == 0:
            # Fixed-point <-> FP16 (SCVTF / UCVTF / FCVTZS / FCVTZU Hd, Wn/Xn, #fbits)
            rmode = (insn >> 19) & 3
            opc = (insn >> 16) & 7
            if (rmode == 0 and opc in (2, 3)) or (rmode == 3 and opc in (0, 1)):
                if (insn >> 31) == 1 or ((insn >> 10) & 0x3F) >= 32:
                    return 'feat_fp16'
        elif (insn >> 31) == 0:
            if (insn & 0xFFE00C00) == 0x1EE00800 and ((insn >> 12) & 0xF) <= 8:
                return 'feat_fp16'
            if (insn & 0xFFE07C00) == 0x1EE04000 and ((insn >> 15) & 0x3F) in FP16_SCALAR_1SRC_OPCS:
                return 'feat_fp16'
            if ((insn & 0xFFE0FC07) == 0x1EE02000 or
                    (insn & 0xFFE00C10) == 0x1EE00400 or
                    (insn & 0xFFE00C00) == 0x1EE00C00 or
                    (insn & 0xFFE01FE0) == 0x1EE01000):
                return 'feat_fp16'
            if (insn & 0xFFE0FC00) == 0x1EE00000:
                rmode = (insn >> 19) & 3
                opc = (insn >> 16) & 7
                if opc <= 1 or (rmode == 0 and 2 <= opc <= 7):
                    return 'feat_fp16'
        else:
            if (insn & 0xFFE0FC00) == 0x9EE00000:
                rmode = (insn >> 19) & 3
                opc = (insn >> 16) & 7
                if opc <= 1 or (rmode == 0 and 2 <= opc <= 7):
                    return 'feat_fp16'

    return None


def rewrite_arm64e_insn(insn):
    """Return (new_insn, kind_str_or_None). If unchanged, new_insn == insn."""
    b3 = insn >> 24

    # Group A: System / Hints / Barriers / Unconditional Branch (register) [0xD5..0xD7]
    if 0xD5 <= b3 <= 0xD7:
        if b3 == 0xD5:
            # ARMv8.3 XPACLRI (HINT #7) -> NOP (LR never carries PAC bits on De-PAC'd system):
            if insn == 0xD50320FF:
                return INSN_NOP, 'xpaclri'
            # ARMv8.5 SB (0xD50330FF, CRn=3, CRm=0, op2=7) -> unallocated on ARMv8.0 (SIGILL); rewrite to ISB:
            if insn == 0xD50330FF:
                return INSN_ISB, 'sb_isb'
            # ARMv8.2 DC CVAP / ARMv8.5 DC CVADP Xt -> ARMv8.0 DC CVAC Xt (PoP == PoC on Sapphire LPDDR4X):
            if (insn & 0xFFFFFFE0) in (0xD50B7C20, 0xD50B7D20):
                return 0xD50B7A20 | (insn & 0x1F), 'dc_cvap_cvac'
            # HINT-space PAC instructions (PACIASP/AUTIASP/PACIBSP/AUTIBSP/etc. -> hardware NOP on ARMv8.0):
            if (insn & 0xFFFFF01F) == 0xD503201F and ((insn >> 5) & 0x7F) in PAC_HINT_IMM7:
                return insn, 'pac_hint_nop'
            # ARMv8.4/v8.5 FEAT_FlagM / FEAT_FlagM2 (CFINV, XAFLAG, AXFLAG -> kernel trap if present):
            if insn in (0xD500401F, 0xD500403F, 0xD500405F):
                return insn, 'feat_flagm'
            return insn, None

        # 1. Combined Return / ERET with PAC:
        if insn in (0xD65F0BFF, 0xD65F0FFF):
            return INSN_RET, 'retab'
        if insn in (0xD69F0BFF, 0xD69F0FFF):
            return INSN_ERET, 'eretab'

        # 2. Combined Branch with Link / Jump with PAC:
        top = insn & 0xFFFFFC00
        if top in (0xD61F0800, 0xD61F0C00):
            if (insn & 0x1F) == 0x1F:
                rn = (insn >> 5) & 0x1F
                return 0xD61F0000 | (rn << 5), 'braaz'
        elif top in (0xD63F0800, 0xD63F0C00):
            if (insn & 0x1F) == 0x1F:
                rn = (insn >> 5) & 0x1F
                return 0xD63F0000 | (rn << 5), 'blraaz'
        elif top in (0xD71F0800, 0xD71F0C00):
            rn = (insn >> 5) & 0x1F
            return 0xD61F0000 | (rn << 5), 'braa'
        elif top in (0xD73F0800, 0xD73F0C00):
            rn = (insn >> 5) & 0x1F
            return 0xD63F0000 | (rn << 5), 'blraa'
        return insn, None

    # Group B: Data-processing (1/2 source) GP PAC / AUT / XPAC / PACGA [0x9A, 0xDA]
    if (insn & 0xBF000000) == 0x9A000000:
        if (insn & 0xFFFF8000) == 0xDAC10000:
            op2 = (insn >> 10) & 0x1F
            rn = (insn >> 5) & 0x1F
            rd = insn & 0x1F
            if op2 <= 7 or (op2 <= 15 and rn == 31):
                return INSN_NOP, 'pac_aut'
            if op2 == 16 and rn == 31:
                return INSN_NOP, 'xpaci'
            if op2 == 17 and rn == 31:
                return INSN_NOP, 'xpacd'
        elif (insn & 0xFFE0FC00) == 0x9AC03000:
            return insn, 'pacga_trap_left'
        return insn, None

    # Group C: Load/Store Atomic, RCpc LDAPR*, and Authenticated LDRAA/LDRAB [(insn & 0x0F000000) == 0x08000000]
    if (insn & 0x0F000000) == 0x08000000:
        # 4. ARMv8.3 LDAPR / LDAPRB / LDAPRH Wt/Xt, [Xn] -> ARMv8.0 LDAR / LDARB / LDARH Wt/Xt, [Xn]:
        if (insn & 0x3FFFFC00) == 0x38BFC000:
            size_bits = insn & 0xC0000000
            rn_rt = insn & 0x3FF
            return size_bits | 0x08DFFC00 | rn_rt, 'ldapr_ldar'

        # 5. LDRAA / LDRAB: 1111_1000_M S 1 i_iiii_iiii W 1 nn_nnnt_tttt
        if (insn & 0xFF200400) == 0xF8200400:
            s = (insn >> 22) & 1
            imm9 = (insn >> 12) & 0x1FF
            w = (insn >> 11) & 1
            rn = (insn >> 5) & 0x1F
            rt = insn & 0x1F
            simm10 = (imm9 - 512) if s else imm9
            byte_off = simm10 * 8
            if w == 0 and byte_off >= 0:
                return 0xF9400000 | (simm10 << 10) | (rn << 5) | rt, 'ldraa_ldr'
            elif w == 0 and -256 <= byte_off < 0:
                return 0xF8400000 | ((byte_off & 0x1FF) << 12) | (rn << 5) | rt, 'ldraa_ldur'
            else:
                return insn, 'ldraa_trap_left'

        # Signed LDAPRSB / LDAPRSH / LDAPRSW (A=1, R=0, bit21=1, Rs=31, o3=1, opc=6 [Xt, sz<=2] or opc=7 [Wt, sz<=1]):
        if (insn & 0x3FFFEC00) == 0x38BFE000:
            sz = insn >> 30
            opc_bit0 = (insn >> 12) & 1
            if (opc_bit0 == 0 and sz <= 2) or (opc_bit0 == 1 and sz <= 1):
                return insn, 'ldaprs_trap_left'

        # LSE Atomics: LDADD..LDUMIN (o3=0), SWP (o3=1, opc=0), CAS/CASB/CASH, CASP:
        if ((insn & 0x3F208C00) == 0x38200000 or
                (insn & 0x3F20FC00) == 0x38208000 or
                (insn & 0x3FA07C00) == 0x08A07C00 or
                (insn & 0xBFA07C00) == 0x08207C00):
            return insn, 'lse_atomic'

        return insn, None

    # Group D: SIMD & Scalar Floating-Point (op0 == x111 -> bits[27:25] == 111) — kernel-emulated telemetry
    if (insn & 0x0E000000) == 0x0E000000:
        return insn, _classify_simd_fp_insn(insn)

    return insn, None


def process_code_range(buf, start, size, rewrite=False, stats=None, dirty_blocks_256=None, rel_base=0):
    """Scan/rewrite 4-byte aligned instructions in buf[start:start+size]."""
    aligned_sz = size & ~3
    if aligned_sz <= 0:
        return 0
    end = start + aligned_sz
    changed = 0
    off = start
    pack_u32 = struct.pack_into
    for (insn,) in _ITER_U32_LE(memoryview(buf)[start:end]):
        # Fast skip when stats is None on pure SIMD/FP quadrant (never rewritten)
        if stats is None and (insn & 0x0E000000) == 0x0E000000:
            off += 4
            continue
        new_insn, kind = rewrite_arm64e_insn(insn)
        if kind and stats is not None:
            stats[kind] += 1
        if new_insn != insn:
            changed += 1
            if dirty_blocks_256 is not None:
                dirty_blocks_256.add((off - rel_base) & ~0xFF)
            if rewrite:
                pack_u32('<I', buf, off, new_insn)
        off += 4
    return changed


def iter_macho64_exec_ranges(buf, slice_off, slice_size):
    if slice_off + 32 > len(buf):
        return
    magic, cputype, _cpusubtype, _filetype, ncmds, sizeofcmds, _flags, _ = struct.unpack_from(
        '<IIIIIIII', buf, slice_off)
    if magic != MH_MAGIC_64 or cputype != CPU_TYPE_ARM64:
        return
    slice_end = min(slice_off + slice_size, len(buf))
    hdr_end = (32 + sizeofcmds + 3) & ~3
    cmd_off = slice_off + 32
    for _ in range(ncmds):
        if cmd_off + 8 > slice_end:
            break
        cmd, cmdsize = struct.unpack_from('<II', buf, cmd_off)
        if cmdsize < 8 or cmd_off + cmdsize > slice_end:
            break
        if cmd == LC_SEGMENT_64 and cmdsize >= 72:
            _segname, _vmaddr, _vmsize, fileoff, filesize, _maxprot, initprot, nsects, _segflags = struct.unpack_from(
                '<16sQQQQIIII', buf, cmd_off + 8)
            if (initprot & VM_PROT_EXECUTE) and filesize > 0:
                if nsects > 0:
                    sect_off = cmd_off + 72
                    for _s in range(nsects):
                        if sect_off + 80 > cmd_off + cmdsize:
                            break
                        s_offset, _s_align, _s_reloff, _s_nreloc, s_flags = struct.unpack_from(
                            '<IIIII', buf, sect_off + 48)
                        s_size, = struct.unpack_from('<Q', buf, sect_off + 40)
                        s_type = s_flags & 0xFF
                        if ((s_flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) and
                                s_type not in (0x1, 0xC) and s_size > 0 and s_offset > 0):
                            if s_offset + s_size <= slice_size:
                                yield slice_off + s_offset, s_size
                        sect_off += 80
                else:
                    # Sectionless executable segment: never scan Mach-O header + load commands!
                    code_rel = max(fileoff, hdr_end)
                    code_end = fileoff + filesize
                    if code_end > code_rel and code_end <= slice_size:
                        yield slice_off + code_rel, code_end - code_rel
        cmd_off += cmdsize


def _cs_hash(htype, data, hlen):
    if htype == CS_HASHTYPE_SHA1:
        return hashlib.sha1(data).digest()[:hlen]
    elif htype in (CS_HASHTYPE_SHA256, CS_HASHTYPE_SHA256_TRUNCATED):
        return hashlib.sha256(data).digest()[:hlen]
    elif htype == CS_HASHTYPE_SHA384:
        return hashlib.sha384(data).digest()[:hlen]
    return None


def _resolve_cd_limit(cs_buf, cd_rel, cd_len, version, hash_off, id_off, n_special, hash_size, code_limit):
    """Resolve effective 64-bit codeLimit, honoring codeLimit64 (including when codeLimit == 0)."""
    limit = code_limit
    if version >= 0x20300 and cd_len >= 64 and cd_rel + 64 <= len(cs_buf):
        first_payload = hash_off - n_special * hash_size
        scatter_ok = True
        if version >= CS_SUPPORTSSCATTER:
            sc_off, = struct.unpack_from('>I', cs_buf, cd_rel + 44)
            if 0 < sc_off < 64:
                scatter_ok = False
        if scatter_ok and first_payload >= 64 and (id_off == 0 or id_off >= 64):
            code_limit_64, = struct.unpack_from('>Q', cs_buf, cd_rel + 56)
            if code_limit_64 > 0:
                limit = code_limit_64
    return limit


def _build_cd_scatter_runs(cs_buf, cd_rel, cd_len, version):
    """Parse CS_CodeDirectory SC_Scatter array if present.

    Returns list of (base_page, page_count, slot_base) or None if un-scattered.
    """
    if version < CS_SUPPORTSSCATTER or cd_rel + 48 > len(cs_buf):
        return None
    scatter_off, = struct.unpack_from('>I', cs_buf, cd_rel + 44)
    if scatter_off == 0:
        return None
    pos = cd_rel + scatter_off
    end = min(cd_rel + cd_len, len(cs_buf))
    runs = []
    slot_cursor = 0
    while pos + 24 <= end:
        count, base, _target_off, _spare = struct.unpack_from('>IIQQ', cs_buf, pos)
        if count == 0:
            break
        runs.append((base, count, slot_cursor))
        slot_cursor += count
        pos += 24
    return runs if runs else None


def _page_to_cd_slot(p_idx, n_code, scatter_runs):
    """Translate file page index p_idx to CS_CodeDirectory hash slot index."""
    if scatter_runs is None:
        return p_idx if p_idx < n_code else None
    for base, count, slot_base in scatter_runs:
        if base <= p_idx < base + count:
            slot = slot_base + (p_idx - base)
            return slot if slot < n_code else None
    return None


def update_cs_superblob_hashes(file_buf, base_off, cs_off, cs_size, modified_ranges=None, slice_size=None):
    """Recompute CS_CodeDirectory code-page hashes in-place after instruction rewrites.

    Updates primary CSSLOT_CODEDIRECTORY (0) and all CSSLOT_ALTERNATE_CODEDIRECTORIES (0x1000..0x1005),
    preserving all Apple entitlements (XML + DER), Launch Constraints, TeamID, and flags,
    and respecting SC_Scatter tables, codeLimit64, partial tail pages, and unpaged (pageSize==0) blobs!
    """
    max_slice_end = len(file_buf) if slice_size is None else min(base_off + slice_size, len(file_buf))
    sb_pos = base_off + cs_off
    sb_end = min(sb_pos + cs_size, max_slice_end) if cs_size > 0 else max_slice_end
    if sb_pos + 12 > sb_end:
        return 0
    magic, sb_len, count = struct.unpack_from('>III', file_buf, sb_pos)
    if sb_len >= 12:
        sb_end = min(sb_end, sb_pos + sb_len)
    if magic == CSMAGIC_CODEDIRECTORY:
        cd_offsets = [sb_pos] if sb_pos + 44 <= sb_end else []
    elif magic == CSMAGIC_EMBEDDED_SIGNATURE:
        cd_offsets = []
        seen_cd = set()
        for i in range(count):
            if sb_pos + 20 + i * 8 > sb_end:
                break
            stype, soff = struct.unpack_from('>II', file_buf, sb_pos + 12 + i * 8)
            if stype == 0 or (0x1000 <= stype <= 0x1005):
                cd_pos = sb_pos + soff
                if cd_pos not in seen_cd and cd_pos + 44 <= sb_end:
                    cmag, = struct.unpack_from('>I', file_buf, cd_pos)
                    if cmag == CSMAGIC_CODEDIRECTORY:
                        seen_cd.add(cd_pos)
                        cd_offsets.append(cd_pos)
    else:
        return 0

    max_rel_limit = max_slice_end - base_off
    updated_pages = 0
    for cd_pos in cd_offsets:
        (_cmag, cd_len, version, _flags, hash_off, id_off,
         n_special, n_code, code_limit, hash_size, hash_type,
         _plat, page_shift) = struct.unpack_from('>IIIIIIIIIBBBB', file_buf, cd_pos)
        if hash_size == 0 or hash_off < 44 or n_code == 0 or page_shift > 30:
            continue
        cd_end = min(cd_pos + cd_len, sb_end)
        limit = _resolve_cd_limit(file_buf, cd_pos, cd_len, version, hash_off, id_off,
                                  n_special, hash_size, code_limit)
        limit = min(limit, max_rel_limit)
        if limit <= 0:
            continue
        page_size = limit if page_shift == 0 else (1 << page_shift)

        scatter_runs = _build_cd_scatter_runs(file_buf, cd_pos, cd_end - cd_pos, version)
        hashes_start = cd_pos + hash_off
        max_page = (limit + page_size - 1) // page_size
        if scatter_runs is None:
            page_iter = range(min(n_code, max_page))
        else:
            page_iter = (p for base, cnt, _sb in scatter_runs for p in range(base, min(base + cnt, max_page)))

        for p_idx in page_iter:
            slot_idx = _page_to_cd_slot(p_idx, n_code, scatter_runs)
            if slot_idx is None:
                continue
            p_rel_start = p_idx * page_size
            p_rel_end = min(p_rel_start + page_size, limit)
            if p_rel_start >= limit:
                continue
            if modified_ranges is not None:
                if not any(r0 < p_rel_end and r1 > p_rel_start for r0, r1 in modified_ranges):
                    continue
            page_bytes = memoryview(file_buf)[base_off + p_rel_start:base_off + p_rel_end]
            dg = _cs_hash(hash_type, page_bytes, hash_size)
            if dg is not None:
                slot_pos = hashes_start + slot_idx * hash_size
                if slot_pos + hash_size <= cd_end:
                    file_buf[slot_pos:slot_pos + hash_size] = dg
                    updated_pages += 1
    return updated_pages


def find_macho64_codesig(buf, slice_off, slice_size=None):
    slice_end = len(buf) if slice_size is None else min(slice_off + slice_size, len(buf))
    if slice_off + 32 > slice_end:
        return None
    magic, cputype, _cs, _ft, ncmds, _sz, _fl, _r = struct.unpack_from('<IIIIIIII', buf, slice_off)
    if magic != MH_MAGIC_64 or cputype != CPU_TYPE_ARM64:
        return None
    cmd_off = slice_off + 32
    for _ in range(ncmds):
        if cmd_off + 8 > slice_end:
            break
        cmd, cmdsize = struct.unpack_from('<II', buf, cmd_off)
        if cmdsize < 8 or cmd_off + cmdsize > slice_end:
            break
        if cmd == LC_CODE_SIGNATURE and cmdsize >= 16:
            dataoff, datasize = struct.unpack_from('<II', buf, cmd_off + 8)
            if slice_size is None or dataoff <= slice_size:
                return dataoff, datasize
        cmd_off += cmdsize
    return None


def process_macho_file(path, rewrite=False, stats=None):
    try:
        orig_stat = os.stat(path)
        with open(path, 'rb') as f:
            head = f.read(8)
            if len(head) < 8:
                return 0
            m_le, = struct.unpack('<I', head[:4])
            m_be, = struct.unpack('>I', head[:4])
            if m_le != MH_MAGIC_64 and m_be not in (FAT_MAGIC, FAT_MAGIC_64):
                return 0
            f.seek(0)
            buf = bytearray(f.read())
    except OSError:
        return 0

    slices = []
    m_le, = struct.unpack_from('<I', buf, 0)
    m_be, = struct.unpack_from('>I', buf, 0)
    if m_le == MH_MAGIC_64:
        slices.append((0, len(buf)))
    elif m_be == FAT_MAGIC:
        nfat, = struct.unpack_from('>I', buf, 4)
        for i in range(nfat):
            if 8 + (i + 1) * 20 > len(buf):
                break
            ct, _cs, off, sz, _al = struct.unpack_from('>IIIII', buf, 8 + i * 20)
            if ct == CPU_TYPE_ARM64 and off + sz <= len(buf):
                slices.append((off, sz))
    elif m_be == FAT_MAGIC_64:
        nfat, = struct.unpack_from('>I', buf, 4)
        for i in range(nfat):
            if 8 + (i + 1) * 32 > len(buf):
                break
            ct, _cs, off, sz, _al, _r = struct.unpack_from('>IIQQII', buf, 8 + i * 32)
            if ct == CPU_TYPE_ARM64 and off + sz <= len(buf):
                slices.append((off, sz))

    total_changed = 0
    for sl_off, sl_sz in slices:
        sl_changed = 0
        dirty_blocks_256 = set() if rewrite else None
        for code_off, code_sz in iter_macho64_exec_ranges(buf, sl_off, sl_sz):
            c = process_code_range(buf, code_off, code_sz, rewrite=rewrite, stats=stats,
                                   dirty_blocks_256=dirty_blocks_256, rel_base=sl_off)
            if c > 0:
                sl_changed += c
        if rewrite and sl_changed > 0:
            cs_info = find_macho64_codesig(buf, sl_off, sl_sz)
            if cs_info is not None:
                mod_ranges = [(b, b + 256) for b in sorted(dirty_blocks_256)]
                pages = update_cs_superblob_hashes(buf, sl_off, cs_info[0], cs_info[1], mod_ranges, sl_sz)
                if stats is not None:
                    stats['cs_pages_rehashed'] += pages
        total_changed += sl_changed

    if rewrite and total_changed > 0:
        with open(path, 'wb') as f:
            f.write(buf)
        # Preserve setuid/setgid (04755/02755) bits cleared by O_TRUNC/write() on Linux
        try:
            os.chmod(path, orig_stat.st_mode & 0o7777)
        except OSError:
            pass
    return total_changed


def _dsc_main_path_for(path):
    """Resolve primary dyld_shared_cache_arm64e path for either primary or subcache."""
    ab = os.path.abspath(path)
    base = re.sub(r'\.(?:[0-9]+|dyld\w+)$', '', ab)
    if base != ab and os.path.isfile(base):
        return base
    return ab


def _read_dsc_mappings(f, hdr):
    """Return list of (vmaddr, size, file_off, flags, initprot) for one DSC file."""
    mapping_off, mapping_cnt = struct.unpack_from('<II', hdr, 0x10)
    slide_map_off = slide_map_cnt = 0
    if mapping_off >= 0x140 and len(hdr) >= 0x140:
        slide_map_off, slide_map_cnt = struct.unpack_from('<II', hdr, 0x138)
    out = []
    if slide_map_off > 0 and slide_map_cnt == mapping_cnt:
        f.seek(slide_map_off)
        for _ in range(mapping_cnt):
            raw = f.read(56)
            if len(raw) < 56:
                break
            addr, size, file_off, _soff, _ssz, flags, _maxp, initp = struct.unpack('<QQQQQQII', raw)
            out.append((addr, size, file_off, flags, initp))
    else:
        f.seek(mapping_off)
        for _ in range(mapping_cnt):
            raw = f.read(32)
            if len(raw) < 32:
                break
            addr, size, file_off, _maxp, initp = struct.unpack('<QQQII', raw)
            out.append((addr, size, file_off, 0, initp))
    return out


_DSC_CODE_RANGES_CACHE = {}


def _collect_dsc_code_ranges(main_path):
    """Build {abs_file_path: [(file_off, size), ...]} of ONLY true instruction sections & stub islands.

    Never touches __TEXT,__const, __cstring, __swift5_*, __unwind_info, or Mach-O headers!
    """
    main_ab = os.path.abspath(main_path)
    if main_ab in _DSC_CODE_RANGES_CACHE:
        return _DSC_CODE_RANGES_CACHE[main_ab]

    cache_files = [main_ab]
    seen = {main_ab}
    for sub in sorted(glob.glob(main_ab + '.*')):
        ab = os.path.abspath(sub)
        if ab in seen or sub.endswith(('.symbols', '.map', '.json')):
            continue
        if os.path.isfile(ab) and not os.path.islink(ab):
            seen.add(ab)
            cache_files.append(ab)

    handles = []
    all_maps = []  # (vmaddr, size, path_ab, file_off, flags, initprot, fhandle)
    main_hdr = None
    try:
        for idx, p in enumerate(cache_files):
            f = open(p, 'rb')
            handles.append(f)
            hdr = f.read(512)
            if not hdr.startswith(b'dyld_v1  arm64e\x00'):
                continue
            if idx == 0:
                main_hdr = hdr
            for addr, sz, foff, flags, initp in _read_dsc_mappings(f, hdr):
                all_maps.append((addr, sz, p, foff, flags, initp, f))

        if main_hdr is None:
            return None

        def vm_to_file(vmaddr):
            for addr, sz, p, foff, _fl, _ip, fh in all_maps:
                if addr <= vmaddr < addr + sz:
                    return p, foff + (vmaddr - addr), sz - (vmaddr - addr), fh
            return None

        def read_vm(vmaddr, length):
            loc = vm_to_file(vmaddr)
            if loc is None:
                return None
            _p, foff, avail, fh = loc
            fh.seek(foff)
            return fh.read(min(length, avail))

        per_file = defaultdict(list)

        # 1. Include dedicated DSC stub-island mappings (DYLD_CACHE_MAPPING_TEXT_STUBS)
        for addr, sz, p, foff, flags, initp, _fh in all_maps:
            if (initp & VM_PROT_EXECUTE) and (flags & DYLD_CACHE_MAPPING_TEXT_STUBS) and sz > 0:
                per_file[p].append((foff, sz))

        # 2. Gather all Mach-O header VM addresses inside DSC (cached dylibs + dyldInCacheMH + branchPools)
        f0 = handles[0]
        mapping_off, = struct.unpack_from('<I', main_hdr, 0x10)
        mh_addrs = []
        seen_mh = set()

        def add_mh(va):
            if va and va not in seen_mh:
                seen_mh.add(va)
                mh_addrs.append(va)

        img_off, img_cnt = struct.unpack_from('<II', main_hdr, 0x18)
        if img_cnt == 0 and mapping_off >= 0x1C8 and len(main_hdr) >= 0x1C8:
            img_off, img_cnt = struct.unpack_from('<II', main_hdr, 0x1C0)
        if img_off and img_cnt:
            f0.seek(img_off)
            raw = f0.read(img_cnt * 32)
            for i in range(len(raw) // 32):
                addr, = struct.unpack_from('<Q', raw, i * 32)
                add_mh(addr)

        if mapping_off >= 0x98 and len(main_hdr) >= 0x98:
            it_off, it_cnt = struct.unpack_from('<QQ', main_hdr, 0x88)
            if it_off and it_cnt:
                f0.seek(it_off)
                raw = f0.read(it_cnt * 32)
                for i in range(len(raw) // 32):
                    load_addr, = struct.unpack_from('<Q', raw, i * 32 + 16)
                    add_mh(load_addr)

        if mapping_off >= 0x88 and len(main_hdr) >= 0x88:
            dyld_mh, = struct.unpack_from('<Q', main_hdr, 0x78)
            add_mh(dyld_mh)

        if mapping_off >= 0x78 and len(main_hdr) >= 0x78:
            bp_off, bp_cnt = struct.unpack_from('<II', main_hdr, 0x70)
            if bp_off and bp_cnt:
                f0.seek(bp_off)
                raw = f0.read(bp_cnt * 8)
                for i in range(len(raw) // 8):
                    bp_va, = struct.unpack_from('<Q', raw, i * 8)
                    add_mh(bp_va)

        # 3. Walk each Mach-O's executable instruction sections (using section_64.addr VM addresses)
        for mh_va in mh_addrs:
            mh = read_vm(mh_va, 4096)
            if not mh or len(mh) < 32:
                continue
            magic, cputype, _cs, _ft, ncmds, sizeofcmds, _fl, _r = struct.unpack_from('<IIIIIIII', mh, 0)
            if magic != MH_MAGIC_64 or cputype != CPU_TYPE_ARM64:
                continue
            if 32 + sizeofcmds > len(mh):
                mh = read_vm(mh_va, 32 + sizeofcmds)
                if not mh or len(mh) < 32 + sizeofcmds:
                    continue
            pos = 32
            for _ in range(ncmds):
                if pos + 8 > len(mh):
                    break
                cmd, cmdsize = struct.unpack_from('<II', mh, pos)
                if cmdsize < 8 or pos + cmdsize > len(mh):
                    break
                if cmd == LC_SEGMENT_64 and cmdsize >= 72:
                    _sn, seg_vm, seg_vmsz, _foff, _fsz, _mp, initp, nsects, _sf = struct.unpack_from(
                        '<16sQQQQIIII', mh, pos + 8)
                    if (initp & VM_PROT_EXECUTE) and seg_vmsz > 0:
                        if nsects > 0:
                            soff = pos + 72
                            for _s in range(nsects):
                                if soff + 80 > pos + cmdsize:
                                    break
                                s_addr, s_size = struct.unpack_from('<QQ', mh, soff + 32)
                                s_flags, = struct.unpack_from('<I', mh, soff + 64)
                                if (s_flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) and s_size > 0:
                                    loc = vm_to_file(s_addr)
                                    if loc is not None:
                                        p_ab, f_off, avail, _ = loc
                                        per_file[p_ab].append((f_off, min(s_size, avail)))
                                soff += 80
                        else:
                            # Branch-pool Mach-O (__TEXT with 0 sections): skip header + load commands
                            hdr_end = (32 + sizeofcmds + 3) & ~3
                            if seg_vmsz > hdr_end:
                                loc = vm_to_file(seg_vm + hdr_end)
                                if loc is not None:
                                    p_ab, f_off, avail, _ = loc
                                    per_file[p_ab].append((f_off, min(seg_vmsz - hdr_end, avail)))
                pos += cmdsize

        # Coalesce overlapping / adjacent ranges per file so every instruction is visited at most once
        merged = {}
        for p_ab, rngs in per_file.items():
            if not rngs:
                merged[p_ab] = []
                continue
            sr = sorted(rngs)
            cur_s, cur_e = sr[0][0], sr[0][0] + sr[0][1]
            out_r = []
            for s, sz in sr[1:]:
                e = s + sz
                if s <= cur_e:
                    if e > cur_e:
                        cur_e = e
                else:
                    out_r.append((cur_s, cur_e - cur_s))
                    cur_s, cur_e = s, e
            out_r.append((cur_s, cur_e - cur_s))
            merged[p_ab] = out_r

        if not any(merged.values()):
            return None
        _DSC_CODE_RANGES_CACHE[main_ab] = merged
        return merged
    finally:
        for fh in handles:
            fh.close()


def process_dsc_file(path, rewrite=False, stats=None):
    """Rewrite instruction sections of a dyld_shared_cache_arm64e (or subcache)
    and update its embedded CS_CodeDirectory page hashes (at header offset 0x28) in-place."""
    path_ab = os.path.abspath(path)
    main_ab = _dsc_main_path_for(path_ab)
    code_map = _collect_dsc_code_ranges(main_ab)

    with open(path_ab, 'r+b' if rewrite else 'rb') as f:
        hdr = f.read(512)
        if not hdr.startswith(b'dyld_v1  arm64e\x00'):
            return 0
        # dyld_cache_header: 0x20 is dyldBaseAddress; 0x28 is codeSignatureOffset, 0x30 is codeSignatureSize!
        cs_offset, cs_size = struct.unpack_from('<QQ', hdr, 0x28)
        f.seek(0, os.SEEK_END)
        file_len = f.tell()
        if cs_offset > 0 and cs_size == 0 and cs_offset < file_len:
            cs_size = file_len - cs_offset

        if code_map is not None:
            ranges = code_map.get(path_ab, [])
        else:
            # Fallback only for synthetic headerless test blobs with 0 images: only TEXT_STUBS or RX
            ranges = [(foff, sz) for _a, sz, foff, _fl, ip in _read_dsc_mappings(f, hdr)
                      if (ip & VM_PROT_EXECUTE) and sz > 0]

        total_changed = 0
        CHUNK = 16 << 20
        dirty_blocks_256 = set()
        for file_off, size in ranges:
            pos = 0
            while pos < size:
                n = min(CHUNK, size - pos)
                f.seek(file_off + pos)
                buf = bytearray(f.read(n))
                chunk_dirty = set() if rewrite else None
                c = process_code_range(buf, 0, len(buf), rewrite=rewrite, stats=stats,
                                       dirty_blocks_256=chunk_dirty, rel_base=0)
                if rewrite and c > 0:
                    f.seek(file_off + pos)
                    f.write(buf)
                    base_p = file_off + pos
                    for b in chunk_dirty:
                        dirty_blocks_256.add((base_p + b) & ~0xFF)
                total_changed += c
                pos += n

        # Rehash modified pages in the DSC's embedded CodeSignature blob (honoring scatterOffset!)
        if rewrite and total_changed > 0 and 0 < cs_offset < file_len and cs_size >= 12:
            f.seek(cs_offset)
            cs_buf = bytearray(f.read(min(cs_size, file_len - cs_offset)))
            if len(cs_buf) >= 12:
                magic, sblen, count = struct.unpack_from('>III', cs_buf, 0)
                sb_end = min(len(cs_buf), sblen) if sblen >= 12 else len(cs_buf)
                cd_rels = []
                seen_cd = set()
                if magic == CSMAGIC_CODEDIRECTORY and 44 <= sb_end:
                    cd_rels.append(0)
                elif magic == CSMAGIC_EMBEDDED_SIGNATURE:
                    for i in range(count):
                        if 20 + i * 8 > sb_end:
                            break
                        st, so = struct.unpack_from('>II', cs_buf, 12 + i * 8)
                        if st == 0 or (0x1000 <= st <= 0x1005):
                            if so not in seen_cd and so + 44 <= sb_end and struct.unpack_from('>I', cs_buf, so)[0] == CSMAGIC_CODEDIRECTORY:
                                seen_cd.add(so)
                                cd_rels.append(so)
                cs_dirty = False
                for cd_rel in cd_rels:
                    (_cm, cd_len, ver, _fl, hash_off, id_off,
                     n_spec, n_code, code_lim, hsz, htype, _pl, pshift) = struct.unpack_from(
                        '>IIIIIIIIIBBBB', cs_buf, cd_rel)
                    if hsz == 0 or hash_off < 44 or n_code == 0 or pshift > 30:
                        continue
                    cd_end = min(cd_rel + cd_len, sb_end)
                    lim = _resolve_cd_limit(cs_buf, cd_rel, cd_len, ver, hash_off, id_off,
                                            n_spec, hsz, code_lim)
                    lim = min(lim, file_len)
                    if lim <= 0:
                        continue
                    psize = lim if pshift == 0 else (1 << pshift)
                    scatter_runs = _build_cd_scatter_runs(cs_buf, cd_rel, cd_end - cd_rel, ver)
                    dirty_cd_pages = {pg // psize for pg in dirty_blocks_256 if pg < lim}
                    for p_idx in sorted(dirty_cd_pages):
                        slot_idx = _page_to_cd_slot(p_idx, n_code, scatter_runs)
                        if slot_idx is None:
                            continue
                        p_off = p_idx * psize
                        p_len = min(psize, lim - p_off)
                        f.seek(p_off)
                        dg = _cs_hash(htype, f.read(p_len), hsz)
                        if dg is not None:
                            s_pos = cd_rel + hash_off + slot_idx * hsz
                            if s_pos + hsz <= cd_end:
                                cs_buf[s_pos:s_pos + hsz] = dg
                                cs_dirty = True
                                if stats is not None:
                                    stats['dsc_cs_pages_rehashed'] += 1
                if cs_dirty:
                    f.seek(cs_offset)
                    f.write(cs_buf)
        return total_changed


def run_selftest():
    cases = [
        ("RETAA -> RET", 0xD65F0BFF, 0xD65F03C0, "retab"),
        ("RETAB -> RET", 0xD65F0FFF, 0xD65F03C0, "retab"),
        ("ERETAA -> ERET", 0xD69F0BFF, 0xD69F03E0, "eretab"),
        ("BLRAA X8, X9 -> BLR X8", 0xD73F0800 | (8 << 5) | 9, 0xD63F0000 | (8 << 5), "blraa"),
        ("BLRAB X16, X17 -> BLR X16", 0xD73F0C00 | (16 << 5) | 17, 0xD63F0000 | (16 << 5), "blraa"),
        ("BLRAAZ X2 -> BLR X2", 0xD63F081F | (2 << 5), 0xD63F0000 | (2 << 5), "blraaz"),
        ("BLRABZ X2 -> BLR X2", 0xD63F0C1F | (2 << 5), 0xD63F0000 | (2 << 5), "blraaz"),
        ("BRAA X16, X17 -> BR X16", 0xD71F0800 | (16 << 5) | 17, 0xD61F0000 | (16 << 5), "braa"),
        ("BRABZ X3 -> BR X3", 0xD61F0C1F | (3 << 5), 0xD61F0000 | (3 << 5), "braaz"),
        ("PACIA X0, X1 -> NOP", 0xDAC10000 | (1 << 5) | 0, 0xD503201F, "pac_aut"),
        ("PACDA X16, X17 -> NOP", 0xDAC10800 | (17 << 5) | 16, 0xD503201F, "pac_aut"),
        ("AUTIA X0, X1 -> NOP", 0xDAC11000 | (1 << 5) | 0, 0xD503201F, "pac_aut"),
        ("AUTDZB X5 -> NOP", 0xDAC13FE0 | 5, 0xD503201F, "pac_aut"),
        ("XPACI X0 -> NOP", 0xDAC143E0 | 0, 0xD503201F, "xpaci"),
        ("XPACI XZR -> NOP", 0xDAC143FF, 0xD503201F, "xpaci"),
        ("XPACD X16 -> NOP (preserve TBI & negative sentinels)", 0xDAC147E0 | 16, 0xD503201F, "xpacd"),
        ("XPACD XZR -> NOP", 0xDAC147FF, 0xD503201F, "xpacd"),
        ("LDAPRB W2, [X3] -> LDARB W2, [X3]", 0x38BFC000 | (3 << 5) | 2, 0x08DFFC00 | (3 << 5) | 2, "ldapr_ldar"),
        ("LDAPR X8, [X9] -> LDAR X8, [X9]", 0xF8BFC000 | (9 << 5) | 8, 0xC8DFFC00 | (9 << 5) | 8, "ldapr_ldar"),
        ("LDAPRSW X4, [X5] -> trap", 0xB8BFE000 | (5 << 5) | 4, 0xB8BFE000 | (5 << 5) | 4, "ldaprs_trap_left"),
        ("LDAPRSB W4, [X5] -> trap", 0x38BFF000 | (5 << 5) | 4, 0x38BFF000 | (5 << 5) | 4, "ldaprs_trap_left"),
        ("LDURSB W4, [X5, #-4] -> native ARMv8.0", 0x38DFC000 | (5 << 5) | 4, 0x38DFC000 | (5 << 5) | 4, None),
        ("LDRAA X0, [X1, #16] -> LDR X0, [X1, #16]",
         0xF8200400 | (2 << 12) | (1 << 5) | 0,
         0xF9400000 | (2 << 10) | (1 << 5) | 0, "ldraa_ldr"),
        ("LDRAB X2, [X3, #-8] -> LDUR X2, [X3, #-8]",
         0xF8A00400 | (1 << 22) | (511 << 12) | (3 << 5) | 2,
         0xF8400000 | ((-8 & 0x1FF) << 12) | (3 << 5) | 2, "ldraa_ldur"),
        ("LDRAA writeback -> trap",
         0xF8200C00 | (1 << 12) | (1 << 5) | 0,
         0xF8200C00 | (1 << 12) | (1 << 5) | 0, "ldraa_trap_left"),
        ("DC CVAP X3 -> DC CVAC X3", 0xD50B7C23, 0xD50B7A23, "dc_cvap_cvac"),
        ("DC CVADP X7 -> DC CVAC X7", 0xD50B7D27, 0xD50B7A27, "dc_cvap_cvac"),
        ("SB -> ISB", 0xD50330FF, 0xD5033FDF, "sb_isb"),
        ("SSBB (DSB #0) -> native ARMv8.0 DSB SY", 0xD503309F, 0xD503309F, None),
        ("PSSBB (DSB #4) -> native ARMv8.0 DSB SY", 0xD503349F, 0xD503349F, None),
        ("CSDB -> native ARMv8.0 hint", 0xD503229F, 0xD503229F, None),
        ("XPACLRI -> NOP", 0xD50320FF, 0xD503201F, "xpaclri"),
        ("PACIBZ hint -> untouched", 0xD503237F, 0xD503237F, "pac_hint_nop"),
        ("SDOT V0.4S, V1.16B, V2.16B -> feat_dotprod", 0x4E829420, 0x4E829420, "feat_dotprod"),
        ("UDOT V0.2S, V1.8B, V2.4B[2] -> feat_dotprod", 0x2F82E820, 0x2F82E820, "feat_dotprod"),
        ("FCADD V0.4H, V1.4H, V2.4H, #90 -> feat_fcma", 0x2E42E420, 0x2E42E420, "feat_fcma"),
        ("FCMLA V0.4H, V1.4H, V2.4H, #0 -> feat_fcma", 0x2E42C420, 0x2E42C420, "feat_fcma"),
        ("FCMLA V0.4S, V1.4S, V2.S[0], #90 -> feat_fcma", 0x6F823020, 0x6F823020, "feat_fcma"),
        ("FMLAL V0.2S, V1.2H, V2.2H -> feat_fhm", 0x0E22EC20, 0x0E22EC20, "feat_fhm"),
        ("FMLSL V0.4S, V1.4H, V2.4H -> feat_fhm", 0x4EA2EC20, 0x4EA2EC20, "feat_fhm"),
        ("FMLAL2 V0.2S, V1.4H, V2.4H -> feat_fhm", 0x2E22CC20, 0x2E22CC20, "feat_fhm"),
        ("FMLSL2 V0.4S, V1.4H, V2.H[0] -> feat_fhm", 0x6F82C020, 0x6F82C020, "feat_fhm"),
        ("FMLAL V0.2S, V1.2H, V2.H[0] -> feat_fhm", 0x0F820020, 0x0F820020, "feat_fhm"),
        ("FACGE V0.2S, V1.2S, V2.2S -> native ARMv8.0", 0x2E22EC20, 0x2E22EC20, None),
        ("FACGE V0.8H, V1.8H, V2.8H -> feat_fp16", 0x6E422C20, 0x6E422C20, "feat_fp16"),
        ("FADDP V0.8H, V1.8H, V2.8H -> feat_fp16", 0x6E421420, 0x6E421420, "feat_fp16"),
        ("FADD V0.4H, V1.4H, V2.4H -> feat_fp16", 0x0E421420, 0x0E421420, "feat_fp16"),
        ("FADD H0, H1, H2 -> feat_fp16", 0x1EE22820, 0x1EE22820, "feat_fp16"),
        ("FCVT S0, H1 -> native ARMv8.0", 0x1EE24020, 0x1EE24020, None),
        ("SQRDMLAH V0.4H, V1.4H, V2.4H -> feat_rdm", 0x2E428420, 0x2E428420, "feat_rdm"),
        ("FJCVTZS W0, D1 -> fjcvtzs", 0x1E7E0020, 0x1E7E0020, "fjcvtzs"),
        ("CASAL X0, X1, [X2] -> lse_atomic", 0xC8E0FC41, 0xC8E0FC41, "lse_atomic"),
        ("CFINV -> feat_flagm", 0xD500401F, 0xD500401F, "feat_flagm"),
        ("XAFLAG -> feat_flagm", 0xD500403F, 0xD500403F, "feat_flagm"),
        ("AXFLAG -> feat_flagm", 0xD500405F, 0xD500405F, "feat_flagm"),
    ]
    for name, inp, exp, exp_kind in cases:
        got, kind = rewrite_arm64e_insn(inp)
        assert got == exp and kind == exp_kind, (
            f"FAIL {name}: in=0x{inp:08x} got=(0x{got:08x}, {kind}) != exp=(0x{exp:08x}, {exp_kind})")

    # Synthetic DSC + subcache + scatter CodeDirectory + __TEXT,__const protection test
    with tempfile.TemporaryDirectory() as td:
        dsc_main = os.path.join(td, "dyld_shared_cache_arm64e")
        # Build a 2-page (2 * 4096 = 8192 bytes) DSC with dyldBaseAddress=0x180000000 at 0x20,
        # codeSignatureOffset=8192 at 0x28, 1 image whose __text is at 0x1000..0x1010 and __const
        # at 0x1010..0x1020 containing false-positive LDRAA word 0xF8202420 (must NOT be touched!)
        buf = bytearray(8192)
        buf[0:16] = b'dyld_v1  arm64e\x00'
        mapping_off = 0x200
        img_off = 0x280
        mh_foff = 0x400
        vm_base = 0x180000000
        # CodeSignature with scatterOffset skipping page 0 and hashing page 1 into slot 0
        scatter_off = 64
        hash_off = 64 + 48
        cd_len = hash_off + 32
        sb_len = 20 + cd_len
        struct.pack_into('<IIII', buf, 0x10, mapping_off, 1, img_off, 1)
        struct.pack_into('<Q', buf, 0x20, vm_base)             # dyldBaseAddress at 0x20
        struct.pack_into('<QQ', buf, 0x28, 8192, sb_len)       # codeSignatureOffset/Size at 0x28!
        struct.pack_into('<QQQII', buf, mapping_off, vm_base, 8192, 0, 5, VM_PROT_EXECUTE | 1)
        struct.pack_into('<QQQII', buf, img_off, vm_base + mh_foff, 0, 0, 0x2A0, 0)
        buf[0x2A0:0x2AD] = b'/usr/lib/t.dy'

        # Mach-O header at mh_foff with 1 LC_SEGMENT_64 (__TEXT) containing __text and __const
        seg_cmd_sz = 72 + 2 * 80
        struct.pack_into('<8I', buf, mh_foff, MH_MAGIC_64, CPU_TYPE_ARM64, 2, 6, 1, seg_cmd_sz, 0, 0)
        struct.pack_into('<II16sQQQQIIII', buf, mh_foff + 32,
                         LC_SEGMENT_64, seg_cmd_sz, b'__TEXT'.ljust(16, b'\x00'),
                         vm_base + mh_foff, 0x1C00, mh_foff, 0x1C00, 5, VM_PROT_EXECUTE | 1, 2, 0)
        # Section 0: __text at file offset 0x1000 (page 1), size 8 (RETAA + LDRAA)
        s0 = mh_foff + 32 + 72
        struct.pack_into('<16s16sQQIIIIIIII', buf, s0,
                         b'__text'.ljust(16, b'\x00'), b'__TEXT'.ljust(16, b'\x00'),
                         vm_base + 0x1000, 8, 0x1000, 2, 0, 0, S_ATTR_PURE_INSTRUCTIONS, 0, 0, 0)
        # Section 1: __const at file offset 0x1008, size 8 (non-instruction data with 0xF8202420!)
        s1 = s0 + 80
        struct.pack_into('<16s16sQQIIIIIIII', buf, s1,
                         b'__const'.ljust(16, b'\x00'), b'__TEXT'.ljust(16, b'\x00'),
                         vm_base + 0x1008, 8, 0x1008, 2, 0, 0, 0, 0, 0, 0)
        false_pos_ldraa = 0xF8200400 | (2 << 12) | (1 << 5) | 0
        struct.pack_into('<IIII', buf, 0x1000, 0xD65F0BFF, false_pos_ldraa, false_pos_ldraa, 0xD65F0BFF)

        cs = bytearray(sb_len)
        struct.pack_into('>IIIII', cs, 0, CSMAGIC_EMBEDDED_SIGNATURE, sb_len, 1, 0, 20)
        cd = 20
        struct.pack_into('>IIIIIIIIIBBBB', cs, cd,
                         CSMAGIC_CODEDIRECTORY, cd_len, 0x20400, 0, hash_off, 0,
                         0, 1, 8192, 32, CS_HASHTYPE_SHA256, 0, 12)
        struct.pack_into('>I', cs, cd + 44, scatter_off)
        struct.pack_into('>Q', cs, cd + 56, 8192)
        # Scatter run 0: base=1, count=1 -> slot 0; terminator count=0
        struct.pack_into('>IIQQ', cs, cd + scatter_off, 1, 1, 4096, 0)
        struct.pack_into('>IIQQ', cs, cd + scatter_off + 24, 0, 0, 0, 0)

        with open(dsc_main, 'wb') as f:
            f.write(buf + cs)
        _DSC_CODE_RANGES_CACHE.clear()
        st = Counter()
        changed = process_dsc_file(dsc_main, rewrite=True, stats=st)
        assert changed == 2 and st['dsc_cs_pages_rehashed'] == 1, f"DSC selftest bad stats: {changed}, {st}"
        out_dsc = open(dsc_main, 'rb').read()
        w0, w1, w2, w3 = struct.unpack_from('<IIII', out_dsc, 0x1000)
        assert w0 == INSN_RET and w1 == (0xF9400000 | (2 << 10) | (1 << 5)), "DSC __text not rewritten"
        assert w2 == false_pos_ldraa and w3 == 0xD65F0BFF, "DSC __const corrupted by false-positive rewrite!"
        exp_hash = hashlib.sha256(out_dsc[4096:8192]).digest()
        got_hash = out_dsc[8192 + 20 + hash_off:8192 + 20 + hash_off + 32]
        assert got_hash == exp_hash, "DSC scattered CS_CodeDirectory hash mismatch!"

    print(f"SELFTEST OK ({len(cases)} instruction encodings + DSC section-only & scattered CodeDirectory verified)")


def _collect_target_paths(targets):
    paths = []
    seen = set()
    for target in targets:
        if os.path.isfile(target):
            ab = os.path.abspath(target)
            if ab not in seen:
                seen.add(ab)
                paths.append(target)
        else:
            for root, _dirs, fnames in os.walk(target):
                for fn in sorted(fnames):
                    p = os.path.join(root, fn)
                    if not os.path.islink(p):
                        ab = os.path.abspath(p)
                        if ab not in seen:
                            seen.add(ab)
                            paths.append(p)
    return paths


def main():
    if len(sys.argv) < 2 or sys.argv[1] == '--selftest':
        run_selftest()
        return
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    mode = sys.argv[1]
    targets = sys.argv[2:]
    stats = Counter()
    _DSC_CODE_RANGES_CACHE.clear()
    if mode in ('--audit', '--rewrite'):
        rewrite = (mode == '--rewrite')
        files = 0
        rewritten_insns = 0
        paths = _collect_target_paths(targets)
        for p in paths:
            bn = os.path.basename(p)
            if bn.startswith('dyld_shared_cache_arm64e') and not bn.endswith(('.symbols', '.map', '.json')):
                c = process_dsc_file(p, rewrite=rewrite, stats=stats)
            else:
                c = process_macho_file(p, rewrite=rewrite, stats=stats)
            if c:
                files += 1
                rewritten_insns += c
        if rewrite:
            print(f"--rewrite: {files} files touched, {rewritten_insns} PAC/non-ARMv8.0 instructions rewritten")
            for k, v in sorted(stats.items()):
                print(f"  {k:24s}: {v}")
        else:
            fatal_cnt = sum(stats[k] for k in UNREWRITTEN_FATAL_KINDS)
            opt_cnt = sum(stats[k] for k in UNREWRITTEN_OPT_KINDS)
            emu_cnt = sum(stats[k] for k in KERNEL_EMULATED_KINDS)
            nop_cnt = sum(stats[k] for k in HARDWARE_NOP_KINDS)
            print(
                f"--audit: {len(paths)} files scanned ({files} with unrewritten insns), "
                f"{fatal_cnt} unrewritten PAC (FATAL), "
                f"{emu_cnt} kernel-emulated (safe via sleh.c)"
            )
            print(f"  unrewritten PAC (FATAL)         : {fatal_cnt}")
            for k in UNREWRITTEN_FATAL_KINDS:
                if stats[k]:
                    print(f"    {k:26s}: {stats[k]}")
            if opt_cnt:
                print(f"  unrewritten ARMv8.2/v8.3 (opt)  : {opt_cnt}")
                for k in UNREWRITTEN_OPT_KINDS:
                    if stats[k]:
                        print(f"    {k:26s}: {stats[k]}")
            print(f"  kernel-emulated (safe via sleh.c): {emu_cnt}")
            for k in KERNEL_EMULATED_KINDS:
                if stats[k]:
                    print(f"    {k:26s}: {stats[k]}")
            if nop_cnt:
                print(f"  hardware NOP hints (ARMv8.0)    : {nop_cnt}")
                for k in HARDWARE_NOP_KINDS:
                    if stats[k]:
                        print(f"    {k:26s}: {stats[k]}")
            if fatal_cnt > 0:
                sys.exit(1)
    elif mode == '--rewrite-dsc':
        rewritten = 0
        for target in targets:
            for p in sorted(glob.glob(target + '*')):
                if os.path.isfile(p) and not os.path.islink(p) and not p.endswith(('.symbols', '.map', '.json')):
                    c = process_dsc_file(p, rewrite=True, stats=stats)
                    print(f"  {os.path.basename(p)}: {c} instructions rewritten")
                    rewritten += c
        print(f"Total DSC instructions rewritten: {rewritten}")
        for k, v in sorted(stats.items()):
            print(f"  {k:24s}: {v}")
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
