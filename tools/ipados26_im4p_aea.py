#!/usr/bin/env python3
"""Pure-Python IM4P (ASN.1 + LZFSE/LZVN/PBZX/PBZE) extractor & AEA HPKE key unwrapper for iPadOS 26.1.

Usage:
  ipados26_im4p_aea.py im4p <input.im4p> <output.bin>
  ipados26_im4p_aea.py pbzx <input.pbzx_or_im4p> <output.bin>
  ipados26_im4p_aea.py aea-key <input.dmg.aea>
"""
import base64
import hashlib
import hmac
import json
import lzma
import os
import struct
import subprocess
import sys
import zlib

# ============================================================================
# Part 1: Pure-Python LZFSE (bvx1 / bvx2 / bvx-) + LZVN (bvxn) Decompressor
# ============================================================================

LZFSE_ENCODE_L_SYMBOLS = 20
LZFSE_ENCODE_M_SYMBOLS = 20
LZFSE_ENCODE_D_SYMBOLS = 64
LZFSE_ENCODE_LITERAL_SYMBOLS = 256
LZFSE_ENCODE_L_STATES = 64
LZFSE_ENCODE_M_STATES = 64
LZFSE_ENCODE_D_STATES = 256
LZFSE_ENCODE_LITERAL_STATES = 1024

L_EXTRA_BITS = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 5, 8]
L_BASE_VALUE = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 20, 28, 60]

M_EXTRA_BITS = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 5, 8, 11]
M_BASE_VALUE = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 24, 56, 312]

D_EXTRA_BITS = [
    0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7,
    8, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 11, 11, 11, 11,
    12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15,
]
D_BASE_VALUE = [
    0, 1, 2, 3, 4, 6, 8, 10, 12, 16, 20, 24, 28, 36, 44, 52,
    60, 76, 92, 108, 124, 156, 188, 220, 252, 316, 380, 444, 508, 636, 764, 892,
    1020, 1276, 1532, 1788, 2044, 2556, 3068, 3580, 4092, 5116, 6140, 7164, 8188, 10236, 12284, 14332,
    16380, 20476, 24572, 28668, 32764, 40956, 49148, 57340, 65532, 81916, 98300, 114684, 131068, 163836, 196604, 229372,
]

FREQ_NB_BITS = [2, 3, 2, 5, 2, 3, 2, 8, 2, 3, 2, 5, 2, 3, 2, 14,
                2, 3, 2, 5, 2, 3, 2, 8, 2, 3, 2, 5, 2, 3, 2, 14]
FREQ_VALUE = [0, 2, 1, 4, 0, 3, 1, -1, 0, 2, 1, 5, 0, 3, 1, -1,
              0, 2, 1, 6, 0, 3, 1, -1, 0, 2, 1, 7, 0, 3, 1, -1]


def _clz32(x):
    return 32 - x.bit_length() if x > 0 else 32


def _build_fse_decoder(nstates, nsymbols, freq):
    table = []
    n_clz = _clz32(nstates)
    for i in range(nsymbols):
        f = freq[i]
        if f == 0:
            continue
        k = _clz32(f) - n_clz
        j0 = ((2 * nstates) >> k) - f
        for j in range(f):
            if j < j0:
                bits = k
                delta = ((f + j) << k) - nstates
            else:
                bits = k - 1
                delta = (j - j0) << (k - 1)
            table.append((bits, i, delta))
    assert len(table) == nstates, f"FSE table size {len(table)} != {nstates}"
    return table


def _build_v_fse_decoder(nstates, nsymbols, freq, vbits_table, vbase_table):
    table = []
    n_clz = _clz32(nstates)
    for i in range(nsymbols):
        f = freq[i]
        if f == 0:
            continue
        k = _clz32(f) - n_clz
        j0 = ((2 * nstates) >> k) - f
        vb = vbits_table[i]
        vbase = vbase_table[i]
        for j in range(f):
            if j < j0:
                kbits = k
                delta = ((f + j) << k) - nstates
            else:
                kbits = k - 1
                delta = (j - j0) << (k - 1)
            table.append((kbits + vb, vb, delta, vbase))
    assert len(table) == nstates, f"VFSE table size {len(table)} != {nstates}"
    return table


class _BackwardBitStream:
    """Exact Apple LZFSE fse_in_stream64 (lzfse_fse.h) where buf_start = block start.

    Apple's FSE encoder flushes 64-bit accumulators from the end of each stream backwards,
    and fse_in_checked_init64/flush64 sets buf_start = s->src_begin (start of the block
    header, NOT payload_start) so the final dummy state update after the last decoded
    symbol may shift in up to 7 preceding header bytes without failing.
    """
    def __init__(self, block_to_stream_end, bit_offset):
        self.bits = int.from_bytes(block_to_stream_end, 'little')
        self.pos = len(block_to_stream_end) * 8 + bit_offset

    def pull(self, n):
        if n == 0:
            return 0
        self.pos -= n
        if self.pos < 0:
            raise ValueError(f"FSE bitstream underflow ({self.pos})")
        return (self.bits >> self.pos) & ((1 << n) - 1)


def _unpack_v2_freq_table(header_bytes):
    bitbuf = int.from_bytes(header_bytes, 'little')
    bitpos = 0
    total_syms = (LZFSE_ENCODE_L_SYMBOLS + LZFSE_ENCODE_M_SYMBOLS +
                  LZFSE_ENCODE_D_SYMBOLS + LZFSE_ENCODE_LITERAL_SYMBOLS)
    freqs = []
    while len(freqs) < total_syms:
        b5 = (bitbuf >> bitpos) & 0x1F
        nb = FREQ_NB_BITS[b5]
        val = FREQ_VALUE[b5]
        if nb == 8:
            val = 8 + ((bitbuf >> (bitpos + 4)) & 0xF)
        elif nb == 14:
            val = 24 + ((bitbuf >> (bitpos + 4)) & 0x3FF)
        bitpos += nb
        freqs.append(val)
    i0 = LZFSE_ENCODE_L_SYMBOLS
    i1 = i0 + LZFSE_ENCODE_M_SYMBOLS
    i2 = i1 + LZFSE_ENCODE_D_SYMBOLS
    return freqs[:i0], freqs[i0:i1], freqs[i1:i2], freqs[i2:]


def _decode_lzfse_block(buf, pos, magic, out):
    out_start = len(out)
    if magic == b'bvx1':
        (n_raw_bytes, n_payload_bytes, n_literals, n_matches,
         n_literal_payload_bytes, n_lmd_payload_bytes,
         literal_bits, l0, l1, l2, l3,
         lmd_bits, l_st, m_st, d_st) = struct.unpack_from('<IIIIIIiHHHHiHHH', buf, pos + 4)
        hdr_off = pos + 50
        l_freq = list(struct.unpack_from(f'<{LZFSE_ENCODE_L_SYMBOLS}H', buf, hdr_off)); hdr_off += 40
        m_freq = list(struct.unpack_from(f'<{LZFSE_ENCODE_M_SYMBOLS}H', buf, hdr_off)); hdr_off += 40
        d_freq = list(struct.unpack_from(f'<{LZFSE_ENCODE_D_SYMBOLS}H', buf, hdr_off)); hdr_off += 128
        lit_freq = list(struct.unpack_from(f'<{LZFSE_ENCODE_LITERAL_SYMBOLS}H', buf, hdr_off)); hdr_off += 512
        payload_start = hdr_off
    else:  # bvx2 (apple/lzfse lzfse_compressed_block_header_v2)
        n_raw_bytes, v0, v1, v2 = struct.unpack_from('<IQQQ', buf, pos + 4)
        n_literals = (v0 >> 0) & 0xFFFFF
        n_literal_payload_bytes = (v0 >> 20) & 0xFFFFF
        n_matches = (v0 >> 40) & 0xFFFFF
        literal_bits = ((v0 >> 60) & 0x7) - 7  # signed -7..0
        l0 = (v1 >> 0) & 0x3FF
        l1 = (v1 >> 10) & 0x3FF
        l2 = (v1 >> 20) & 0x3FF
        l3 = (v1 >> 30) & 0x3FF
        n_lmd_payload_bytes = (v1 >> 40) & 0xFFFFF
        lmd_bits = ((v1 >> 60) & 0x7) - 7
        header_size = (v2 >> 0) & 0xFFFFFFFF
        l_st = (v2 >> 32) & 0x3FF
        m_st = (v2 >> 42) & 0x3FF
        d_st = (v2 >> 52) & 0x3FF
        l_freq, m_freq, d_freq, lit_freq = _unpack_v2_freq_table(buf[pos + 32:pos + header_size])
        payload_start = pos + header_size

    lit_dec = _build_fse_decoder(LZFSE_ENCODE_LITERAL_STATES, LZFSE_ENCODE_LITERAL_SYMBOLS, lit_freq)
    # Match apple/lzfse lzfse_decode_base.c: buf_start is pos (block header start)
    lit_end = payload_start + n_literal_payload_bytes
    lit_bs = _BackwardBitStream(buf[pos:lit_end], literal_bits)
    literals = bytearray(n_literals + 4)
    s0, s1, s2, s3 = l0, l1, l2, l3
    for i in range(0, n_literals, 4):
        kb, sym, dl = lit_dec[s0]; s0 = dl + lit_bs.pull(kb); literals[i] = sym
        kb, sym, dl = lit_dec[s1]; s1 = dl + lit_bs.pull(kb); literals[i + 1] = sym
        kb, sym, dl = lit_dec[s2]; s2 = dl + lit_bs.pull(kb); literals[i + 2] = sym
        kb, sym, dl = lit_dec[s3]; s3 = dl + lit_bs.pull(kb); literals[i + 3] = sym

    l_dec = _build_v_fse_decoder(LZFSE_ENCODE_L_STATES, LZFSE_ENCODE_L_SYMBOLS, l_freq, L_EXTRA_BITS, L_BASE_VALUE)
    m_dec = _build_v_fse_decoder(LZFSE_ENCODE_M_STATES, LZFSE_ENCODE_M_SYMBOLS, m_freq, M_EXTRA_BITS, M_BASE_VALUE)
    d_dec = _build_v_fse_decoder(LZFSE_ENCODE_D_STATES, LZFSE_ENCODE_D_SYMBOLS, d_freq, D_EXTRA_BITS, D_BASE_VALUE)

    lmd_start = payload_start + n_literal_payload_bytes
    lmd_end = lmd_start + n_lmd_payload_bytes
    lmd_bs = _BackwardBitStream(buf[pos:lmd_end], lmd_bits)

    lit_idx = 0
    prev_d = -1
    ls, ms, ds = l_st, m_st, d_st
    for _ in range(n_matches):
        tb, vb, dl, vbase = l_dec[ls]
        bits = lmd_bs.pull(tb)
        ls = dl + (bits >> vb)
        L = vbase + (bits & ((1 << vb) - 1))

        tb, vb, dl, vbase = m_dec[ms]
        bits = lmd_bs.pull(tb)
        ms = dl + (bits >> vb)
        M = vbase + (bits & ((1 << vb) - 1))

        tb, vb, dl, vbase = d_dec[ds]
        bits = lmd_bs.pull(tb)
        ds = dl + (bits >> vb)
        D = vbase + (bits & ((1 << vb) - 1))
        if D == 0:
            D = prev_d
        else:
            prev_d = D

        if L:
            out.extend(literals[lit_idx:lit_idx + L])
            lit_idx += L
        if M:
            start = len(out) - D
            assert start >= 0, f"invalid LZFSE match distance D={D} at out={len(out)}"
            if D >= M:
                out.extend(out[start:start + M])
            else:
                for k in range(M):
                    out.append(out[start + k])

    produced = len(out) - out_start
    if produced < n_raw_bytes and lit_idx < n_literals:
        need = min(n_raw_bytes - produced, n_literals - lit_idx)
        out.extend(literals[lit_idx:lit_idx + need])
        produced = len(out) - out_start

    assert produced == n_raw_bytes, f"LZFSE block output {produced} != {n_raw_bytes}"
    next_pos = lmd_start + n_lmd_payload_bytes
    return next_pos


def _decode_lzvn(src, n_raw, out):
    out_start = len(out)
    i = 0
    prev_d = 0
    while i < len(src) and (len(out) - out_start) < n_raw:
        opc = src[i]
        if opc == 0x06:  # EOS
            break
        elif opc in (0x0E, 0x16, 0x1E, 0x26, 0x2E, 0x36, 0x3E):  # NOP
            i += 1
            continue
        elif opc == 0xE0:  # large L
            L = src[i + 1] + 16
            i += 2
            out.extend(src[i:i + L]); i += L
            continue
        elif (opc & 0xF0) == 0xE0:  # small L
            L = opc & 0x0F
            i += 1
            out.extend(src[i:i + L]); i += L
            continue
        elif opc == 0xF0:  # large M, prev D
            M = src[i + 1] + 16
            i += 2
            L, D = 0, prev_d
        elif (opc & 0xF0) == 0xF0:  # small M, prev D
            M = opc & 0x0F
            i += 1
            L, D = 0, prev_d
        elif (opc & 0x07) == 0x07:  # large D (3 bytes)
            b1, b2 = src[i + 1], src[i + 2]
            L = (opc >> 6) & 0x3
            M = ((opc >> 3) & 0x7) + 3
            D = (b2 << 8) | b1
            prev_d = D
            i += 3
        elif (opc & 0x07) == 0x06:  # prev D (1 byte: 01/10/11 top bits)
            L = (opc >> 6) & 0x3
            M = ((opc >> 3) & 0x7) + 3
            D = prev_d
            i += 1
        elif (opc & 0xE0) == 0xA0:  # medium D (3 bytes: 101LLMMM DDDDDDMM DDDDDDDD)
            b1, b2 = src[i + 1], src[i + 2]
            L = (opc >> 3) & 0x3
            M = (((opc & 0x7) << 2) | (b1 & 0x3)) + 3
            D = (b2 << 6) | (b1 >> 2)
            prev_d = D
            i += 3
        elif (opc & 0xC0) == 0xC0 or (opc & 0xE0) == 0x80:
            raise ValueError(f"reserved LZVN opcode 0x{opc:02x}")
        else:  # small D (2 bytes: LLMMMDDD DDDDDDDD)
            b1 = src[i + 1]
            L = (opc >> 6) & 0x3
            M = ((opc >> 3) & 0x7) + 3
            D = ((opc & 0x7) << 8) | b1
            prev_d = D
            i += 2

        if L:
            out.extend(src[i:i + L]); i += L
        if M:
            start = len(out) - D
            for k in range(M):
                out.append(out[start + k])


def lzfse_decompress(buf):
    pos = 0
    out = bytearray()
    while pos < len(buf):
        magic = buf[pos:pos + 4]
        if magic == b'bvx$':
            break
        elif magic in (b'bvx1', b'bvx2'):
            pos = _decode_lzfse_block(buf, pos, magic, out)
        elif magic == b'bvxn':
            n_raw, n_pay = struct.unpack_from('<II', buf, pos + 4)
            _decode_lzvn(buf[pos + 12:pos + 12 + n_pay], n_raw, out)
            pos += 12 + n_pay
        elif magic == b'bvx-':
            n_raw, = struct.unpack_from('<I', buf, pos + 4)
            out.extend(buf[pos + 8:pos + 8 + n_raw])
            pos += 8 + n_raw
        else:
            raise ValueError(f"Unknown LZFSE block magic {magic!r} at {pos}")
    return bytes(out)


# ============================================================================
# Part 2: ASN.1 DER IM4P Parser
# ============================================================================

def _asn1_read_tlv(data, pos):
    tag = data[pos]; pos += 1
    length = data[pos]; pos += 1
    if length & 0x80:
        nbytes = length & 0x7F
        length = int.from_bytes(data[pos:pos + nbytes], 'big')
        pos += nbytes
    return tag, data[pos:pos + length], pos + length


def decompress_pbzx(data, max_chunks=None):
    """Decompress Apple PBZX / PBZE chunked streams (LZFSE, XZ/LZMA, zlib, or raw chunks)."""
    magic = data[:4]
    if magic not in (b'pbzx', b'pbze'):
        raise ValueError(f"Expected pbzx/pbze magic, got {magic!r}")
    _flags, = struct.unpack_from('>Q', data, 4)
    pos = 12
    out = bytearray()
    chunks = 0
    while pos + 16 <= len(data):
        u_sz, c_sz = struct.unpack_from('>QQ', data, pos)
        pos += 16
        chunk = data[pos:pos + c_sz]
        pos += c_sz
        if c_sz == u_sz:
            out.extend(chunk)
        elif chunk[:4] in (b'bvx1', b'bvx2', b'bvxn', b'bvx-'):
            dec = lzfse_decompress(chunk)
            assert len(dec) == u_sz, f"PBZE LZFSE chunk size {len(dec)} != {u_sz}"
            out.extend(dec)
        elif chunk[:6] == b'\xfd7zXZ\x00':
            dec = lzma.decompress(chunk)
            assert len(dec) == u_sz, f"PBZX XZ chunk size {len(dec)} != {u_sz}"
            out.extend(dec)
        else:
            dec = zlib.decompress(chunk)
            assert len(dec) == u_sz, f"PBZX zlib chunk size {len(dec)} != {u_sz}"
            out.extend(dec)
        chunks += 1
        if max_chunks is not None and chunks >= max_chunks:
            break
    return bytes(out)


def extract_im4p(im4p_bytes):
    tag, seq, _ = _asn1_read_tlv(im4p_bytes, 0)
    assert tag == 0x30, "Not an ASN.1 SEQUENCE"
    p = 0
    _, magic, p = _asn1_read_tlv(seq, p)
    assert magic == b'IM4P', f"Expected IM4P, got {magic!r}"
    _, fourcc, p = _asn1_read_tlv(seq, p)
    _, desc, p = _asn1_read_tlv(seq, p)
    _, payload, p = _asn1_read_tlv(seq, p)
    if payload[:4] in (b'bvx1', b'bvx2', b'bvxn', b'bvx-'):
        payload = lzfse_decompress(payload)
    elif payload[:4] in (b'pbzx', b'pbze'):
        payload = decompress_pbzx(payload)
    return fourcc.decode('ascii', 'replace'), desc.decode('ascii', 'replace'), payload


# ============================================================================
# Part 3: Pure-Python NIST P-256 + RFC 9180 HPKE + AES-256-GCM AEA Key Unwrap
# ============================================================================

# NIST P-256 parameters
_P256_P = 0xffffffff00000001000000000000000000000000ffffffffffffffffffffffff
_P256_A = _P256_P - 3
_P256_B = 0x5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b
_P256_GX = 0x6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296
_P256_GY = 0x4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5


def _p256_add(P, Q):
    if P is None: return Q
    if Q is None: return P
    x1, y1 = P; x2, y2 = Q
    if x1 == x2:
        if (y1 + y2) % _P256_P == 0:
            return None
        lam = (3 * x1 * x1 + _P256_A) * pow(2 * y1, -1, _P256_P) % _P256_P
    else:
        lam = (y2 - y1) * pow(x2 - x1, -1, _P256_P) % _P256_P
    x3 = (lam * lam - x1 - x2) % _P256_P
    y3 = (lam * (x1 - x3) - y1) % _P256_P
    return (x3, y3)


def _p256_mul(k, P):
    R = None
    Q = P
    while k > 0:
        if k & 1:
            R = _p256_add(R, Q)
        Q = _p256_add(Q, Q)
        k >>= 1
    return R


# Minimal Pure-Python AES-256 + GCM (for 32-byte key unwrap only!)
_SBOX = [
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
]


def _xtime(a):
    return (((a << 1) ^ 0x1b) & 0xff) if (a & 0x80) else (a << 1)


def _aes256_expand_key(key):
    w = list(struct.unpack('>8I', key))
    rcon = 0x01000000
    for i in range(8, 60):
        t = w[i - 1]
        if i % 8 == 0:
            t = ((_SBOX[(t >> 16) & 0xff] << 24) | (_SBOX[(t >> 8) & 0xff] << 16) |
                 (_SBOX[t & 0xff] << 8) | _SBOX[(t >> 24) & 0xff]) ^ rcon
            rcon = (_xtime(rcon >> 24)) << 24
        elif i % 8 == 4:
            t = ((_SBOX[(t >> 24) & 0xff] << 24) | (_SBOX[(t >> 16) & 0xff] << 16) |
                 (_SBOX[(t >> 8) & 0xff] << 8) | _SBOX[t & 0xff])
        w.append(w[i - 8] ^ t)
    return [struct.pack('>4I', *w[r * 4:(r + 1) * 4]) for r in range(15)]


def _aes256_encrypt_block(rk, block):
    s = [b ^ k for b, k in zip(block, rk[0])]
    for r in range(1, 15):
        s = [_SBOX[b] for b in s]
        s = [s[0], s[5], s[10], s[15], s[4], s[9], s[14], s[3],
             s[8], s[13], s[2], s[7], s[12], s[1], s[6], s[11]]
        if r < 14:
            ns = [0] * 16
            for c in range(4):
                i = c * 4
                a0, a1, a2, a3 = s[i], s[i + 1], s[i + 2], s[i + 3]
                t = a0 ^ a1 ^ a2 ^ a3
                ns[i] = a0 ^ t ^ _xtime(a0 ^ a1)
                ns[i + 1] = a1 ^ t ^ _xtime(a1 ^ a2)
                ns[i + 2] = a2 ^ t ^ _xtime(a2 ^ a3)
                ns[i + 3] = a3 ^ t ^ _xtime(a3 ^ a0)
            s = ns
        s = [b ^ k for b, k in zip(s, rk[r])]
    return bytes(s)


def _ghash_mul(x, y):
    z = 0
    v = y
    for i in range(127, -1, -1):
        if (x >> i) & 1:
            z ^= v
        if v & 1:
            v = (v >> 1) ^ 0xe1000000000000000000000000000000
        else:
            v >>= 1
    return z


def _aes256_gcm_decrypt(key, nonce, ct_and_tag, aad=b''):
    assert len(nonce) == 12 and len(ct_and_tag) >= 16
    ct, tag = ct_and_tag[:-16], ct_and_tag[-16:]
    rk = _aes256_expand_key(key)
    H = int.from_bytes(_aes256_encrypt_block(rk, b'\x00' * 16), 'big')
    j0 = nonce + b'\x00\x00\x00\x01'

    # GHASH over aad || ct
    def pad16(b):
        return b + b'\x00' * ((16 - len(b) % 16) % 16)
    gdata = pad16(aad) + pad16(ct) + struct.pack('>QQ', len(aad) * 8, len(ct) * 8)
    y = 0
    for i in range(0, len(gdata), 16):
        blk = int.from_bytes(gdata[i:i + 16], 'big')
        y = _ghash_mul(y ^ blk, H)
    calc_tag = (y ^ int.from_bytes(_aes256_encrypt_block(rk, j0), 'big')).to_bytes(16, 'big')
    if not hmac.compare_digest(calc_tag, tag):
        raise ValueError("AES-256-GCM tag mismatch during HPKE open")

    pt = bytearray()
    ctr = 2
    for i in range(0, len(ct), 16):
        ks = _aes256_encrypt_block(rk, nonce + struct.pack('>I', ctr))
        chunk = ct[i:i + 16]
        pt.extend(b ^ k for b, k in zip(chunk, ks))
        ctr += 1
    return bytes(pt)


def _hkdf_extract(salt, ikm):
    return hmac.new(salt if salt else b'\x00' * 32, ikm, hashlib.sha256).digest()


def _hkdf_expand(prk, info, length):
    out = b''
    t = b''
    c = 1
    while len(out) < length:
        t = hmac.new(prk, t + info + bytes([c]), hashlib.sha256).digest()
        out += t
        c += 1
    return out[:length]


def _labeled_extract(suite_id, salt, label, ikm):
    return _hkdf_extract(salt, b'HPKE-v1' + suite_id + label + ikm)


def _labeled_expand(suite_id, prk, label, info, length):
    return _hkdf_expand(prk, struct.pack('>H', length) + b'HPKE-v1' + suite_id + label + info, length)


def _parse_ec_p256_pem(pem_text):
    lines = [l.strip() for l in pem_text.splitlines() if l.strip() and not l.startswith('-----')]
    der = base64.b64decode(''.join(lines))
    # PKCS#8 PrivateKeyInfo -> OCTET STRING ECPrivateKey -> OCTET STRING (32-byte d) + [1] BIT STRING (65-byte pub)
    _, seq, _ = _asn1_read_tlv(der, 0)
    p = 0
    _, _ver, p = _asn1_read_tlv(seq, p)
    _, _alg, p = _asn1_read_tlv(seq, p)
    _, ec_priv_der, p = _asn1_read_tlv(seq, p)
    _, ec_seq, _ = _asn1_read_tlv(ec_priv_der, 0)
    q = 0
    _, _ec_ver, q = _asn1_read_tlv(ec_seq, q)
    _, d_bytes, q = _asn1_read_tlv(ec_seq, q)
    d = int.from_bytes(d_bytes, 'big')
    pub = _p256_mul(d, (_P256_GX, _P256_GY))
    pk_rm = b'\x04' + pub[0].to_bytes(32, 'big') + pub[1].to_bytes(32, 'big')
    return d, pk_rm


def unwrap_aea_fcs_key(fcs_resp_json, pem_text):
    resp = json.loads(fcs_resp_json) if isinstance(fcs_resp_json, (str, bytes)) else fcs_resp_json
    enc = base64.b64decode(resp['enc-request'])
    wrapped = base64.b64decode(resp['wrapped-key'])
    sk_r, pk_rm = _parse_ec_p256_pem(pem_text)

    assert len(enc) == 65 and enc[0] == 0x04
    epk = (int.from_bytes(enc[1:33], 'big'), int.from_bytes(enc[33:65], 'big'))
    z_pt = _p256_mul(sk_r, epk)
    dh = z_pt[0].to_bytes(32, 'big')

    # RFC 9180 DHKEM(P-256, HKDF-SHA256) kem_id = 0x0010
    kem_suite = b'KEM' + struct.pack('>H', 0x0010)
    kem_context = enc + pk_rm
    eae_prk = _labeled_extract(kem_suite, b'', b'eae_prk', dh)
    shared_secret = _labeled_expand(kem_suite, eae_prk, b'shared_secret', kem_context, 32)

    # RFC 9180 HPKE Base Mode: kem=0x0010, kdf=0x0001 (HKDF-SHA256), aead=0x0002 (AES-256-GCM)
    hpke_suite = b'HPKE' + struct.pack('>HHH', 0x0010, 0x0001, 0x0002)
    mode = b'\x00'
    psk_id_hash = _labeled_extract(hpke_suite, b'', b'psk_id_hash', b'')
    info_hash = _labeled_extract(hpke_suite, b'', b'info_hash', b'')
    ks_context = mode + psk_id_hash + info_hash
    secret = _labeled_extract(hpke_suite, shared_secret, b'secret', b'')
    key = _labeled_expand(hpke_suite, secret, b'key', ks_context, 32)
    base_nonce = _labeled_expand(hpke_suite, secret, b'base_nonce', ks_context, 12)
    return _aes256_gcm_decrypt(key, base_nonce, wrapped, aad=b'')


def parse_aea_auth_data(aea_header_bytes):
    assert aea_header_bytes[:4] == b'AEA1', "Not an AEA1 archive"
    auth_len, = struct.unpack_from('<I', aea_header_bytes, 8)
    auth = aea_header_bytes[12:12 + auth_len]
    meta = {}
    k = 0
    while k < len(auth):
        kvlen, = struct.unpack_from('<I', auth, k)
        kv = auth[k + 4:k + kvlen]
        sep = kv.find(b'\x00')
        meta[kv[:sep].decode('utf-8', 'replace')] = kv[sep + 1:].decode('utf-8', 'replace')
        k += kvlen
    return meta


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    if cmd == 'im4p':
        im4p_path, out_path = sys.argv[2], sys.argv[3]
        fourcc, desc, payload = extract_im4p(open(im4p_path, 'rb').read())
        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        with open(out_path, 'wb') as f:
            f.write(payload)
        print(f"{im4p_path} -> {out_path}: fourcc={fourcc!r} desc={desc!r} ({len(payload)} bytes)")
    elif cmd == 'pbzx':
        in_path, out_path = sys.argv[2], sys.argv[3]
        raw = open(in_path, 'rb').read()
        payload = extract_im4p(raw)[2] if raw[:1] == b'\x30' else decompress_pbzx(raw)
        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        with open(out_path, 'wb') as f:
            f.write(payload)
        print(f"{in_path} -> {out_path}: ({len(payload)} bytes)")
    elif cmd == 'aea-key':
        src = sys.argv[2]
        if os.path.exists(src):
            with open(src, 'rb') as f:
                hdr = f.read(16384)
        else:
            raise FileNotFoundError(src)
        meta = parse_aea_auth_data(hdr)
        key_url = meta['com.apple.wkms.fcs-key-url']
        pem = subprocess.run(['curl', '-ksfL', key_url], check=True, capture_output=True).stdout.decode()
        raw_key = unwrap_aea_fcs_key(meta['com.apple.wkms.fcs-response'], pem)
        print(f"base64:{base64.b64encode(raw_key).decode()}")
        print(f"hex:{raw_key.hex()}", file=sys.stderr)
    else:
        sys.exit(f"Unknown command: {cmd}")


if __name__ == '__main__':
    main()
