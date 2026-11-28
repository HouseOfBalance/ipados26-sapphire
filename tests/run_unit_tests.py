#!/usr/bin/env python3
"""Comprehensive Automated Unit Test Suite for ipados26-sapphire.

Zero external dependencies (pure Python 3 standard library `unittest`) so it
runs out-of-the-box on Windows, Linux, and macOS CI runners (GitHub Actions,
Codemagic, and CircleCI).

Covers:
  1. `tools/ipados26_depac.py` ARMv8.3 -> ARMv8.0 instruction rewriter,
     TBI-preserving `XPACD` bitmask encoding, section-only safety (`__DATA` &
     `__TEXT,__const` untouched), and `CS_CodeDirectory` SHA-256 page rehashing.
  2. `tools/ipados26_mkadt.py` Apple DeviceTree (`.adt`) generation across
     16K/4K AMP/Cellular/Highmem configurations and Sapphire SM6225 physical
     memory carveout non-overlap invariants.
  3. `tools/ipados26_ca_metallib.py` `MTLB` + `BC\\xc0\\xde` bitcode container
     extractor, deduplicator, and merged metallib builder (plus DSC slide/trie
     helpers in `tools/ipados26_dsc_offsets.py`).
  4. `xnu/patches-ipados26/zz-*.patch` unified diff hunk line-count integrity
     and `-mgeneral-regs-only` floating-point type enforcement in `osfmk/arm64/sleh.c`.
  5. Syntax compilation (`py_compile`) across all `tools/ipados26*.py` scripts
     and CI workflow toolchain standardization invariants.
"""
import sys

sys.dont_write_bytecode = True

import glob
import hashlib
import os
import py_compile
import re
import struct
import subprocess
import tempfile
import unittest
import warnings

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
TOOLS_DIR = os.path.join(REPO_ROOT, 'tools')
PATCHES_DIR = os.path.join(REPO_ROOT, 'xnu', 'patches-ipados26')

if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import adt  # noqa: E402
import ipados26_ca_metallib  # noqa: E402
import ipados26_depac  # noqa: E402
import ipados26_dsc_offsets  # noqa: E402
import ipados26_mkadt  # noqa: E402


def _clean_subprocess_env():
    env = os.environ.copy()
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    return env


def decode_arm64_bitmask_imm64(n_bit, imms, immr):
    """Reference ARMv8-A `DecodeBitMasks(immN, imms, immr, immediate=TRUE)` for 64-bit."""
    if n_bit == 1:
        esize = 64
    else:
        not_imms = (~imms) & 0x3F
        if not_imms == 0:
            raise ValueError('Reserved bitmask immediate encoding')
        esize = 1 << (not_imms.bit_length() - 1)
    levels = esize - 1
    s = imms & levels
    r = immr & levels
    if s == levels:
        raise ValueError('All-ones bitmask immediate is reserved')
    welem = (1 << (s + 1)) - 1
    if r != 0:
        welem = ((welem >> r) | (welem << (esize - r))) & ((1 << esize) - 1)
    result = 0
    for shift in range(0, 64, esize):
        result |= welem << shift
    return result


def build_synthetic_arm64e_macho(text_insns, const_words, data_words):
    """Construct a valid 3-page 64-bit arm64e Mach-O executable with SHA-256 CS_CodeDirectory.

    Layout:
      - Page 0 [0x0000..0x1000): mach_header_64 + load commands + __TEXT,__text + __TEXT,__const
      - Page 1 [0x1000..0x2000): __DATA,__data (writable non-executable segment)
      - Page 2 [0x2000..end):    __LINKEDIT containing CSMAGIC_EMBEDDED_SIGNATURE + CodeDirectory
    """
    MH_EXECUTE = 2
    CPU_SUBTYPE_ARM64E = 0x80000002
    VM_PROT_READ = 0x01
    VM_PROT_WRITE = 0x02
    VM_PROT_EXECUTE = 0x04

    text_seg_cmd_sz = 72 + 2 * 80   # __TEXT with __text and __const
    data_seg_cmd_sz = 72 + 1 * 80   # __DATA with __data
    link_seg_cmd_sz = 72            # __LINKEDIT (0 sections)
    cs_cmd_sz = 16                  # LC_CODE_SIGNATURE
    sizeofcmds = text_seg_cmd_sz + data_seg_cmd_sz + link_seg_cmd_sz + cs_cmd_sz

    text_sect_off = 0x200
    text_bytes = struct.pack(f'<{len(text_insns)}I', *text_insns)
    const_sect_off = text_sect_off + ((len(text_bytes) + 15) & ~15)
    const_bytes = struct.pack(f'<{len(const_words)}I', *const_words)
    assert const_sect_off + len(const_bytes) <= 0x1000

    data_sect_off = 0x1000
    data_bytes = struct.pack(f'<{len(data_words)}I', *data_words)
    assert len(data_bytes) <= 0x1000

    cs_file_off = 0x2000
    code_limit = 0x2000
    page_shift = 12
    page_size = 1 << page_shift
    n_code_slots = code_limit // page_size  # 2 pages (0x0000, 0x1000)
    n_special_slots = 2
    hash_size = 32

    ident = b'com.sapphire.unit-test\x00'
    cd_hdr_len = 64  # version 0x20400 header size
    id_off = cd_hdr_len
    special_off = (id_off + len(ident) + 7) & ~7
    hash_off = special_off + n_special_slots * hash_size
    cd_len = hash_off + n_code_slots * hash_size
    sb_len = 20 + cd_len
    linkedit_vmsize = (sb_len + 0xFFF) & ~0xFFF

    buf = bytearray(cs_file_off + sb_len)

    # mach_header_64 (32 bytes)
    struct.pack_into(
        '<8I', buf, 0,
        ipados26_depac.MH_MAGIC_64,
        ipados26_depac.CPU_TYPE_ARM64,
        CPU_SUBTYPE_ARM64E,
        MH_EXECUTE,
        4,  # ncmds
        sizeofcmds,
        0x00200085,  # MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL | MH_PIE
        0,
    )

    pos = 32
    vm_text = 0x100000000
    vm_data = 0x100001000
    vm_link = 0x100002000

    # 1. LC_SEGMENT_64 __TEXT
    struct.pack_into(
        '<II16sQQQQIIII', buf, pos,
        ipados26_depac.LC_SEGMENT_64, text_seg_cmd_sz,
        b'__TEXT'.ljust(16, b'\x00'),
        vm_text, 0x1000, 0, 0x1000,
        VM_PROT_READ | VM_PROT_EXECUTE, VM_PROT_READ | VM_PROT_EXECUTE,
        2, 0,
    )
    # Section 0: __TEXT,__text (executable instructions)
    struct.pack_into(
        '<16s16sQQIIIIIIII', buf, pos + 72,
        b'__text'.ljust(16, b'\x00'), b'__TEXT'.ljust(16, b'\x00'),
        vm_text + text_sect_off, len(text_bytes),
        text_sect_off, 2, 0, 0,
        ipados26_depac.S_ATTR_PURE_INSTRUCTIONS | ipados26_depac.S_ATTR_SOME_INSTRUCTIONS,
        0, 0, 0,
    )
    # Section 1: __TEXT,__const (non-instruction data inside executable __TEXT segment!)
    struct.pack_into(
        '<16s16sQQIIIIIIII', buf, pos + 72 + 80,
        b'__const'.ljust(16, b'\x00'), b'__TEXT'.ljust(16, b'\x00'),
        vm_text + const_sect_off, len(const_bytes),
        const_sect_off, 2, 0, 0,
        0,  # NO S_ATTR_*_INSTRUCTIONS flags!
        0, 0, 0,
    )
    pos += text_seg_cmd_sz

    # 2. LC_SEGMENT_64 __DATA
    struct.pack_into(
        '<II16sQQQQIIII', buf, pos,
        ipados26_depac.LC_SEGMENT_64, data_seg_cmd_sz,
        b'__DATA'.ljust(16, b'\x00'),
        vm_data, 0x1000, data_sect_off, 0x1000,
        VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE,
        1, 0,
    )
    struct.pack_into(
        '<16s16sQQIIIIIIII', buf, pos + 72,
        b'__data'.ljust(16, b'\x00'), b'__DATA'.ljust(16, b'\x00'),
        vm_data, len(data_bytes),
        data_sect_off, 3, 0, 0,
        0, 0, 0, 0,
    )
    pos += data_seg_cmd_sz

    # 3. LC_SEGMENT_64 __LINKEDIT
    struct.pack_into(
        '<II16sQQQQIIII', buf, pos,
        ipados26_depac.LC_SEGMENT_64, link_seg_cmd_sz,
        b'__LINKEDIT'.ljust(16, b'\x00'),
        vm_link, linkedit_vmsize, cs_file_off, sb_len,
        VM_PROT_READ, VM_PROT_READ,
        0, 0,
    )
    pos += link_seg_cmd_sz

    # 4. LC_CODE_SIGNATURE
    struct.pack_into('<IIII', buf, pos, ipados26_depac.LC_CODE_SIGNATURE, cs_cmd_sz, cs_file_off, sb_len)
    pos += cs_cmd_sz
    assert pos == 32 + sizeofcmds

    # Populate section payloads
    buf[text_sect_off:text_sect_off + len(text_bytes)] = text_bytes
    buf[const_sect_off:const_sect_off + len(const_bytes)] = const_bytes
    buf[data_sect_off:data_sect_off + len(data_bytes)] = data_bytes

    # Populate Embedded Signature SuperBlob + SHA-256 CodeDirectory
    sb = cs_file_off
    struct.pack_into('>IIIII', buf, sb, ipados26_depac.CSMAGIC_EMBEDDED_SIGNATURE, sb_len, 1, 0, 20)
    cd = sb + 20
    struct.pack_into(
        '>IIIIIIIIIbbbb', buf, cd,
        ipados26_depac.CSMAGIC_CODEDIRECTORY,
        cd_len,
        0x20400,
        0x00020002,  # CS_ADHOC | CS_LINKER_SIGNED
        hash_off,
        id_off,
        n_special_slots,
        n_code_slots,
        code_limit,
        hash_size,
        ipados26_depac.CS_HASHTYPE_SHA256,
        0,
        page_shift,
    )
    struct.pack_into('>I', buf, cd + 44, 0)           # scatterOffset = 0
    struct.pack_into('>Q', buf, cd + 56, code_limit)  # codeLimit64
    buf[cd + id_off:cd + id_off + len(ident)] = ident

    # Negative special slots (-2 = requirements, -1 = Info.plist)
    special_slot_minus2 = hashlib.sha256(b'synthetic-requirements-blob').digest()
    special_slot_minus1 = hashlib.sha256(b'synthetic-info-plist').digest()
    buf[cd + special_off:cd + special_off + 32] = special_slot_minus2
    buf[cd + special_off + 32:cd + special_off + 64] = special_slot_minus1

    # Initial pre-rewrite SHA-256 code page hashes
    for p_idx in range(n_code_slots):
        pg = bytes(buf[p_idx * page_size:(p_idx + 1) * page_size])
        slot_pos = cd + hash_off + p_idx * hash_size
        buf[slot_pos:slot_pos + hash_size] = hashlib.sha256(pg).digest()

    layout = {
        'text_off': text_sect_off,
        'text_len': len(text_bytes),
        'const_off': const_sect_off,
        'const_len': len(const_bytes),
        'data_off': data_sect_off,
        'data_len': len(data_bytes),
        'cs_off': cs_file_off,
        'cd_off': cd,
        'special_off': cd + special_off,
        'hash_off': cd + hash_off,
        'n_code_slots': n_code_slots,
        'page_size': page_size,
        'special_hashes': (special_slot_minus2, special_slot_minus1),
    }
    return bytes(buf), layout


def build_single_mtlb(func_name, bitcode_payload, extra_tag=(b'TYPE', b'\x02')):
    """Construct a valid single-function Apple MTLB container for testing."""
    header_prefix = b'MTLB\x01\x00\x02\x00\x00\x00\x00\x00\x00\x00\x00\x00'
    tags = [
        (b'NAME', func_name.encode('utf-8') + b'\x00'),
        (b'MDSZ', struct.pack('<Q', len(bitcode_payload))),
        (b'OFFT', struct.pack('<QQQ', 0, 0, 0)),
    ]
    if extra_tag is not None:
        tags.append(extra_tag)

    body = bytearray()
    for tag, val in tags:
        body.extend(tag)
        body.extend(struct.pack('<H', len(val)))
        body.extend(val)
    body.extend(b'ENDT')

    entry = struct.pack('<I', 4 + len(body)) + bytes(body)
    funcs_off = 0x58
    funcs_len = len(entry)
    pub_off = funcs_off + 4 + funcs_len
    pub_buf = struct.pack('<I', 0)
    priv_off = pub_off + len(pub_buf)
    priv_buf = struct.pack('<I', 0)
    bc_off = priv_off + len(priv_buf)
    bc_len = len(bitcode_payload)
    total_size = bc_off + bc_len

    out = bytearray()
    out.extend(header_prefix)
    out.extend(struct.pack('<Q', total_size))
    out.extend(struct.pack('<QQ', funcs_off, funcs_len))
    out.extend(struct.pack('<QQ', pub_off, len(pub_buf)))
    out.extend(struct.pack('<QQ', priv_off, len(priv_buf)))
    out.extend(struct.pack('<QQ', bc_off, bc_len))
    out.extend(struct.pack('<I', 1))
    out.extend(entry)
    out.extend(pub_buf)
    out.extend(priv_buf)
    out.extend(bitcode_payload)
    assert len(out) == total_size
    return bytes(out)


def strip_c_comments_and_strings_preserving_lines(src):
    """Replace C block/line comments and string/char literals with spaces while keeping newlines."""
    # If a diff hunk begins inside an already-open /* ... */ comment, blank up to the first */
    first_close = src.find('*/')
    first_open = src.find('/*')
    first_line_cmt = src.find('//')
    if first_close != -1 and (first_open == -1 or first_close < first_open):
        if first_line_cmt == -1 or first_close < first_line_cmt:
            prefix = src[:first_close + 2]
            blank_prefix = ''.join('\n' if ch == '\n' else ' ' for ch in prefix)
            src = blank_prefix + src[first_close + 2:]

    out = []
    i = 0
    n = len(src)
    state = 'CODE'  # CODE | BLOCK_COMMENT | LINE_COMMENT | STRING | CHAR
    while i < n:
        ch = src[i]
        nxt = src[i + 1] if i + 1 < n else ''
        if state == 'CODE':
            if ch == '/' and nxt == '*':
                out.append('  ')
                i += 2
                state = 'BLOCK_COMMENT'
            elif ch == '/' and nxt == '/':
                out.append('  ')
                i += 2
                state = 'LINE_COMMENT'
            elif ch == '"':
                out.append(' ')
                i += 1
                state = 'STRING'
            elif ch == "'":
                out.append(' ')
                i += 1
                state = 'CHAR'
            else:
                out.append(ch)
                i += 1
        elif state == 'BLOCK_COMMENT':
            if ch == '*' and nxt == '/':
                out.append('  ')
                i += 2
                state = 'CODE'
            else:
                out.append('\n' if ch == '\n' else ' ')
                i += 1
        elif state == 'LINE_COMMENT':
            if ch == '\n':
                out.append('\n')
                state = 'CODE'
            else:
                out.append(' ')
            i += 1
        elif state == 'STRING':
            if ch == '\\' and i + 1 < n:
                out.append('\n' if nxt == '\n' else '  ')
                i += 2
            elif ch == '"':
                out.append(' ')
                i += 1
                state = 'CODE'
            else:
                out.append('\n' if ch == '\n' else ' ')
                i += 1
        elif state == 'CHAR':
            if ch == '\\' and i + 1 < n:
                out.append('\n' if nxt == '\n' else '  ')
                i += 2
            elif ch == "'":
                out.append(' ')
                i += 1
                state = 'CODE'
            else:
                out.append('\n' if ch == '\n' else ' ')
                i += 1
    return ''.join(out)


FORBIDDEN_SLEH_FP_RE = re.compile(r'\b(?:float|double|_Float16)\b')
HUNK_HEADER_RE = re.compile(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@')


def validate_unified_patch_text(patch_name, text):
    """Validate unified diff hunk line counts and `-mgeneral-regs-only` rules on sleh.c.

    Returns (hunk_count, errors_list).
    """
    lines = text.splitlines(keepends=True)
    errors = []
    cur_target = None
    hunk_count = 0
    i = 0
    n = len(lines)

    while i < n:
        ln = lines[i]
        if ln.startswith('+++ '):
            raw_target = ln[4:].strip().split('\t')[0]
            cur_target = raw_target[2:] if raw_target.startswith('b/') else raw_target
            i += 1
            continue

        m = HUNK_HEADER_RE.match(ln)
        if not m:
            i += 1
            continue

        hunk_count += 1
        hunk_start_line = i + 1
        old_expected = int(m.group(2)) if m.group(2) is not None else 1
        new_expected = int(m.group(4)) if m.group(4) is not None else 1
        old_seen = 0
        new_seen = 0
        post_image_lines = []  # (patch_lineno, is_added, text_without_prefix)

        i += 1
        while i < n and (old_seen < old_expected or new_seen < new_expected):
            hl = lines[i]
            if hl.startswith('\\'):
                # "\ No newline at end of file" marker
                i += 1
                continue
            if hl.startswith('-') and not hl.startswith('--- '):
                old_seen += 1
            elif hl.startswith('+') and not hl.startswith('+++ '):
                new_seen += 1
                post_image_lines.append((i + 1, True, hl[1:]))
            elif hl.startswith(' '):
                old_seen += 1
                new_seen += 1
                post_image_lines.append((i + 1, False, hl[1:]))
            elif hl in ('\n', '\r\n'):
                # Unprefixed empty context line
                old_seen += 1
                new_seen += 1
                post_image_lines.append((i + 1, False, hl))
            else:
                errors.append(
                    f'{patch_name}:{i + 1}: unexpected line inside hunk started at line '
                    f'{hunk_start_line} ({cur_target}): {hl.rstrip()!r}'
                )
                break
            i += 1

        if old_seen != old_expected or new_seen != new_expected:
            errors.append(
                f'{patch_name}:{hunk_start_line}: hunk count mismatch in {cur_target}: '
                f'header expected (-{old_expected}, +{new_expected}), '
                f'actual (-{old_seen}, +{new_seen})'
            )

        if cur_target and cur_target.replace('\\', '/').endswith('osfmk/arm64/sleh.c'):
            joined = ''.join(
                t if t.endswith('\n') else (t + '\n')
                for _, _, t in post_image_lines
            )
            stripped_lines = strip_c_comments_and_strings_preserving_lines(joined).splitlines()
            for idx_pl, (patch_lineno, is_added, orig_code) in enumerate(post_image_lines):
                if not is_added or idx_pl >= len(stripped_lines):
                    continue
                code_only = stripped_lines[idx_pl]
                fp_match = FORBIDDEN_SLEH_FP_RE.search(code_only)
                if fp_match:
                    errors.append(
                        f'{patch_name}:{patch_lineno}: forbidden FP type {fp_match.group(0)!r} '
                        f'in osfmk/arm64/sleh.c (-mgeneral-regs-only): {orig_code.strip()}'
                    )

    return hunk_count, errors


class TestDepacRewriterAndCodeSignature(unittest.TestCase):
    """P2-15.1: Verify ipados26_depac.py instruction rewrites, section safety, and SHA-256 rehashing."""

    def test_tbi_safe_xpacd_and_xpaci_bitmask_encodings(self):
        # XPACI X0 -> NOP (0xD503201F)
        xpaci_x0, xpaci_kind = ipados26_depac.rewrite_arm64e_insn(0xDAC143E0)
        self.assertEqual(xpaci_kind, 'xpaci')
        self.assertEqual(xpaci_x0, 0xD503201F)

        # XPACD X1 -> NOP (0xD503201F): preserves Darwin TBI [63:56] AND negative
        # sentinel pointers (-1, -2 = 0xfffffffffffffffe in libdispatch)!
        xpacd_x1, xpacd_kind = ipados26_depac.rewrite_arm64e_insn(0xDAC147E1)
        self.assertEqual(xpacd_kind, 'xpacd')
        self.assertEqual(xpacd_x1, 0xD503201F)

        xpaclri, xpaclri_kind = ipados26_depac.rewrite_arm64e_insn(0xD50320FF)
        self.assertEqual(xpaclri_kind, 'xpaclri')
        self.assertEqual(xpaclri, 0xD503201F)

    def test_synthetic_macho_rewrite_cs_rehash_and_audit_cli(self):
        # Pairs of (arm64e_input_insn, expected_armv80_insn)
        insn_vectors = [
            (0xD65F0BFF, 0xD65F03C0),                          # RETAA -> RET
            (0xD65F0FFF, 0xD65F03C0),                          # RETAB -> RET
            (0xD73F0800 | (8 << 5) | 9, 0xD63F0000 | (8 << 5)),  # BLRAA X8, X9 -> BLR X8
            (0xD61F0C1F | (3 << 5), 0xD61F0000 | (3 << 5)),    # BRABZ X3 -> BR X3
            (0xD503233F, 0xD503233F),                          # PACIASP (HINT #25, hw NOP on v8.0)
            (0xD50323BF, 0xD50323BF),                          # AUTIASP (HINT #29, hw NOP on v8.0)
            (0xDAC10020, 0xD503201F),                          # PACIA X0, X1 -> NOP
            (0xDAC11462, 0xD503201F),                          # AUTIB X2, X3 -> NOP
            (0xDAC143E0, 0xD503201F),                          # XPACI X0 -> NOP
            (0xDAC147E1, 0xD503201F),                          # XPACD X1 -> NOP
            (0xD50320FF, 0xD503201F),                          # XPACLRI -> NOP
            (0xF8BFC000 | (3 << 5) | 2, 0xC8DFFC00 | (3 << 5) | 2),  # LDAPR X2, [X3] -> LDAR X2, [X3]
            (0xD50B7C24, 0xD50B7A24),                          # DC CVAP, X4 -> DC CVAC, X4
            (0xD50330FF, 0xD5033FDF),                          # SB -> ISB (ARMv8.5 barrier)
            (0xF8200400 | (1 << 12) | (6 << 5) | 5,            # LDRAA X5, [X6, #8] -> LDR X5, [X6, #8]
             0xF9400000 | (1 << 10) | (6 << 5) | 5),
        ]
        input_text = [pair[0] for pair in insn_vectors]
        expected_text = [pair[1] for pair in insn_vectors]

        # PAC-looking words placed in __TEXT,__const and __DATA,__data — MUST NOT be rewritten!
        decoy_pac_words = [
            0xD65F0BFF,  # looks like RETAA
            0xD65F0FFF,  # looks like RETAB
            0xD73F0909,  # looks like BLRAA
            0xD61F0C7F,  # looks like BRABZ
            0xDAC143E0,  # looks like XPACI X0
            0xDAC147E1,  # looks like XPACD X1
            0xD50320FF,  # looks like XPACLRI
            0xF8BFC062,  # looks like LDAPR
            0xD50B7C24,  # looks like DC CVAP
            0xF82014C5,  # looks like LDRAA
        ]

        raw_macho, layout = build_synthetic_arm64e_macho(
            text_insns=input_text,
            const_words=decoy_pac_words,
            data_words=decoy_pac_words * 4,
        )
        orig_const_bytes = raw_macho[layout['const_off']:layout['const_off'] + layout['const_len']]
        orig_data_bytes = raw_macho[layout['data_off']:layout['data_off'] + layout['data_len']]
        orig_page0_sha = hashlib.sha256(raw_macho[0:4096]).digest()

        depac_script = os.path.join(TOOLS_DIR, 'ipados26_depac.py')
        with tempfile.TemporaryDirectory() as td:
            bin_path = os.path.join(td, 'test_arm64e_bin')
            with open(bin_path, 'wb') as f:
                f.write(raw_macho)

            # 1. Run --rewrite via CLI
            res_rw = subprocess.run(
                [sys.executable, '-B', depac_script, '--rewrite', bin_path],
                capture_output=True, text=True, env=_clean_subprocess_env(),
            )
            self.assertEqual(res_rw.returncode, 0, msg=res_rw.stderr)
            self.assertIn('13 PAC/non-ARMv8.0 instructions rewritten', res_rw.stdout)

            with open(bin_path, 'rb') as f:
                post_buf = f.read()
            self.assertEqual(len(post_buf), len(raw_macho))

            # 2. Assert every __TEXT,__text instruction matches exact ARMv8.0 encoding
            got_text = list(struct.unpack_from(f'<{len(expected_text)}I', post_buf, layout['text_off']))
            self.assertEqual(
                [hex(x) for x in got_text],
                [hex(x) for x in expected_text],
            )

            # 3. Assert __TEXT,__const and __DATA,__data bytes are 100% untouched
            self.assertEqual(
                post_buf[layout['const_off']:layout['const_off'] + layout['const_len']],
                orig_const_bytes,
            )
            self.assertEqual(
                post_buf[layout['data_off']:layout['data_off'] + layout['data_len']],
                orig_data_bytes,
            )

            # 4. Assert negative special slots (-2, -1) are untouched and EVERY code page SHA-256 matches
            sp_off = layout['special_off']
            self.assertEqual(post_buf[sp_off:sp_off + 32], layout['special_hashes'][0])
            self.assertEqual(post_buf[sp_off + 32:sp_off + 64], layout['special_hashes'][1])

            psize = layout['page_size']
            for p_idx in range(layout['n_code_slots']):
                page_bytes = post_buf[p_idx * psize:(p_idx + 1) * psize]
                expected_sha = hashlib.sha256(page_bytes).digest()
                slot_off = layout['hash_off'] + p_idx * 32
                recorded_sha = post_buf[slot_off:slot_off + 32]
                self.assertEqual(
                    recorded_sha, expected_sha,
                    f'CS_CodeDirectory SHA-256 mismatch on page {p_idx}',
                )
            # Page 0 hash MUST have updated from its pre-rewrite digest
            self.assertNotEqual(post_buf[layout['hash_off']:layout['hash_off'] + 32], orig_page0_sha)

            # 5. Run --audit via CLI and assert exit 0 + 0 remaining unrewritten PAC instructions
            res_audit = subprocess.run(
                [sys.executable, '-B', depac_script, '--audit', bin_path],
                capture_output=True, text=True, env=_clean_subprocess_env(),
            )
            self.assertEqual(res_audit.returncode, 0, msg=res_audit.stderr)
            self.assertIn('0 unrewritten PAC (FATAL)', res_audit.stdout)

    def test_sb_isb_ipa_helper_parity_and_audit_kernel_telemetry(self):
        # Verify SB -> ISB rewrite + --audit kernel-emulated telemetry (DotProd/FCMA/FHM/FlagM/FP16)
        kemu_and_sb_text = [
            0xD50330FF,  # SB -> ISB (rewritten)
            0x4E829420,  # SDOT V0.4S, V1.16B, V2.16B (feat_dotprod)
            0x2FA2E020,  # UDOT V0.2S, V1.8B, V2.4B[1] (feat_dotprod)
            0x2E82E420,  # FCADD V0.4S, V1.4S, V2.4S, #90 (feat_fcma)
            0x6E82C420,  # FCMLA V0.4S, V1.4S, V2.4S, #0 (feat_fcma)
            0x0E22EC20,  # FMLAL V0.2S, V1.2H, V2.2H (feat_fhm)
            0x6F82C020,  # FMLSL2 V0.4S, V1.4H, V2.H[0] (feat_fhm)
            0xD500401F,  # CFINV (feat_flagm)
            0xD500403F,  # XAFLAG (feat_flagm)
            0xD500405F,  # AXFLAG (feat_flagm)
            0x6E422C20,  # FACGE V0.8H, V1.8H, V2.8H (feat_fp16)
            0x6E421420,  # FADDP V0.8H, V1.8H, V2.8H (feat_fp16)
            0xD65F03C0,  # RET
        ]
        raw_macho, layout = build_synthetic_arm64e_macho(
            text_insns=kemu_and_sb_text,
            const_words=[0x12345678],
            data_words=[0x87654321],
        )
        depac_script = os.path.join(TOOLS_DIR, 'ipados26_depac.py')
        with tempfile.TemporaryDirectory() as td:
            bin_path = os.path.join(td, 'test_kemu_bin')
            with open(bin_path, 'wb') as f:
                f.write(raw_macho)

            res_rw = subprocess.run(
                [sys.executable, '-B', depac_script, '--rewrite', bin_path],
                capture_output=True, text=True, env=_clean_subprocess_env(),
            )
            self.assertEqual(res_rw.returncode, 0, msg=res_rw.stderr)
            self.assertIn('1 PAC/non-ARMv8.0 instructions rewritten', res_rw.stdout)

            with open(bin_path, 'rb') as f:
                post = f.read()
            first_insn, = struct.unpack_from('<I', post, layout['text_off'])
            self.assertEqual(first_insn, 0xD5033FDF)  # ISB

            res_audit = subprocess.run(
                [sys.executable, '-B', depac_script, '--audit', bin_path],
                capture_output=True, text=True, env=_clean_subprocess_env(),
            )
            self.assertEqual(res_audit.returncode, 0, msg=res_audit.stderr)
            self.assertIn('0 unrewritten PAC (FATAL), 11 kernel-emulated', res_audit.stdout)
            for tag in ('feat_dotprod', 'feat_fcma', 'feat_fhm', 'feat_flagm', 'feat_fp16'):
                self.assertIn(tag, res_audit.stdout)

        # Verify native on-device IPA installer C rewriter (userland/store/ipa_helper.m) parity
        ipa_helper_path = os.path.join(REPO_ROOT, 'userland', 'store', 'ipa_helper.m')
        with open(ipa_helper_path, 'r', encoding='utf-8') as f:
            ipa_src = f.read()
        self.assertIn('0xD50330FFU', ipa_src)  # SB
        self.assertIn('0xD5033FDFU', ipa_src)  # ISB
        self.assertIn('0xD503201FU', ipa_src)  # NOP for PAC/AUT/XPACI/XPACD/XPACLRI
        self.assertIn('0xFFFFFC00U', ipa_src)  # Tightened branch-with-PAC mask

    def test_depac_builtin_dsc_and_scatter_selftest(self):
        with warnings.catch_warnings():
            warnings.simplefilter('ignore', ResourceWarning)
            ipados26_depac.run_selftest()


class TestMkadtAndMemoryCarveouts(unittest.TestCase):
    """P2-15.2: Verify ipados26_mkadt.py DeviceTree invariants and SM6225 memory carveout disjointness."""

    def _collect_phandles(self, node, out_list):
        ph = node.prop('AAPL,phandle')
        if ph is not None:
            self.assertEqual(len(ph), 4)
            val, = struct.unpack('<I', ph)
            self.assertGreater(val, 0)
            out_list.append(val)
        for c in node.children:
            self._collect_phandles(c, out_list)

    def test_mkadt_cli_across_flag_combinations(self):
        mkadt_script = os.path.join(TOOLS_DIR, 'ipados26_mkadt.py')
        configs = [
            ('default_16k_wifi', [], 'iPad11,6', 'J171aAP', 'J171a', 4, 0xB0000000, 0x4D000000, (1080, 2400), False),
            ('amp_4k_8core', ['--amp'], 'iPad11,6', 'J171aAP', 'J171a', 8, 0xB0000000, 0x4D000000, (1080, 2400), False),
            ('cellular_j172a', ['--cellular'], 'iPad11,7', 'J172aAP', 'J172a', 4, 0xB0000000, 0x4D000000, (1080, 2400), True),
            ('highmem_wifi', ['--highmem'], 'iPad11,6', 'J171aAP', 'J171a', 4, 0x140000000, 0x100000000, (1080, 2400), False),
            ('full_amp_cell_highmem_canvas', ['--highmem', '--amp', '--cellular', '--ipad-canvas'],
             'iPad11,7', 'J172aAP', 'J172a', 8, 0x140000000, 0x100000000, (1620, 2160), True),
        ]

        with tempfile.TemporaryDirectory() as td:
            for label, flags, exp_model, exp_board, exp_target, exp_cpus, exp_base, exp_sz, exp_canvas, exp_bb in configs:
                with self.subTest(config=label):
                    out_adt = os.path.join(td, f'{label}.adt')
                    res = subprocess.run(
                        [sys.executable, '-B', mkadt_script, out_adt, *flags],
                        capture_output=True, text=True, env=_clean_subprocess_env(),
                    )
                    self.assertEqual(res.returncode, 0, msg=res.stderr)
                    self.assertTrue(os.path.isfile(out_adt))

                    with open(out_adt, 'rb') as f:
                        blob = f.read()
                    self.assertGreater(len(blob), 512)
                    self.assertEqual(len(blob) % 4, 0, 'ADT blob must be 4-byte aligned')
                    self.assertIn(exp_model.encode('ascii') + b'\x00', blob)
                    self.assertIn(exp_board.encode('ascii') + b'\x00', blob)

                    root, end_off = adt.parse(blob)
                    self.assertEqual(end_off, len(blob), 'ADT parser did not consume exact blob length')
                    self.assertEqual(root.name, 'device-tree')
                    self.assertEqual(root.prop('model'), exp_model.encode('ascii') + b'\x00')
                    self.assertEqual(root.prop('target-sub-type'), exp_board.encode('ascii') + b'\x00')
                    self.assertEqual(root.prop('target-type'), exp_target.encode('ascii') + b'\x00')

                    # Every AAPL,phandle across the tree must be unique
                    phandles = []
                    self._collect_phandles(root, phandles)
                    self.assertEqual(len(phandles), len(set(phandles)), 'Duplicate AAPL,phandle in ADT')

                    by_name = {c.name: c for c in root.children}
                    chosen = by_name['chosen']
                    self.assertEqual(struct.unpack('<Q', chosen.prop('dram-base'))[0], exp_base)
                    self.assertEqual(struct.unpack('<Q', chosen.prop('dram-size'))[0], exp_sz)
                    self.assertEqual(struct.unpack('<I', chosen.prop('chip-id'))[0], 0x8020)

                    cpus_node = by_name['cpus']
                    self.assertEqual(len(cpus_node.children), exp_cpus)
                    for idx_c, cpu_n in enumerate(cpus_node.children):
                        self.assertEqual(struct.unpack('<I', cpu_n.prop('timebase-frequency'))[0], 19200000)
                        mpidr, = struct.unpack('<I', cpu_n.prop('reg'))
                        expected_mpidr = (idx_c & 3) if idx_c >= 4 else (0x100 | idx_c)
                        self.assertEqual(mpidr, expected_mpidr)

                    # /arm-io ranges must translate relative to non-zero SOC_BASE (0x01000000)
                    arm_io = by_name['arm-io']
                    child_bus, parent_pa, range_sz = struct.unpack('<QQQ', arm_io.prop('ranges'))
                    self.assertEqual(child_bus, 0)
                    self.assertEqual(parent_pa, 0x01000000)
                    self.assertGreater(range_sz, 0)

                    arm_children = {c.name: c for c in arm_io.children}
                    gic = arm_children['gic']
                    gicd_rel, gicd_sz, gicr_rel, gicr_sz = struct.unpack('<4Q', gic.prop('reg'))
                    self.assertEqual((parent_pa + gicd_rel, gicd_sz), (0x0F200000, 0x10000))
                    self.assertEqual((parent_pa + gicr_rel, gicr_sz), (0x0F300000, 0x100000))
                    self.assertEqual(struct.unpack('<I', gic.prop('virtual-timer-intid'))[0], 19)

                    disp0 = arm_children['disp0']
                    cw, = struct.unpack('<I', disp0.prop('canvas-width'))
                    ch, = struct.unpack('<I', disp0.prop('canvas-height'))
                    self.assertEqual((cw, ch), exp_canvas)

                    self.assertEqual('baseband' in by_name, exp_bb)
                    self.assertIn('sweet-ufs', by_name)

    def test_sapphire_physical_memory_carveouts_non_overlapping(self):
        # Authoritative Sapphire SM6225 physical memory map (IPADOS26_XNU_SPECS.md Section 2)
        regions = [
            ('SOC_MMIO_WINDOW', 0x00000000, 0x40000000),
            ('HYP_EL2', 0x45700000, 0x00600000),
            ('AOP_RPM', 0x45E00000, 0x00140000),
            ('SEC_APPS', 0x45FFF000, 0x00001000),
            ('SMEM', 0x46000000, 0x00200000),
            ('CDSP_SECURE', 0x46200000, 0x01E00000),
            ('MODEM_PIL_PAS4', 0x4AB00000, 0x06900000),
            ('WLAN_MSA', 0x51900000, 0x00100000),
            ('ADSP_PIL_PAS1', 0x53800000, 0x02200000),
            ('IPA_FW_PAS15', 0x55A00000, 0x00010000),
            ('GPU_ZAP_PAS13', 0x55A15000, 0x00002000),
            ('CONT_SPLASH_FB', ipados26_mkadt.FB, ipados26_mkadt.FB_SIZE),
            ('UEFI_XNU_PAYLOAD', 0xA0000000, 0x04000000),
            ('CAMERA_ISP_POOL', 0xD7000000, 0x09000000),
            ('MODEM_WLAN_DMA_POOL', 0xE0000000, 0x01000000),
            ('GPU_RING_PT2_POOL', 0xE1000000, 0x00500000),
            ('SWEET_FB_LOW_POOL', 0xE2000000, 0x03000000),
            ('QSEE_TZ_POOL', 0xE8000000, 0x01000000),
            ('GPU_USER_GPOOL', 0x100000000, 0x40000000),
            ('XNU_HIGHMEM_RAM', ipados26_mkadt.HIGH_PHYS_BASE, ipados26_mkadt.HIGH_MEM_SIZE),
        ]

        overlaps = []
        for idx, (name_a, base_a, sz_a) in enumerate(regions):
            self.assertGreater(sz_a, 0, f'{name_a} must have positive size')
            self.assertEqual(base_a & 0xFFF, 0, f'{name_a} base 0x{base_a:x} not 4K-aligned')
            self.assertEqual(sz_a & 0xFFF, 0, f'{name_a} size 0x{sz_a:x} not 4K-aligned')
            end_a = base_a + sz_a
            for name_b, base_b, sz_b in regions[idx + 1:]:
                end_b = base_b + sz_b
                if base_a < end_b and base_b < end_a:
                    overlaps.append(
                        f'{name_a} [0x{base_a:x}..0x{end_a:x}) overlaps '
                        f'{name_b} [0x{base_b:x}..0x{end_b:x})'
                    )
        self.assertEqual(overlaps, [])

        # Also verify Low-RAM mode [0xB0000000..0xFD000000) never overlaps TZ/PIL/Splash/MMIO below 0xA4000000
        low_start = ipados26_mkadt.PHYS_BASE
        low_end = low_start + ipados26_mkadt.MEM_SIZE
        for name, base, sz in regions:
            if base < 0xB0000000:
                self.assertLessEqual(base + sz, low_start, f'{name} overlaps XNU low-RAM base')
        self.assertLessEqual(low_end, 0x100000000)


class TestCaMetallibAndDscOffsets(unittest.TestCase):
    """P2-15.3: Verify ipados26_ca_metallib.py MTLB + BC\\xc0\\xde round-trip and DSC offset helpers."""

    def test_synthetic_mtlb_extract_merge_and_rootfs_generation(self):
        bc_vert = b'BC\xc0\xde' + b'\x21\x0c\x00\x00' + b'AIR_VERT_SHADER_PAYLOAD_01' * 4
        bc_frag = b'BC\xc0\xde' + b'\x21\x0c\x00\x00' + b'AIR_FRAG_SHADER_PAYLOAD_02' * 5
        bc_vert_dup = b'BC\xc0\xde' + b'\x21\x0c\x00\x00' + b'DUPLICATE_SHOULD_BE_SKIPPED'

        m1 = build_single_mtlb('fixed_vert_lph_gen', bc_vert)
        m2 = build_single_mtlb('fixed_frag_lph_cpf', bc_frag)
        m3 = build_single_mtlb('fixed_vert_lph_gen', bc_vert_dup)

        # Concatenated pipelinelib/functions stream (with padding noise between containers)
        concat_stream = b'\x00' * 16 + m1 + b'\x00' * 8 + m2 + m3

        found_mtlbs = list(ipados26_ca_metallib.iter_concatenated_mtlbs(concat_stream))
        self.assertEqual(len(found_mtlbs), 3)

        n1, _t1, mod1 = ipados26_ca_metallib.extract_first_function(found_mtlbs[0])
        self.assertEqual(n1, 'fixed_vert_lph_gen')
        self.assertEqual(mod1, bc_vert)

        merged_blob, unique_cnt = ipados26_ca_metallib.build_merged_metallib(concat_stream)
        self.assertEqual(unique_cnt, 2)
        self.assertIsNotNone(merged_blob)
        self.assertEqual(merged_blob[:4], b'MTLB')

        total_sz, = struct.unpack_from('<Q', merged_blob, 0x10)
        funcs_off, funcs_len = struct.unpack_from('<QQ', merged_blob, 0x18)
        bc_off, bc_len = struct.unpack_from('<QQ', merged_blob, 0x48)
        self.assertEqual(total_sz, len(merged_blob))
        self.assertEqual(funcs_off, 0x58)
        self.assertEqual(bc_len, len(bc_vert) + len(bc_frag))
        self.assertEqual(merged_blob[bc_off:bc_off + bc_len], bc_vert + bc_frag)
        self.assertEqual(struct.unpack_from('<I', merged_blob, funcs_off)[0], 2)

        # Verify first function extraction works on merged MTLB too
        first_fn = ipados26_ca_metallib.extract_first_function(merged_blob)
        self.assertIsNotNone(first_fn)
        self.assertEqual(first_fn[0], 'fixed_vert_lph_gen')
        self.assertEqual(first_fn[2], bc_vert)

        # Verify generate_ca_metallib() end-to-end on synthetic Cryptex RootFS layout
        with tempfile.TemporaryDirectory() as td:
            fn_dir = os.path.join(
                td, 'System', 'Cryptexes', 'OS', 'System', 'Library',
                'Frameworks', 'QuartzCore.framework', 'default.pipelinelib',
            )
            os.makedirs(fn_dir, exist_ok=True)
            with open(os.path.join(fn_dir, 'functions'), 'wb') as f:
                f.write(concat_stream)

            ok = ipados26_ca_metallib.generate_ca_metallib(td)
            self.assertTrue(ok)
            out_ml = os.path.join(td, 'redwood', 'share', 'ca-share', 'QuartzCore-default.metallib')
            self.assertTrue(os.path.isfile(out_ml))
            with open(out_ml, 'rb') as f:
                self.assertEqual(f.read(), merged_blob)

    def test_metallib_v3_extended_offt_rflt_and_fat64_air64(self):
        # Build a synthetic iPadOS 26.1 / Metal 3.2+ Metallib v3 (version=3, 0x80 header,
        # 32-byte extended OFFT [pub, priv, bitcode, rflt], 4-byte RFLT tag length) wrapped in
        # a 64-bit Universal Mach-O (FAT_MAGIC_64 = 0xCAFEBABF, CPU_TYPE_AIR64 = 0x01000017).
        bc_pad = b'\xAA' * 64
        bc_v3 = b'BC\xc0\xde' + b'\x35\x14\x00\x00' + b'AIR64_V3_FRAGMENT_BITCODE_PAYLOAD' * 3
        bc_stream = bc_pad + bc_v3
        rflt_payload = b'RFLT_REFLECTION_BLOB' * 20

        body = bytearray()
        for tag, val in (
            (b'NAME', b'ca_v3_frag_shader\x00'),
            (b'TYPE', b'\x01'),
            (b'VERS', struct.pack('<HHHH', 2, 8, 3, 2)),
            (b'MDSZ', struct.pack('<Q', len(bc_v3))),
            # 32-byte extended OFFT: [pub=0, priv=0, bitcode=len(bc_pad), rflt=8]
            (b'OFFT', struct.pack('<QQQQ', 0, 0, len(bc_pad), 8)),
        ):
            body.extend(tag)
            body.extend(struct.pack('<H', len(val)))
            body.extend(val)
        # Mode 1 large chunk tag RFLT uses a 32-bit uint32_t length field
        body.extend(b'RFLT')
        body.extend(struct.pack('<I', len(rflt_payload)))
        body.extend(rflt_payload)
        body.extend(b'ENDT')

        entry = struct.pack('<I', 4 + len(body)) + bytes(body)
        funcs_off = 0x80
        funcs_len = len(entry)
        pub_off = funcs_off + 4 + funcs_len
        pub_buf = struct.pack('<I', 0)
        priv_off = pub_off + len(pub_buf)
        priv_buf = struct.pack('<I', 0)
        bc_off = priv_off + len(priv_buf)
        rflt_sec_off = bc_off + len(bc_stream)
        rflt_sec = b'\x00' * 32
        total_v3 = rflt_sec_off + len(rflt_sec)

        thin_v3 = bytearray()
        thin_v3.extend(b'MTLB')
        thin_v3.extend(struct.pack('<HHHH', 1, 2, 3, 0))  # version = 3 at +0x08
        thin_v3.extend(b'\x00' * 4)
        thin_v3.extend(struct.pack('<Q', total_v3))
        thin_v3.extend(struct.pack('<QQ', funcs_off, funcs_len))
        thin_v3.extend(struct.pack('<QQ', pub_off, len(pub_buf)))
        thin_v3.extend(struct.pack('<QQ', priv_off, len(priv_buf)))
        thin_v3.extend(struct.pack('<QQ', bc_off, len(bc_stream)))
        thin_v3.extend(struct.pack('<QQ', rflt_sec_off, len(rflt_sec)))
        thin_v3.extend(b'\x00' * (funcs_off - len(thin_v3)))
        thin_v3.extend(struct.pack('<I', 1))
        thin_v3.extend(entry)
        thin_v3.extend(pub_buf)
        thin_v3.extend(priv_buf)
        thin_v3.extend(bc_stream)
        thin_v3.extend(rflt_sec)
        self.assertEqual(len(thin_v3), total_v3)

        # Wrap thin_v3 in FAT_MAGIC_64 (2 slices: dummy GPU binary + CPU_TYPE_AIR64)
        dummy_slice = b'DUMMY_GPU_ARCH' * 4
        fat64_hdr_sz = 8 + 2 * 32
        off0 = (fat64_hdr_sz + 15) & ~15
        off1 = (off0 + len(dummy_slice) + 15) & ~15
        fat64 = bytearray(off1 + len(thin_v3))
        struct.pack_into('>II', fat64, 0, ipados26_ca_metallib.FAT_MAGIC_64, 2)
        struct.pack_into('>IIQQII', fat64, 8, 0x01000013, 1, off0, len(dummy_slice), 4, 0)
        struct.pack_into('>IIQQII', fat64, 40, ipados26_ca_metallib.CPU_TYPE_AIR64, 2, off1, len(thin_v3), 4, 0)
        fat64[off0:off0 + len(dummy_slice)] = dummy_slice
        fat64[off1:off1 + len(thin_v3)] = thin_v3

        extracted = ipados26_ca_metallib.extract_first_function(bytes(fat64))
        self.assertIsNotNone(extracted)
        self.assertEqual(extracted[0], 'ca_v3_frag_shader')
        self.assertEqual(extracted[2], bc_v3)

        merged_v3, cnt_v3 = ipados26_ca_metallib.build_merged_metallib(bytes(fat64))
        self.assertEqual(cnt_v3, 1)
        self.assertIsNotNone(merged_v3)
        re_ext = ipados26_ca_metallib.extract_first_function(merged_v3)
        self.assertIsNotNone(re_ext)
        self.assertEqual(re_ext[0], 'ca_v3_frag_shader')
        self.assertEqual(re_ext[2], bc_v3)

    def test_dsc_offsets_pointer_decode_and_exports_trie(self):
        # Verify DyldSharedCacheReader.decode_pointer static logic for slide v5 & v3
        decode = ipados26_dsc_offsets.DyldSharedCacheReader.decode_pointer
        val_add = 0x180000000
        # Slide v5 authenticated pointer (bit 63 = 1, runtime offset in [33:0])
        raw_v5_auth = (1 << 63) | 0x01234560
        self.assertEqual(decode(None, raw_v5_auth, 5, val_add), val_add + 0x01234560)
        # Slide v5 non-auth pointer with top-byte TBI = 0xAB (stored at [41:34], shifted << 22)
        raw_v5_tbi = (0xAB << 34) | 0x00456780
        self.assertEqual(decode(None, raw_v5_tbi, 5, val_add), (0xAB << 56) | (val_add + 0x00456780))
        # Slide v3 authenticated pointer (bit 63 = 1, 32-bit offset)
        raw_v3_auth = (1 << 63) | 0x00ABCDEF
        self.assertEqual(decode(None, raw_v3_auth, 3, val_add), val_add + 0x00ABCDEF)

        # Build a minimal Mach-O LC_DYLD_EXPORTS_TRIE with 1 child edge -> terminal export at +0x4200
        # Root node @0: term_size=0, n_children=1, edge="__ZTVTest\0", child_off=14
        # Child node @14: term_size=3, flags=0, sym_off=0x4200 (ULEB 0x80 0x84 0x01 -> wait: 0x4200 = 0x80, 0x84, 0x01), n_children=0
        trie = bytearray([0, 1]) + b'__ZTVTest\x00' + bytearray([13, 4, 0, 0x80, 0x84, 0x01, 0])
        found = ipados26_dsc_offsets._walk_exports_trie(bytes(trie), 0, len(trie), {'__ZTVTest'})
        self.assertEqual(found, {'__ZTVTest': 0x4200})


class TestXnuPatchsetIntegrity(unittest.TestCase):
    """P2-15.4: Verify unified diff hunk line counts and sleh.c -mgeneral-regs-only safety."""

    def test_all_zz_patches_have_valid_hunks_and_clean_sleh(self):
        patch_files = sorted(glob.glob(os.path.join(PATCHES_DIR, 'zz-*.patch')))
        self.assertGreater(len(patch_files), 0, f'No zz-*.patch files found in {PATCHES_DIR}')

        total_hunks = 0
        all_errors = []
        for p in patch_files:
            with open(p, 'r', encoding='utf-8', errors='replace') as f:
                text = f.read()
            self.assertGreater(len(text), 0, f'Empty patch file: {p}')
            hunks, errs = validate_unified_patch_text(os.path.basename(p), text)
            self.assertGreater(hunks, 0, f'Patch {p} contains 0 unified diff hunks')
            total_hunks += hunks
            all_errors.extend(errs)

        self.assertGreater(total_hunks, 0)
        self.assertEqual(all_errors, [], '\n'.join(all_errors))

    def test_patch_validator_catches_bad_hunk_counts_and_sleh_fp_types(self):
        # 1. Bad hunk count must be rejected
        bad_count_patch = (
            '--- a/osfmk/arm64/foo.c\n'
            '+++ b/osfmk/arm64/foo.c\n'
            '@@ -1,2 +1,3 @@\n'
            ' int a = 1;\n'
            '+int b = 2;\n'
        )
        _, errs_cnt = validate_unified_patch_text('bad_count.patch', bad_count_patch)
        self.assertEqual(len(errs_cnt), 1)
        self.assertIn('hunk count mismatch', errs_cnt[0])

        # 2. Forbidden float/double/_Float16 in sleh.c code must be rejected, while comments pass
        sleh_patch = (
            '--- a/osfmk/arm64/sleh.c\n'
            '+++ b/osfmk/arm64/sleh.c\n'
            '@@ -10,2 +10,7 @@\n'
            ' uint64_t x = 0;\n'
            '+/* Comment mentioning float, double, and _Float16 is safe */\n'
            '+// Another float64 / double comment\n'
            '+float bad1 = 0;\n'
            '+double bad2 = 0;\n'
            '+_Float16 bad3 = 0;\n'
            ' return x;\n'
        )
        _, errs_fp = validate_unified_patch_text('bad_sleh.patch', sleh_patch)
        self.assertEqual(len(errs_fp), 3)

    def test_zz_0105_sequential_application_on_zz_0104(self):
        p104 = os.path.join(PATCHES_DIR, 'zz-0104-ipados26-armv82-fp16-rdm-emu.patch')
        p105 = os.path.join(PATCHES_DIR, 'zz-0105-ipados26-armv82-dotprod-fcma-fhm-emu.patch')
        self.assertTrue(os.path.isfile(p104))
        self.assertTrue(os.path.isfile(p105))

        def parse_hunks(path):
            with open(path, 'r', encoding='utf-8') as f:
                lines = f.read().splitlines()
            hunks = []
            idx = 0
            while idx < len(lines):
                m = HUNK_HEADER_RE.match(lines[idx])
                if not m:
                    idx += 1
                    continue
                old_start = int(m.group(1))
                old_cnt = int(m.group(2)) if m.group(2) is not None else 1
                new_start = int(m.group(3))
                new_cnt = int(m.group(4)) if m.group(4) is not None else 1
                idx += 1
                body = []
                o_seen = n_seen = 0
                while idx < len(lines) and (o_seen < old_cnt or n_seen < new_cnt):
                    hl = lines[idx]
                    if hl.startswith('-'):
                        o_seen += 1
                    elif hl.startswith('+'):
                        n_seen += 1
                    else:
                        o_seen += 1
                        n_seen += 1
                    body.append(hl)
                    idx += 1
                hunks.append((old_start, old_cnt, new_start, new_cnt, body))
            return hunks

        def apply_hunks_strict(file_lines, hunks):
            out = list(file_lines)
            delta = 0
            for old_start, old_cnt, _new_start, _new_cnt, body in hunks:
                pos = old_start - 1 + delta
                expected_old = [b[1:] for b in body if not b.startswith('+')]
                self.assertEqual(
                    out[pos:pos + old_cnt], expected_old,
                    f'Zero-fuzz context mismatch at line {old_start}',
                )
                replacement = [b[1:] for b in body if not b.startswith('-')]
                out[pos:pos + old_cnt] = replacement
                delta += len(replacement) - old_cnt
            return out

        h104 = parse_hunks(p104)
        h105 = parse_hunks(p105)
        self.assertEqual(len(h104), 3)
        self.assertEqual(len(h105), 3)

        # Seed a 2500-line mock sleh.c with zz-0104's exact pre-image context lines
        mock_sleh = [f'/* line {k} */' for k in range(1, 2501)]
        for old_start, old_cnt, _ns, _nc, body in h104:
            pre = [b[1:] for b in body if not b.startswith('+')]
            mock_sleh[old_start - 1:old_start - 1 + old_cnt] = pre

        after_104 = apply_hunks_strict(mock_sleh, h104)
        after_105 = apply_hunks_strict(after_104, h105)
        joined_105 = '\n'.join(after_105)

        for token in (
            'sweet_emulate_dotprod_fcma_fhm',
            '0xD500401FU',  # CFINV
            '0xD500403FU',  # XAFLAG
            '0xD500405FU',  # AXFLAG
            'sweet_fp16_to_fp32_bits',
            'sweet_norm124_to_fp32',
        ):
            self.assertIn(token, joined_105)


class TestRootfsTweaksAndCryptex(unittest.TestCase):
    """P1-9 & P2-10: Verify diskarbitrationd disablement, Cryptex mirroring, and /redwood defaults."""

    def test_diskarbitrationd_disable_and_cryptex_mirror(self):
        import importlib.util
        import plistlib

        spec = importlib.util.spec_from_file_location(
            'ipados26_root_tweaks',
            os.path.join(TOOLS_DIR, 'ipados26-root-tweaks.py'),
        )
        tweaks = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tweaks)

        with tempfile.TemporaryDirectory() as td:
            # 1. Populate synthetic /private/preboot/Cryptexes/{OS,App} + empty /System/Cryptexes/OS
            pre_os = os.path.join(td, 'private', 'preboot', 'Cryptexes', 'OS')
            pre_app = os.path.join(td, 'private', 'preboot', 'Cryptexes', 'App')
            os.makedirs(os.path.join(pre_os, 'System', 'Library', 'xpc'), exist_ok=True)
            os.makedirs(os.path.join(pre_os, 'System', 'Library', 'Frameworks'), exist_ok=True)
            os.makedirs(os.path.join(pre_app, 'System', 'Applications', 'Safari.app'), exist_ok=True)

            sample_fw = os.path.join(pre_os, 'System', 'Library', 'Frameworks', 'Sample.bin')
            with open(sample_fw, 'wb') as f:
                f.write(b'CRYPTEX_OS_PAYLOAD')
            safari_bin = os.path.join(pre_app, 'System', 'Applications', 'Safari.app', 'Safari')
            with open(safari_bin, 'wb') as f:
                f.write(b'CRYPTEX_APP_SAFARI')
            can_symlink = True
            try:
                os.symlink('Sample.bin', os.path.join(pre_os, 'System', 'Library', 'Frameworks', 'SampleLink'))
            except OSError:
                can_symlink = False

            launchd_seed = {
                'LaunchDaemons': {
                    '/System/Library/LaunchDaemons/com.apple.diskarbitrationd.plist': {
                        'Label': 'com.apple.diskarbitrationd',
                        '_PanicOnCrash': True,
                    },
                    '/System/Library/LaunchDaemons/com.apple.aned.plist': {
                        'Label': 'com.apple.aned',
                    },
                    '/System/Library/LaunchDaemons/com.apple.SpringBoard.plist': {
                        'Label': 'com.apple.SpringBoard',
                        '_PanicOnCrash': True,
                        '_PanicOnNonZeroExit': True,
                    },
                }
            }
            for lp in (
                os.path.join(td, 'System', 'Library', 'xpc', 'launchd.plist'),
                os.path.join(pre_os, 'System', 'Library', 'xpc', 'launchd.plist'),
            ):
                os.makedirs(os.path.dirname(lp), exist_ok=True)
                with open(lp, 'wb') as f:
                    plistlib.dump(launchd_seed, f, fmt=plistlib.FMT_BINARY)

            da_plist = os.path.join(td, 'System', 'Library', 'LaunchDaemons', 'com.apple.diskarbitrationd.plist')
            os.makedirs(os.path.dirname(da_plist), exist_ok=True)
            with open(da_plist, 'wb') as f:
                plistlib.dump({'Label': 'com.apple.diskarbitrationd'}, f, fmt=plistlib.FMT_XML)

            tweaks.ensure_cryptex_paths(td)
            tweaks.patch_launchd(td)
            tweaks.setup_containers(td)

            # Verify /System/Cryptexes/{OS,App} mirrored as real directories with hard-linked payloads
            mirrored_fw = os.path.join(td, 'System', 'Cryptexes', 'OS', 'System', 'Library', 'Frameworks', 'Sample.bin')
            mirrored_link = os.path.join(td, 'System', 'Cryptexes', 'OS', 'System', 'Library', 'Frameworks', 'SampleLink')
            mirrored_safari = os.path.join(td, 'System', 'Cryptexes', 'App', 'System', 'Applications', 'Safari.app', 'Safari')
            self.assertFalse(os.path.islink(os.path.join(td, 'System', 'Cryptexes', 'OS')))
            self.assertTrue(os.path.isfile(mirrored_fw))
            if can_symlink:
                self.assertTrue(os.path.islink(mirrored_link))
                self.assertEqual(os.readlink(mirrored_link), 'Sample.bin')
            self.assertTrue(os.path.isfile(mirrored_safari))

            # Verify diskarbitrationd & aned removed from launchd.plist and SpringBoard panic flags disarmed
            for lp in (
                os.path.join(td, 'System', 'Library', 'xpc', 'launchd.plist'),
                os.path.join(td, 'System', 'Cryptexes', 'OS', 'System', 'Library', 'xpc', 'launchd.plist'),
            ):
                with open(lp, 'rb') as f:
                    pl = plistlib.load(f)
                jobs = pl['LaunchDaemons']
                self.assertNotIn('/System/Library/LaunchDaemons/com.apple.diskarbitrationd.plist', jobs)
                self.assertNotIn('/System/Library/LaunchDaemons/com.apple.aned.plist', jobs)
                sb_job = jobs['/System/Library/LaunchDaemons/com.apple.SpringBoard.plist']
                self.assertNotIn('_PanicOnCrash', sb_job)
                self.assertNotIn('_PanicOnNonZeroExit', sb_job)
                self.assertTrue(sb_job.get('_PanicOnCrasX'))

            with open(da_plist, 'rb') as f:
                self.assertIs(plistlib.load(f).get('Disabled'), True)

            disabled_db = os.path.join(td, 'private', 'var', 'db', 'com.apple.xpc.launchd', 'disabled.plist')
            with open(disabled_db, 'rb') as f:
                db_data = plistlib.load(f)
            self.assertIs(db_data.get('com.apple.diskarbitrationd'), True)

            # Verify setup_containers staged /redwood/etc/metal.args and /redwood/bin/ipados26_depac.py
            staged_args = os.path.join(td, 'redwood', 'etc', 'metal.args')
            staged_depac = os.path.join(td, 'redwood', 'bin', 'ipados26_depac.py')
            self.assertTrue(os.path.isfile(staged_args))
            self.assertTrue(os.path.isfile(staged_depac))
            with open(staged_args, 'r', encoding='utf-8') as f:
                args_txt = f.read()
            self.assertIn('metal_gmem=1', args_txt)
            self.assertIn('metal_gmem_draws=56', args_txt)


class TestPythonToolchainAndCiConfigs(unittest.TestCase):
    """P2-15.5 & P2-11: Syntax-check all tools/ipados26*.py scripts and verify CI toolchain hardening."""

    def test_py_compile_all_ipados26_tools(self):
        scripts = sorted(glob.glob(os.path.join(TOOLS_DIR, 'ipados26*.py')))
        self.assertGreaterEqual(len(scripts), 5)
        scripts.append(os.path.join(TOOLS_DIR, 'adt.py'))
        scripts.append(os.path.abspath(__file__))

        with tempfile.TemporaryDirectory() as td:
            for idx, script in enumerate(scripts):
                with self.subTest(script=os.path.basename(script)):
                    cfile = os.path.join(td, f'compiled_{idx}.pyc')
                    py_compile.compile(script, cfile=cfile, doraise=True)
                    self.assertTrue(os.path.isfile(cfile))

    def test_ci_workflows_have_deterministic_xcode_and_unit_tests(self):
        ci_files = [
            os.path.join(REPO_ROOT, '.github', 'workflows', 'xnu-ipados26.yml'),
            os.path.join(REPO_ROOT, '.github', 'workflows', 'userland-ipados26.yml'),
            os.path.join(REPO_ROOT, 'codemagic.yaml'),
            os.path.join(REPO_ROOT, '.circleci', 'config.yml'),
        ]
        for path in ci_files:
            rel = os.path.relpath(path, REPO_ROOT)
            with self.subTest(ci_file=rel):
                self.assertTrue(os.path.isfile(path), f'Missing CI config: {rel}')
                with open(path, 'r', encoding='utf-8') as f:
                    content = f.read()
                for lineno, line in enumerate(content.splitlines(), 1):
                    self.assertFalse(
                        line.startswith('\t') or re.match(r'^ *\t', line),
                        f'{rel}:{lineno}: YAML indentation must not contain hard tabs',
                    )
                self.assertNotRegex(
                    content, r'^\s*xcode:\s*latest\b',
                    f'{rel}: must pin Xcode deterministically instead of unpinned xcode: latest',
                )
                self.assertIn('DEVELOPER_DIR', content, f'{rel}: must export DEVELOPER_DIR')
                self.assertIn('xcodebuild -version', content, f'{rel}: must log xcodebuild -version')
                self.assertIn('tests/run_unit_tests.py', content, f'{rel}: must run tests/run_unit_tests.py')


if __name__ == '__main__':
    unittest.main(verbosity=2)
