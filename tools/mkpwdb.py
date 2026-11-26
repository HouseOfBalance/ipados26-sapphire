#!/usr/bin/env python3
"""Generate OpenBSD/libiosexec-compatible pwd.db, spwd.db (4.4BSD db 1.85 DB_HASH,
little-endian 64-bit time_t matching arm64 iOS/iPadOS) and 7-field passwd from a
10-field BSD master.passwd file, with zero external C library dependencies.

Matches tools/mkpwdb.c record & key layout:
  keys:
    b'1' + name
    b'2' + uint32_le(entry_num_from_1)
    b'3' + uint32_le(uid)            (first entry wins: R_NOOVERWRITE)
  record:
    name\\0 passwd\\0 int32(uid) int32(gid) int64(change)
    class\\0 gecos\\0 dir\\0 shell\\0 int64(expire) int32(flags=0)
  pwd.db stores "*" for every non-empty password; spwd.db stores the hash.

Usage:
  python3 tools/mkpwdb.py <master.passwd> <outdir>
  python3 tools/mkpwdb.py --selftest
"""
import os
import struct
import sys
import tempfile

HASHMAGIC = 0x061561
HASHVERSION = 2
LORDER_LE = 1234
BSIZE = 4096
BSHIFT = 12
DSIZE = 256
SSIZE = 256
SSHIFT = 8
FFACTOR = 32
HDRPAGES = 1
NCACHED = 32
SPLITSHIFT = 11
CHARKEY = b"%$sniglet^&\n\x00"  # sizeof("%$sniglet^&\n") == 13 in C


def hash4(data: bytes) -> int:
    h = 0
    for b in data:
        h = ((h << 5) + h + b) & 0xFFFFFFFF
    return h


def _ceil_log2(x: int) -> int:
    """BSD __log2(num): smallest k such that (1 << k) >= num."""
    k = 0
    limit = 1
    while limit < x:
        limit <<= 1
        k += 1
    return k


def _bucket_to_page(bucket: int, spares: list[int]) -> int:
    if bucket == 0:
        return HDRPAGES
    return bucket + HDRPAGES + spares[_ceil_log2(bucket + 1) - 1]


def build_bsd_hash_db(items: list[tuple[bytes, bytes]], nbuckets: int = 64) -> bytes:
    """Build a 4.4BSD db 1.85 DB_HASH image with power-of-two buckets."""
    assert nbuckets >= 16 and (nbuckets & (nbuckets - 1)) == 0
    ovfl_point = _ceil_log2(nbuckets)
    max_bucket = nbuckets - 1
    high_mask = nbuckets - 1
    low_mask = (nbuckets >> 1) - 1

    # 1 bitmap page allocated at OADDR_OF(ovfl_point, 1) right after bucket max_bucket
    spares = [0 if i < ovfl_point else 1 for i in range(NCACHED)]
    bitmaps = [0] * NCACHED
    bitmaps[0] = (ovfl_point << SPLITSHIFT) + 1

    buckets: list[list[tuple[bytes, bytes]]] = [[] for _ in range(nbuckets)]
    for k, v in items:
        b = hash4(k) & high_mask
        if b > max_bucket:
            b &= low_mask
        buckets[b].append((k, v))

    bitmap_page_idx = _bucket_to_page(max_bucket, spares) + 1
    total_pages = bitmap_page_idx + 1
    pages = [bytearray(BSIZE) for _ in range(total_pages)]

    # Page 0: HASHHDR (17 int32/uint32 + 32 int32 spares + 32 uint16 bitmaps = 260 bytes)
    h_charkey = hash4(CHARKEY)
    if h_charkey >= 0x80000000:
        h_charkey -= 0x100000000
    hdr = struct.pack(
        "<iiIiiiiiiiiiiiiii32i32H",
        HASHMAGIC,
        HASHVERSION,
        LORDER_LE,
        BSIZE,
        BSHIFT,
        DSIZE,
        SSIZE,
        SSHIFT,
        ovfl_point,
        0,            # last_freed
        max_bucket,
        high_mask,
        low_mask,
        FFACTOR,
        len(items),   # nkeys
        HDRPAGES,
        h_charkey,
        *spares,
        *bitmaps,
    )
    pages[0][:len(hdr)] = hdr

    # Bucket pages: bp[0]=n, bp[1..n]=key/val offsets, bp[n+1]=freespace, bp[n+2]=offset_top
    for b_idx, pairs in enumerate(buckets):
        pno = _bucket_to_page(b_idx, spares)
        pg = pages[pno]
        n = 0
        off = BSIZE
        for k, v in pairs:
            needed = len(k) + len(v) + 4
            free_now = off - ((n + 3) * 2)
            if needed > free_now:
                raise ValueError("bucket overflow; increase nbuckets")
            off -= len(k)
            pg[off:off + len(k)] = k
            n += 1
            struct.pack_into("<H", pg, n * 2, off)
            off -= len(v)
            pg[off:off + len(v)] = v
            n += 1
            struct.pack_into("<H", pg, n * 2, off)
        freespace = off - ((n + 3) * 2)
        struct.pack_into("<H", pg, 0, n)
        struct.pack_into("<H", pg, (n + 1) * 2, freespace)
        struct.pack_into("<H", pg, (n + 2) * 2, off)

    # Bitmap page: bit 0 = 1 (marks the bitmap page itself allocated)
    struct.pack_into("<I", pages[bitmap_page_idx], 0, 1)
    return b"".join(pages)


def lookup_bsd_hash_db(blob: bytes, key: bytes) -> bytes | None:
    """Reader used by --selftest to verify header, bucket mapping, and key/value offsets."""
    hdr_fmt = "<iiIiiiiiiiiiiiiii32i32H"
    unpacked = struct.unpack_from(hdr_fmt, blob, 0)
    magic, version, lorder, bsize = unpacked[0:4]
    max_bucket, high_mask, low_mask = unpacked[10:13]
    h_charkey = unpacked[16] & 0xFFFFFFFF
    spares = list(unpacked[17:49])
    assert magic == HASHMAGIC and version == HASHVERSION and lorder == LORDER_LE and bsize == BSIZE
    assert h_charkey == hash4(CHARKEY)
    b = hash4(key) & high_mask
    if b > max_bucket:
        b &= low_mask
    pno = _bucket_to_page(b, spares)
    pg = memoryview(blob)[pno * bsize:(pno + 1) * bsize]
    n = struct.unpack_from("<H", pg, 0)[0]
    prev_off = bsize
    for idx in range(1, n + 1, 2):
        k_off = struct.unpack_from("<H", pg, idx * 2)[0]
        v_off = struct.unpack_from("<H", pg, (idx + 1) * 2)[0]
        k_bytes = bytes(pg[k_off:prev_off])
        v_bytes = bytes(pg[v_off:k_off])
        prev_off = v_off
        if k_bytes == key:
            return v_bytes
    return None


def _pack_record(fields: list[str], shadow: bool) -> tuple[int, bytes]:
    name, pw, uid_s, gid_s, pw_class, change_s, expire_s, gecos, home, shell = fields
    uid = int(uid_s)
    gid = int(gid_s)
    change = int(change_s) if change_s else 0
    expire = int(expire_s) if expire_s else 0
    pw_out = pw if (shadow or not pw) else "*"
    rec = bytearray()
    rec += name.encode("utf-8") + b"\x00"
    rec += pw_out.encode("utf-8") + b"\x00"
    rec += struct.pack("<iiq", uid, gid, change)
    rec += pw_class.encode("utf-8") + b"\x00"
    rec += gecos.encode("utf-8") + b"\x00"
    rec += home.encode("utf-8") + b"\x00"
    rec += shell.encode("utf-8") + b"\x00"
    rec += struct.pack("<qi", expire, 0)
    return uid, bytes(rec)


def generate_pwdb(master_passwd_path: str, out_dir: str) -> int:
    pwd_items: list[tuple[bytes, bytes]] = []
    spwd_items: list[tuple[bytes, bytes]] = []
    passwd_lines: list[str] = []
    seen_uids: set[int] = set()
    seen_names: set[str] = set()
    num = 0

    with open(master_passwd_path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split(":")
            if len(parts) != 10:
                raise ValueError(f"bad master.passwd line ({len(parts)} fields): {line}")
            name = parts[0]
            if name in seen_names:
                continue
            seen_names.add(name)
            uid, prec = _pack_record(parts, shadow=False)
            _, srec = _pack_record(parts, shadow=True)
            entry_no = num + 1
            k_name = b"1" + name.encode("utf-8")
            k_num = b"2" + struct.pack("<i", entry_no)
            k_uid = b"3" + struct.pack("<I", uid & 0xFFFFFFFF)

            pwd_items.append((k_name, prec))
            pwd_items.append((k_num, prec))
            spwd_items.append((k_name, srec))
            spwd_items.append((k_num, srec))
            if uid not in seen_uids:
                seen_uids.add(uid)
                pwd_items.append((k_uid, prec))
                spwd_items.append((k_uid, srec))

            pw7 = "*" if parts[1] else ""
            passwd_lines.append(f"{parts[0]}:{pw7}:{parts[2]}:{parts[3]}:{parts[7]}:{parts[8]}:{parts[9]}")
            num += 1

    nbuckets = 64
    while nbuckets < max(64, num * 4):
        nbuckets <<= 1

    os.makedirs(out_dir, exist_ok=True)
    pwd_db_path = os.path.join(out_dir, "pwd.db")
    spwd_db_path = os.path.join(out_dir, "spwd.db")
    passwd_path = os.path.join(out_dir, "passwd")

    with open(pwd_db_path, "wb") as f:
        f.write(build_bsd_hash_db(pwd_items, nbuckets=nbuckets))
    os.chmod(pwd_db_path, 0o644)

    with open(spwd_db_path, "wb") as f:
        f.write(build_bsd_hash_db(spwd_items, nbuckets=nbuckets))
    os.chmod(spwd_db_path, 0o600)

    with open(passwd_path, "w", encoding="utf-8") as f:
        f.write("\n".join(passwd_lines) + "\n")
    os.chmod(passwd_path, 0o644)
    return num


def _selftest() -> None:
    sample = (
        "# Comment line\n"
        "root:sa3tHJ3/KuYvI:0:0::0:0:System Administrator:/var/root:/bin/sh\n"
        "toor:*:0:0::0:0:Bourne-again Superuser:/var/root:/bin/sh\n"
        "mobile:sa3tHJ3/KuYvI:501:501::0:0:Mobile User:/var/mobile:/bin/sh\n"
        "nobody:*:-2:-2::0:0:Unprivileged User:/var/empty:/usr/bin/false\n"
    )
    with tempfile.TemporaryDirectory() as td:
        mp = os.path.join(td, "master.passwd")
        with open(mp, "w", encoding="utf-8") as f:
            f.write(sample)
        n = generate_pwdb(mp, td)
        assert n == 4, n
        pdb = open(os.path.join(td, "pwd.db"), "rb").read()
        sdb = open(os.path.join(td, "spwd.db"), "rb").read()
        # Lookup root by name, entry #1, and uid 0 (R_NOOVERWRITE -> root wins over toor)
        r_pwd = lookup_bsd_hash_db(pdb, b"1root")
        r_spwd = lookup_bsd_hash_db(sdb, b"1root")
        r_uid0 = lookup_bsd_hash_db(pdb, b"3" + struct.pack("<i", 0))
        r_num3 = lookup_bsd_hash_db(sdb, b"2" + struct.pack("<i", 3))
        assert r_pwd is not None and r_pwd.startswith(b"root\x00*\x00")
        assert r_spwd is not None and r_spwd.startswith(b"root\x00sa3tHJ3/KuYvI\x00")
        assert r_uid0 == r_pwd
        assert r_num3 is not None and r_num3.startswith(b"mobile\x00sa3tHJ3/KuYvI\x00")
        # Negative UID (-2 = nobody)
        r_nobody = lookup_bsd_hash_db(pdb, b"3" + struct.pack("<i", -2))
        assert r_nobody is not None and r_nobody.startswith(b"nobody\x00*\x00")
    print("mkpwdb.py selftest: OK")


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "--selftest":
        _selftest()
    elif len(sys.argv) == 3:
        cnt = generate_pwdb(sys.argv[1], sys.argv[2])
        print(f"{cnt} entries")
    else:
        sys.exit("usage: mkpwdb.py <master.passwd> <outdir> | --selftest")
