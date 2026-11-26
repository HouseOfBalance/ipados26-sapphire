#!/usr/bin/env python3
"""Blocks to write back to the phone after editing the WSL HFS+ image:

    tools/hfs-push-ranges.py IMG BITMAP_OR_IMG.before BITMAP.after META-RANGES [EXTRA-RUNS ...] > RANGES

Collects:
  - Newly allocated blocks (bitmap after & ~before) AND any in-place modified
    blocks when BITMAP_OR_IMG.before is a full HFS+ image (or EXTRA-RUNS files
    produced by sapphire-jb-install.sh / sapphire-ssh-setup.sh).
  - Full HFS+ JournalInfoBlock + actual journal range (read from JournalInfoBlock).
  - Primary & alternate volume headers and all 5 special files (including any
    overflow extents in the Extents Overflow B-tree).
"""
import os
import struct
import sys


def _read_fork_bytes(f, bsz, extents):
    out = bytearray()
    for start_blk, blk_cnt in extents:
        f.seek(start_blk * bsz)
        out.extend(f.read(blk_cnt * bsz))
    return bytes(out)


def _collect_overflow_extents(ext_btree_bytes, target_cnid):
    """Scan HFS+ Extents Overflow B-tree leaf nodes (kind == -1) for target_cnid."""
    if len(ext_btree_bytes) < 512:
        return []
    node_size, total_nodes = struct.unpack_from('>HI', ext_btree_bytes, 32)
    if node_size < 512:
        return []
    found = []
    for n in range(min(total_nodes, len(ext_btree_bytes) // node_size)):
        noff = n * node_size
        _flink, _blink, kind, height, num_recs = struct.unpack_from('>IIbbH', ext_btree_bytes, noff)
        if kind != -1 or height != 1:
            continue
        for r in range(num_recs):
            roff, = struct.unpack_from('>H', ext_btree_bytes, noff + node_size - 2 * (r + 1))
            pos = noff + roff
            if pos + 12 > noff + node_size:
                continue
            key_len, fork_type, _pad, cnid, _start_block = struct.unpack_from('>HBBII', ext_btree_bytes, pos)
            if key_len == 10 and fork_type == 0 and cnid == target_cnid and pos + 12 + 64 <= noff + node_size:
                exts = [struct.unpack_from('>II', ext_btree_bytes, pos + 12 + 8 * k) for k in range(8)]
                found.extend(e for e in exts if e[1])
    return found


def compute_runs(argv):
    if len(argv) < 5:
        sys.exit(__doc__)
    img, before_path, after_path = argv[1], argv[2], argv[3]
    run_files = argv[4:]

    with open(img, 'rb') as f:
        f.seek(1024)
        v = f.read(512)
        bsz = struct.unpack_from('>I', v, 40)[0] or 4096
        total = struct.unpack_from('>I', v, 44)[0]
        jib = struct.unpack_from('>I', v, 12)[0]

        meta = [(0, 1), (total - 1, 1)]
        if jib > 0:
            meta.append((jib, 1))
            f.seek(jib * bsz)
            jd = f.read(bsz)
            if len(jd) >= 52:
                j_off, j_size = struct.unpack_from('>QQ', jd, 36)
                if j_off > 0 and j_size > 0:
                    meta.append((j_off // bsz, (j_size + bsz - 1) // bsz))

        # Special file CNIDs: 0=Allocation(6), 1=Extents(3), 2=Catalog(4), 3=Attributes(8), 4=Startup(7)
        special_cnids = (6, 3, 4, 8, 7)
        ext_Fork_extents = [struct.unpack_from('>II', v, 112 + 80 * 1 + 16 + 8 * k) for k in range(8)]
        ext_Fork_extents = [e for e in ext_Fork_extents if e[1]]
        ext_btree = _read_fork_bytes(f, bsz, ext_Fork_extents) if ext_Fork_extents else b''

        for i in range(5):
            o = 112 + 80 * i
            tot = struct.unpack_from('>I', v, o + 12)[0]
            ext = [struct.unpack_from('>II', v, o + 16 + 8 * k) for k in range(8)]
            ext = [e for e in ext if e[1]]
            if sum(e[1] for e in ext) < tot and ext_btree:
                ext.extend(_collect_overflow_extents(ext_btree, special_cnids[i]))
            meta.extend(ext)

    a = open(after_path, 'rb').read()
    new = []
    freed = 0
    if os.path.getsize(before_path) == total * bsz and os.path.getsize(img) == total * bsz:
        # Full image comparison: catches BOTH newly allocated AND in-place modified / reallocated blocks!
        with open(before_path, 'rb') as fb, open(img, 'rb') as fa:
            CHUNK_BLKS = 1024
            blk_idx = 0
            while blk_idx < total:
                cnt = min(CHUNK_BLKS, total - blk_idx)
                ba = fa.read(cnt * bsz)
                bb = fb.read(cnt * bsz)
                if ba != bb:
                    for k in range(cnt):
                        if ba[k * bsz:(k + 1) * bsz] != bb[k * bsz:(k + 1) * bsz]:
                            new.append(blk_idx + k)
                blk_idx += cnt
    else:
        b = open(before_path, 'rb').read()
        for i in range(min(len(a), len(b))):
            x = a[i] & ~b[i]
            if x:
                for bit in range(8):
                    if x & (0x80 >> bit):
                        new.append(i * 8 + bit)
            freed += bin(b[i] & ~a[i]).count('1')
        if freed > 0 and len(run_files) == 1:
            print(
                f"WARNING: {freed} blocks were freed during edit; pass caller files.runs as an extra argument "
                f"(or pass full IMG.before) so reused freed blocks are included!",
                file=sys.stderr,
            )

    for rf in run_files:
        for l in open(rf):
            parts = l.split()
            if len(parts) >= 2:
                meta.append((int(parts[0]), int(parts[1])))

    blocks = set(new)
    for s, c in meta:
        blocks.update(range(s, s + c))
    blocks = sorted(blocks)
    runs = []
    for x in blocks:
        if runs and runs[-1][0] + runs[-1][1] == x:
            runs[-1][1] += 1
        else:
            runs.append([x, 1])
    print(
        f'nuevos {len(new)} bloques ({len(new)*4/1024:.1f} MB), liberados {freed}, '
        f'total a escribir {len(blocks)} bloques ({len(blocks)*4/1024:.1f} MB) en {len(runs)} tramos',
        file=sys.stderr,
    )
    for s, c in runs:
        print(s, c)


if __name__ == '__main__':
    compute_runs(sys.argv)
