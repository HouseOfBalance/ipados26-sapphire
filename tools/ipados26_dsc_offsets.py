#!/usr/bin/env python3
"""Offline dyld_shared_cache_arm64e & Mach-O symbol/vtable scanner for iPadOS 26.1.

Extracts exact unslid shared-cache / binary addresses needed by Sapphire userland bridges
(e.g. QuartzCore CA::OGL::GLESContext vtable & GLContext::function_supported/function_uniform
for eagl.dylib / libredwood-gl.dylib) and writes /redwood/etc/sapphire-offsets.conf.

Supports dyld_v1 arm64e split subcaches (.01, .02, ..., .symbols) with slide info v3 & v5!

Usage:
  python3 tools/ipados26_dsc_offsets.py <MOUNTED_ROOT_OR_DSC_PATH> [OUT_CONF]
"""
import glob
import os
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x02
LC_DYLD_EXPORTS_TRIE = 0x80000033


def _read_uleb128(buf, pos, end):
    val = 0
    shift = 0
    while pos < end:
        b = buf[pos]
        pos += 1
        val |= (b & 0x7F) << shift
        if (b & 0x80) == 0:
            return val, pos
        shift += 7
    return val, pos


def _walk_exports_trie(buf, start, end, target_names):
    """Iterative walk of LC_DYLD_EXPORTS_TRIE looking for exact symbol names."""
    found = {}
    if start >= end or end > len(buf):
        return found
    stack = [(start, "")]
    visited = set()
    while stack and len(found) < len(target_names):
        node_off, prefix = stack.pop()
        if node_off in visited or node_off < start or node_off >= end:
            continue
        visited.add(node_off)
        term_size, p = _read_uleb128(buf, node_off, end)
        children_pos = p + term_size
        if term_size > 0:
            flags, q = _read_uleb128(buf, p, children_pos)
            # Regular / weak-def export (not re-export 0x08, not stub-and-resolver 0x10)
            if (flags & 0x08) == 0:
                if flags & 0x10:
                    _stub, q = _read_uleb128(buf, q, children_pos)
                    sym_off, _ = _read_uleb128(buf, q, children_pos)
                else:
                    sym_off, _ = _read_uleb128(buf, q, children_pos)
                if prefix in target_names:
                    found[prefix] = sym_off
        if children_pos >= end:
            continue
        n_children = buf[children_pos]
        cpos = children_pos + 1
        for _ in range(n_children):
            nul = buf.find(b'\x00', cpos, end)
            if nul < 0:
                break
            edge = buf[cpos:nul].decode('ascii', 'replace')
            cpos = nul + 1
            child_rel, cpos = _read_uleb128(buf, cpos, end)
            next_prefix = prefix + edge
            # Only descend if next_prefix can still match at least one target
            if any(t.startswith(next_prefix) or next_prefix.startswith(t) for t in target_names):
                stack.append((start + child_rel, next_prefix))
    return found


class DyldSharedCacheReader:
    def __init__(self, main_path):
        self.main_path = main_path
        self.files = []       # list of open binary files
        self.mappings = []    # list of (vmaddr, size, file_idx, file_off, slide_ver, value_add)
        self._open_all()

    def close(self):
        for f in self.files:
            f.close()
        self.files.clear()

    def _add_cache_file(self, path):
        if not os.path.isfile(path):
            return None
        f = open(path, 'rb')
        hdr = f.read(512)
        if not hdr.startswith(b'dyld_v1 '):
            f.close()
            return None
        idx = len(self.files)
        self.files.append(f)
        mapping_off, mapping_cnt = struct.unpack_from('<II', hdr, 16)

        # Check mappingWithSlideOffset (offset 0x138 = 312 in modern dyld_cache_header)
        slide_map_off = 0
        slide_map_cnt = 0
        if mapping_off >= 0x140:
            slide_map_off, slide_map_cnt = struct.unpack_from('<II', hdr, 0x138)

        if slide_map_off > 0 and slide_map_cnt == mapping_cnt:
            f.seek(slide_map_off)
            raw_maps = [struct.unpack('<QQQQQQII', f.read(56)) for _ in range(mapping_cnt)]
            for addr, size, file_off, slide_off, slide_sz, _flags, _maxp, _initp in raw_maps:
                s_ver, val_add = 0, 0
                if slide_off > 0 and slide_sz >= 24:
                    f.seek(slide_off)
                    sh = f.read(32)
                    s_ver, = struct.unpack_from('<I', sh, 0)
                    if s_ver in (3, 5):
                        val_add, = struct.unpack_from('<Q', sh, 16)
                self.mappings.append((addr, size, idx, file_off, s_ver, val_add))
        else:
            f.seek(mapping_off)
            for _ in range(mapping_cnt):
                addr, size, file_off, _maxp, _initp = struct.unpack('<QQQII', f.read(32))
                self.mappings.append((addr, size, idx, file_off, 0, 0))
        return hdr

    def _open_all(self):
        hdr = self._add_cache_file(self.main_path)
        if hdr is None:
            raise ValueError(f"Not a dyld_shared_cache: {self.main_path}")
        # Discover all subcaches (.01, .02, ..., .dylddata, .dyldlinkedit, etc.)
        seen = {os.path.abspath(self.main_path)}
        for sub in sorted(glob.glob(self.main_path + '.*')):
            ab = os.path.abspath(sub)
            if ab not in seen and not sub.endswith(('.symbols', '.map', '.json')) and os.path.isfile(ab):
                seen.add(ab)
                self._add_cache_file(sub)

    def base_address(self):
        return min(m[0] for m in self.mappings) if self.mappings else 0x180000000

    def vm_to_loc(self, vmaddr):
        for addr, size, fidx, foff, sver, val_add in self.mappings:
            if addr <= vmaddr < addr + size:
                return self.files[fidx], foff + (vmaddr - addr), sver, val_add
        return None

    def read_vm(self, vmaddr, length):
        loc = self.vm_to_loc(vmaddr)
        if loc is None:
            return None
        f, off, _, _ = loc
        f.seek(off)
        return f.read(length)

    def decode_pointer(self, raw_u64, sver, val_add):
        """Decode an unslid pointer stored in DSC DATA/AUTH_CONST (v3, v5, or plain)."""
        if raw_u64 == 0:
            return 0
        if sver == 5:
            is_auth = (raw_u64 >> 63) & 1
            runtime_off = raw_u64 & 0x3FFFFFFFF
            if is_auth:
                return runtime_off + val_add
            high8 = (raw_u64 << 22) & 0xFF00000000000000
            return (runtime_off + val_add) | high8
        elif sver == 3:
            is_auth = (raw_u64 >> 63) & 1
            if is_auth:
                return (raw_u64 & 0xFFFFFFFF) + val_add
            top8 = raw_u64 & 0x0007F80000000000
            bot43 = raw_u64 & 0x000007FFFFFFFFFF
            return (top8 << 13) | bot43
        return raw_u64 & 0x0000FFFFFFFFFFFF

    def list_images(self):
        f0 = self.files[0]
        f0.seek(0)
        hdr = f0.read(512)
        mapping_off, = struct.unpack_from('<I', hdr, 16)
        img_off, img_cnt = struct.unpack_from('<II', hdr, 24)
        # Modern DSCs move images to imagesOffsetOld (24) == 0 and put imagesText / images at 0x1C0 (448)
        if img_cnt == 0 and mapping_off >= 0x1C8:
            img_off, img_cnt = struct.unpack_from('<II', hdr, 0x1C0)
        images = []
        f0.seek(img_off)
        raw = f0.read(img_cnt * 32)
        for i in range(img_cnt):
            addr, _mod, _ino, path_off, _pad = struct.unpack_from('<QQQII', raw, i * 32)
            f0.seek(path_off)
            pbytes = f0.read(256).split(b'\x00', 1)[0]
            images.append((pbytes.decode('utf-8', 'replace'), addr))
        return images

    def find_image_symbols(self, image_vmaddr, wanted_symbols):
        """Find symbols in an image inside the DSC via LC_DYLD_EXPORTS_TRIE, LC_SYMTAB, and .symbols file."""
        found = {}
        mh = self.read_vm(image_vmaddr, 4096)
        if not mh or len(mh) < 32:
            return found, []
        magic, _ct, _cs, _ft, ncmds, sizeofcmds, _fl, _r = struct.unpack_from('<IIIIIIII', mh, 0)
        if magic != MH_MAGIC_64:
            return found, []
        if 32 + sizeofcmds > len(mh):
            mh = self.read_vm(image_vmaddr, 32 + sizeofcmds)
            if not mh or len(mh) < 32 + sizeofcmds:
                return found, []

        linkedit_vm, linkedit_foff, linkedit_fsz = 0, 0, 0
        text_foff = 0
        symoff = nsyms = stroff = strsize = 0
        trie_off = trie_sz = 0
        segs = []

        pos = 32
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from('<II', mh, pos)
            if cmd == LC_SEGMENT_64:
                sname, vmaddr, vmsize, fileoff, filesize, _mp, _ip, _ns, _sf = struct.unpack_from(
                    '<16sQQQQIIII', mh, pos + 8)
                sn = sname.split(b'\x00', 1)[0].decode('ascii', 'replace')
                segs.append((sn, vmaddr, vmsize, fileoff, filesize))
                if sn == '__TEXT':
                    text_foff = fileoff
                elif sn == '__LINKEDIT':
                    linkedit_vm, linkedit_foff, linkedit_fsz = vmaddr, fileoff, filesize
            elif cmd == LC_SYMTAB:
                symoff, nsyms, stroff, strsize = struct.unpack_from('<IIII', mh, pos + 8)
            elif cmd == LC_DYLD_EXPORTS_TRIE:
                trie_off, trie_sz = struct.unpack_from('<II', mh, pos + 8)
            pos += cmdsize

        # 1. Check exports trie in __LINKEDIT
        if trie_off and trie_sz and linkedit_vm:
            trie_vm = linkedit_vm + (trie_off - linkedit_foff)
            tbuf = self.read_vm(trie_vm, trie_sz)
            if tbuf:
                rel = _walk_exports_trie(tbuf, 0, len(tbuf), wanted_symbols)
                for k, off in rel.items():
                    found[k] = image_vmaddr + off

        # 2. Check LC_SYMTAB in __LINKEDIT
        if len(found) < len(wanted_symbols) and symoff and nsyms and linkedit_vm:
            sym_vm = linkedit_vm + (symoff - linkedit_foff)
            str_vm = linkedit_vm + (stroff - linkedit_foff)
            sbuf = self.read_vm(sym_vm, nsyms * 16)
            stbuf = self.read_vm(str_vm, strsize)
            if sbuf and stbuf:
                for i in range(nsyms):
                    n_strx, n_type, _sect, _desc, n_val = struct.unpack_from('<IBBHQ', sbuf, i * 16)
                    if n_val and n_strx < len(stbuf):
                        end = stbuf.find(b'\x00', n_strx)
                        if end > n_strx:
                            nm = stbuf[n_strx:end].decode('ascii', 'replace')
                            if nm in wanted_symbols and nm not in found:
                                found[nm] = n_val

        # 3. Check companion .symbols subcache (localSymbolsOffset at 0x48 in .symbols header) if present
        sym_file = self.main_path + '.symbols'
        if len(found) < len(wanted_symbols) and os.path.isfile(sym_file):
            with open(sym_file, 'rb') as sf:
                shdr = sf.read(256)
                if shdr.startswith(b'dyld_v1 ') and len(shdr) >= 0x58:
                    loc_off, loc_sz = struct.unpack_from('<QQ', shdr, 0x48)
                    if loc_off and loc_sz:
                        sf.seek(loc_off)
                        lh = sf.read(24)
                        if len(lh) == 24:
                            nlist_off, _nlist_cnt, str_off, str_sz, ent_off, ent_cnt = struct.unpack('<6I', lh)
                            # Modern DSCs use 16-byte dyld_cache_local_symbols_entry_64 (<QII); older use 12-byte (<III)
                            use_64 = struct.unpack_from('<I', shdr, 0x10)[0] >= 0x140
                            esz = 16 if use_64 else 12
                            sf.seek(loc_off + ent_off)
                            ent_raw = sf.read(ent_cnt * esz)
                            for idx_e in range(len(ent_raw) // esz):
                                if use_64:
                                    dylib_foff, nl_start, nl_count = struct.unpack_from('<QII', ent_raw, idx_e * 16)
                                else:
                                    dylib_foff, nl_start, nl_count = struct.unpack_from('<III', ent_raw, idx_e * 12)
                                if dylib_foff in (text_foff, image_vmaddr - self.base_address()):
                                    sf.seek(loc_off + nlist_off + nl_start * 16)
                                    nbuf = sf.read(nl_count * 16)
                                    sf.seek(loc_off + str_off)
                                    stbuf = sf.read(str_sz)
                                    for j in range(len(nbuf) // 16):
                                        n_strx, _nt, _ns, _nd, n_val = struct.unpack_from('<IBBHQ', nbuf, j * 16)
                                        if n_val and n_strx < len(stbuf):
                                            end = stbuf.find(b'\x00', n_strx)
                                            if end > n_strx:
                                                nm = stbuf[n_strx:end].decode('ascii', 'replace')
                                                if nm in wanted_symbols and nm not in found:
                                                    found[nm] = n_val
                                    break
        return found, segs


def extract_quartzcore_gles_offsets(dsc):
    """Locate CA::OGL::GLESContext vtable and function_supported / function_uniform slots."""
    wanted = {
        '__ZTVN2CA3OGL11GLESContextE',
        '__ZNK2CA3OGL9GLContext18function_supportedEv',
        '__ZN2CA3OGL9GLContext16function_uniformEjmmPKf',
    }
    qc_addr = None
    for path, addr in dsc.list_images():
        if path.endswith('/QuartzCore.framework/QuartzCore'):
            qc_addr = addr
            break
    if qc_addr is None:
        return {}

    syms, segs = dsc.find_image_symbols(qc_addr, wanted)
    out = {'dsc_base': dsc.base_address(), 'quartzcore_base': qc_addr}
    vt = syms.get('__ZTVN2CA3OGL11GLESContextE')
    fn_sup = syms.get('__ZNK2CA3OGL9GLContext18function_supportedEv')
    fn_uni = syms.get('__ZN2CA3OGL9GLContext16function_uniformEjmmPKf')
    if vt:
        out['ca_vt_gles_context'] = vt
    if fn_sup:
        out['ca_fn_supported'] = fn_sup
    if fn_uni:
        out['ca_fn_uniform'] = fn_uni

    # Scan vtable entries (+0x10 .. +0x800) to find exact slot addresses for function_supported & function_uniform!
    if vt and (fn_sup or fn_uni):
        loc = dsc.vm_to_loc(vt)
        if loc:
            f, foff, sver, val_add = loc
            f.seek(foff)
            vt_bytes = f.read(0x800)
            for off in range(0x10, len(vt_bytes) - 7, 8):
                raw_ptr, = struct.unpack_from('<Q', vt_bytes, off)
                target = dsc.decode_pointer(raw_ptr, sver, val_add) & 0x00FFFFFFFFFFFFFF
                if fn_sup and target == (fn_sup & 0x00FFFFFFFFFFFFFF) and 'ca_vt_slot_supported' not in out:
                    out['ca_vt_slot_supported'] = vt + off
                elif fn_uni and target == (fn_uni & 0x00FFFFFFFFFFFFFF) and 'ca_vt_slot_uniform' not in out:
                    out['ca_vt_slot_uniform'] = vt + off
    return out


def find_dsc_in_root(root_or_dsc):
    if os.path.isfile(root_or_dsc):
        return root_or_dsc
    candidates = [
        os.path.join(root_or_dsc, 'System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e'),
        os.path.join(root_or_dsc, 'System/Library/dyld/dyld_shared_cache_arm64e'),
        os.path.join(root_or_dsc, 'private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e'),
        os.path.join(root_or_dsc, 'private/preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_arm64e'),
    ]
    return next((c for c in candidates if os.path.isfile(c)), None)


def generate_offsets_conf(root_or_dsc, out_conf=None):
    dsc_path = find_dsc_in_root(root_or_dsc)
    if not dsc_path:
        print(f"ipados26_dsc_offsets: no dyld_shared_cache_arm64e found in {root_or_dsc}", file=sys.stderr)
        return {}
    dsc = DyldSharedCacheReader(dsc_path)
    try:
        offsets = extract_quartzcore_gles_offsets(dsc)
    finally:
        dsc.close()

    if out_conf is None and os.path.isdir(root_or_dsc):
        out_conf = os.path.join(root_or_dsc, 'redwood/etc/sapphire-offsets.conf')

    if out_conf:
        os.makedirs(os.path.dirname(os.path.abspath(out_conf)), exist_ok=True)
        with open(out_conf, 'w') as f:
            f.write("# Auto-generated by tools/ipados26_dsc_offsets.py from dyld_shared_cache_arm64e\n")
            for k, v in sorted(offsets.items()):
                f.write(f"{k}=0x{v:x}\n")
        print(f"ipados26_dsc_offsets: wrote {len(offsets)} entries to {out_conf}")
    return offsets


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    target = sys.argv[1]
    out_conf = sys.argv[2] if len(sys.argv) > 2 else None
    offsets = generate_offsets_conf(target, out_conf)
    for k, v in sorted(offsets.items()):
        print(f"  {k} = 0x{v:x}")


if __name__ == '__main__':
    main()
