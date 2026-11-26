#!/usr/bin/env python3
"""Build /redwood/share/ca-share/QuartzCore-default.metallib from an extracted
iOS / iPadOS RootFS's QuartzCore.framework/default.pipelinelib/functions file.

CoreAnimation's default.pipelinelib/functions contains concatenated single- or
multi-function MTLB containers (specialized variants of fixed_vert_lph_gen,
fixed_vert_lph_spc, fixed_frag_lph_cpf, fixed_frag_lph_cph, inplace_copy_lph,
inplace_copy_lpf, etc.) across Metallib v1.x, v2.x, and iPadOS 26.1 / Metal 3.2+
/ Metal 4 v3+ containers. HakiosMetalCA.m uses
/redwood/share/ca-share/QuartzCore-default.metallib when QuartzCore.framework
carries no standalone AIR64 default.metallib, then swaps in the exact
pipeline-specific AIR variant via hm_bitcode_for().
"""
import os
import struct
import sys

HEADER_SIZE = 0x58
FAT_MAGIC = 0xCAFEBABE
FAT_MAGIC_64 = 0xCAFEBABF
CPU_TYPE_AIR64 = 0x01000017
BITCODE_WRAPPER_MAGIC = 0x0B17C0DE
RAW_BITCODE_MAGIC = b'BC\xc0\xde'
LARGE_CHUNK_TAGS = frozenset((b'RFLT', b'DEBI', b'BSYM', b'SMTH'))


def _inside(offset, length, size):
    return 0 <= offset <= size and 0 <= length <= size - offset


def _is_fourcc(tag):
    return len(tag) == 4 and all(0x20 <= b <= 0x7E for b in tag)


def thin_metallib(buf):
    """Extract the AIR64 slice from a universal (0xCAFEBABE / 0xCAFEBABF) metallib.

    Returns (thin_bytes_or_None, container_span, is_fat).
    - If buf is not a valid fat container at offset 0: (bytes(buf), len(buf), False).
    - If buf is a valid fat container with an AIR64 slice: (slice_bytes, fat_end, True).
    - If buf is a valid fat container with no AIR64 slice: (None, fat_end, True).
    """
    mv = memoryview(buf)
    n = len(mv)
    if n < 8:
        return bytes(mv), n, False
    magic, count = struct.unpack_from('>II', mv, 0)
    if magic not in (FAT_MAGIC, FAT_MAGIC_64) or count == 0 or count > 64:
        return bytes(mv), n, False
    fat64 = (magic == FAT_MAGIC_64)
    stride = 32 if fat64 else 20
    header_end = 8 + count * stride
    if not _inside(0, header_end, n):
        return bytes(mv), n, False

    fat_end = header_end
    air_slice = None
    for i in range(count):
        at = 8 + i * stride
        cputype = struct.unpack_from('>I', mv, at)[0]
        if fat64:
            offset, length = struct.unpack_from('>QQ', mv, at + 8)
        else:
            offset, length = struct.unpack_from('>II', mv, at + 8)
        if not _inside(offset, length, n):
            return bytes(mv), n, False
        if offset + length > fat_end:
            fat_end = offset + length
        if cputype == CPU_TYPE_AIR64 and air_slice is None:
            air_slice = bytes(mv[offset:offset + length])
    return air_slice, fat_end, True


def unwrap_bitcode(module):
    """Validate and extract raw LLVM bitcode ('BC\\xc0\\xde') from a module payload.

    Handles both Darwin's 0x0B17C0DE bitcode wrapper and bare 'BC\\xc0\\xde' streams
    without integer overflow or out-of-bounds reads. Returns raw bitcode bytes or None.
    """
    mv = memoryview(module)
    n = len(mv)
    if n >= 16 and struct.unpack_from('<I', mv, 0)[0] == BITCODE_WRAPPER_MAGIC:
        bc_off, bc_len = struct.unpack_from('<II', mv, 8)
        if bc_len < 4 or not _inside(bc_off, bc_len, n):
            return None
        inner = bytes(mv[bc_off:bc_off + bc_len])
        if inner[:4] != RAW_BITCODE_MAGIC:
            return None
        return inner
    if n >= 4 and bytes(mv[:4]) == RAW_BITCODE_MAGIC:
        return bytes(mv)
    return None


def _parse_entry_tags_mode(mv, start, end, mode):
    """Parse TLV tags in mv[start:end] under tag-length framing mode 0/1/2."""
    q = start
    name = None
    func_type = 255
    vers = (0, 0, 0, 0)
    mod_off = None
    mod_sz = None
    tags = []
    saw_endt = False

    while _inside(q, 4, end):
        tag = bytes(mv[q:q + 4])
        if not _is_fourcc(tag):
            return None
        if tag == b'ENDT':
            if any(b != 0 for b in mv[q + 4:end]):
                return None
            saw_endt = True
            break
        w = 4 if (mode == 2 or (mode == 1 and tag in LARGE_CHUNK_TAGS)) else 2
        if not _inside(q, 4 + w, end):
            return None
        vlen = struct.unpack_from('<I' if w == 4 else '<H', mv, q + 4)[0]
        if not _inside(q + 4 + w, vlen, end):
            return None
        val = bytes(mv[q + 4 + w:q + 4 + w + vlen])
        if tag == b'NAME':
            stripped = val.rstrip(b'\x00')
            if stripped:
                name = stripped.decode('utf-8', errors='replace')
        elif tag == b'TYPE' and vlen >= 1:
            func_type = val[0]
        elif tag == b'VERS' and vlen >= 4:
            if vlen >= 8:
                vers = struct.unpack_from('<HHHH', val, 0)
            else:
                v0, v1 = struct.unpack_from('<HH', val, 0)
                vers = (v0, v1, 0, 0)
        elif tag == b'OFFT':
            if vlen >= 24:
                mod_off = struct.unpack_from('<Q', val, 16)[0]
            elif vlen >= 12:
                mod_off = struct.unpack_from('<I', val, 8)[0]
        elif tag == b'MDSZ':
            if vlen >= 8:
                mod_sz = struct.unpack_from('<Q', val, 0)[0]
            elif vlen >= 4:
                mod_sz = struct.unpack_from('<I', val, 0)[0]
        tags.append((tag, val))
        q += 4 + w + vlen

    if not saw_endt:
        return None
    return {
        'name': name,
        'type': func_type,
        'vers': vers,
        'mod_off': mod_off,
        'mod_sz': mod_sz,
        'tags': tags,
    }


def parse_metallib(buf):
    """Parse a thin or universal MTLB v1/v2/v3+ container with full bounds checks.

    Returns a dict with container version metadata and extracted AIR functions,
    or None if the container is malformed or holds no AIR64 slice.
    """
    thin, _span, is_fat = thin_metallib(buf)
    if thin is None:
        return None
    mv = memoryview(thin)
    n = len(mv)
    if n < HEADER_SIZE or bytes(mv[:4]) != b'MTLB':
        return None

    raw_platform, fmt_major, fmt_minor = struct.unpack_from('<HHH', mv, 4)
    file_type, target_os, os_major, os_minor = struct.unpack_from('<BBHH', mv, 10)
    if fmt_major == 0 and fmt_minor != 0 and os_major == 0 and os_minor == 0:
        fmt_major = fmt_minor
        fmt_minor = struct.unpack_from('<H', mv, 10)[0]

    file_size = struct.unpack_from('<Q', mv, 0x10)[0]
    if file_size != 0 and (file_size < HEADER_SIZE or file_size > n):
        return None

    funcs_off, funcs_len = struct.unpack_from('<QQ', mv, 0x18)
    pub_off, pub_len = struct.unpack_from('<QQ', mv, 0x28)
    priv_off, priv_len = struct.unpack_from('<QQ', mv, 0x38)
    bc_off, bc_len = struct.unpack_from('<QQ', mv, 0x48)

    if funcs_off < HEADER_SIZE or not _inside(funcs_off, 4, n):
        return None
    if _inside(funcs_off + 4, funcs_len, n):
        dir_end = funcs_off + 4 + funcs_len
    elif funcs_len >= 4 and _inside(funcs_off, funcs_len, n):
        dir_end = funcs_off + funcs_len
    else:
        return None

    if pub_len > 0 and (pub_off < HEADER_SIZE or not _inside(pub_off, pub_len, n)):
        return None
    if priv_len > 0 and (priv_off < HEADER_SIZE or not _inside(priv_off, priv_len, n)):
        return None
    if (bc_len > 0 and bc_off < HEADER_SIZE) or not _inside(bc_off, bc_len, n):
        return None

    count = struct.unpack_from('<I', mv, funcs_off)[0]
    p = funcs_off + 4
    functions = []
    for _ in range(count):
        if not _inside(p, 4, dir_end):
            return None
        entry_len = struct.unpack_from('<I', mv, p)[0]
        if entry_len < 8 or not _inside(p, entry_len, dir_end):
            return None
        end = p + entry_len
        parsed = (
            _parse_entry_tags_mode(mv, p + 4, end, 0)
            or _parse_entry_tags_mode(mv, p + 4, end, 1)
            or _parse_entry_tags_mode(mv, p + 4, end, 2)
        )
        if parsed is None:
            return None
        name = parsed['name']
        mod_off = parsed['mod_off']
        mod_sz = parsed['mod_sz']
        if not name or mod_off is None or mod_sz is None or mod_sz == 0:
            return None
        if not _inside(mod_off, mod_sz, bc_len):
            return None
        abs_off = bc_off + mod_off
        if not _inside(abs_off, mod_sz, n):
            return None
        module = bytes(mv[abs_off:abs_off + mod_sz])
        raw_bc = unwrap_bitcode(module)
        if raw_bc is None:
            return None
        functions.append({
            'name': name,
            'type': parsed['type'],
            'vers': parsed['vers'],
            'tags': parsed['tags'],
            'module': module,
            'bitcode': raw_bc,
        })
        p = end

    return {
        'is_fat': is_fat,
        'platform': raw_platform & 0x7FFF,
        'is_64bit': bool(raw_platform & 0x8000),
        'version': (fmt_major, fmt_minor),
        'file_type': file_type,
        'target_os': target_os,
        'os_version': (os_major, os_minor),
        'header_prefix': bytes(mv[:16]),
        'thin_bytes': bytes(mv),
        'functions': functions,
    }


def iter_concatenated_mtlbs(buf):
    pos = 0
    n = len(buf)
    while pos < n:
        idx_mtlb = buf.find(b'MTLB', pos)
        idx_fat32 = buf.find(b'\xca\xfe\xba\xbe', pos)
        idx_fat64 = buf.find(b'\xca\xfe\xba\xbf', pos)
        candidates = [x for x in (idx_mtlb, idx_fat32, idx_fat64) if x >= 0]
        if not candidates:
            break
        idx = min(candidates)
        if idx != idx_mtlb:
            air_slice, fat_span, is_fat = thin_metallib(memoryview(buf)[idx:])
            if is_fat and fat_span > 0:
                if air_slice is not None and len(air_slice) >= HEADER_SIZE and air_slice[:4] == b'MTLB':
                    yield air_slice
                pos = idx + fat_span
                continue
            pos = idx + 4
            continue

        if not _inside(idx, HEADER_SIZE, n):
            break
        total = struct.unpack_from('<Q', buf, idx + 0x10)[0]
        if total < HEADER_SIZE or not _inside(idx, total, n):
            pos = idx + 4
            continue
        yield bytes(buf[idx:idx + total])
        pos = idx + total


def extract_functions(mtlb):
    info = parse_metallib(mtlb)
    if info is None:
        return []
    return [(fn['name'], fn['tags'], fn['module']) for fn in info['functions']]


def extract_first_function(mtlb):
    funcs = extract_functions(mtlb)
    return funcs[0] if funcs else None


def build_merged_metallib(functions_blob):
    header_prefix = None
    seen = {}
    ordered = []
    for mtlb in iter_concatenated_mtlbs(functions_blob):
        info = parse_metallib(mtlb)
        if info is None or not info['functions']:
            continue
        if header_prefix is None:
            header_prefix = info['header_prefix']
        for fn in info['functions']:
            name = fn['name']
            if name not in seen:
                seen[name] = True
                ordered.append((name, fn['tags'], fn['module']))

    if not ordered or header_prefix is None:
        return None, 0

    entries_buf = bytearray()
    modules_buf = bytearray()
    pub_buf = struct.pack('<I', 0)
    priv_buf = struct.pack('<I', 0)

    for _name, tags, module in ordered:
        cur_bc_off = len(modules_buf)
        modules_buf.extend(module)
        body = bytearray()
        for tag, val in tags:
            if tag == b'SOFF':
                continue
            if tag == b'OFFT':
                val = struct.pack('<QQQ', 0, 0, cur_bc_off)
            elif tag == b'MDSZ':
                val = struct.pack('<Q', len(module))
            if len(val) > 0xFFFF:
                continue
            body.extend(tag)
            body.extend(struct.pack('<H', len(val)))
            body.extend(val)
        body.extend(b'ENDT')
        entries_buf.extend(struct.pack('<I', 4 + len(body)))
        entries_buf.extend(body)

    funcs_off = HEADER_SIZE
    funcs_len = len(entries_buf)
    pub_off = funcs_off + 4 + funcs_len
    pub_len = len(pub_buf)
    priv_off = pub_off + pub_len
    priv_len = len(priv_buf)
    bc_off = priv_off + priv_len
    bc_len = len(modules_buf)
    total_size = bc_off + bc_len

    out = bytearray()
    out.extend(header_prefix)
    out.extend(struct.pack('<Q', total_size))
    out.extend(struct.pack('<QQ', funcs_off, funcs_len))
    out.extend(struct.pack('<QQ', pub_off, pub_len))
    out.extend(struct.pack('<QQ', priv_off, priv_len))
    out.extend(struct.pack('<QQ', bc_off, bc_len))
    assert len(out) == HEADER_SIZE
    out.extend(struct.pack('<I', len(ordered)))
    out.extend(entries_buf)
    out.extend(pub_buf)
    out.extend(priv_buf)
    out.extend(modules_buf)
    assert len(out) == total_size
    return bytes(out), len(ordered)


def generate_ca_metallib(root):
    candidates = [
        os.path.join(root, 'System/Library/Frameworks/QuartzCore.framework'),
        os.path.join(root, 'private/preboot/Cryptexes/OS/System/Library/Frameworks/QuartzCore.framework'),
        os.path.join(root, 'System/Cryptexes/OS/System/Library/Frameworks/QuartzCore.framework'),
    ]
    qc_dir = next((c for c in candidates if os.path.isdir(c)), None)
    if not qc_dir:
        return False
    out_path = os.path.join(root, 'redwood/share/ca-share/QuartzCore-default.metallib')
    default_ml = os.path.join(qc_dir, 'default.metallib')
    if os.path.isfile(default_ml):
        with open(default_ml, 'rb') as src:
            raw_default = src.read()
        info = parse_metallib(raw_default)
        if info is not None and info['functions']:
            os.makedirs(os.path.dirname(out_path), exist_ok=True)
            with open(out_path, 'wb') as dst:
                dst.write(info['thin_bytes'])
            os.chmod(out_path, 0o644)
            print(f'QuartzCore default.metallib copied -> {os.path.relpath(out_path, root)}')
            return True

    funcs_path = os.path.join(qc_dir, 'default.pipelinelib/functions')
    if not os.path.isfile(funcs_path):
        return False
    with open(funcs_path, 'rb') as f:
        blob, count = build_merged_metallib(f.read())
    if not blob:
        return False
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'wb') as f:
        f.write(blob)
    os.chmod(out_path, 0o644)
    print(f'Built {os.path.relpath(out_path, root)}: {count} unique functions ({len(blob)} bytes)')
    return True


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('usage: ipados26_ca_metallib.py <MOUNTED_ROOT>')
    generate_ca_metallib(sys.argv[1])
