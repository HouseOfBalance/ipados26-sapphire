#!/bin/bash
# Stage Apple's open-source hfs-704.40.4.0.1 (paired with xnu-12377.41.6 in macOS/iPadOS 26.1)
# for the SWEET built-in HFS+ root filesystem (bsd/sweet_hfs), including dirty RW root mount
# (patch 0031) and hardlink console spam rate-limiting (patch 0041).
set -euo pipefail

[[ "${MACHINE_CONFIG:-}" == SWEET ]] || exit 0
[[ -f bsd/kern/bsd_init.c ]] || exit 1

hfs_sha=ba0655c7cb232027c547c16b34a7b2c63e80a998
hfs_tmp=$(mktemp -d)
trap 'rm -rf "$hfs_tmp"' EXIT
git init -q "$hfs_tmp"
git -C "$hfs_tmp" fetch -q --depth 1 \
    https://github.com/apple-oss-distributions/hfs "$hfs_sha"
git -C "$hfs_tmp" checkout -q FETCH_HEAD

python3 - "$hfs_tmp" <<'PY'
from pathlib import Path
import shutil
import sys

source = Path(sys.argv[1])
target = Path('bsd/sweet_hfs')
target.mkdir(parents=True, exist_ok=True)
# Only replace core/ and hfs_encodings/ so sweet_hfs_compat.h & sweet_hfs_iokit.c stay intact
for directory in ('core', 'hfs_encodings'):
    dst = target / directory
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(source / directory, dst)

# 1. Guard AppleKeyStore header when !CONFIG_PROTECT
path = target / 'core' / 'hfs_iokit.h'
text = path.read_text()
include = '#include <AppleKeyStore/AppleKeyStoreFSServices.h>\n'
first = 'int hfs_unwrap_key('
end = '__END_DECLS'
if include not in text or first not in text or end not in text:
    raise SystemExit(f'HFS source changed unexpectedly: {path}')
text = text.replace(include, '#if CONFIG_PROTECT\n' + include + '#endif\n')
i, j = text.index(first), text.index(end)
text = text[:i] + '#if CONFIG_PROTECT\n' + text[i:j].rstrip('\n') + '\n#endif\n\n' + text[j:]
path.write_text(text)

# 2. Apply patch 0031: allow dirty non-journaled root volume to mount RW when rootrw=1
vfsutils = target / 'core' / 'hfs_vfsutils.c'
vt = vfsutils.read_text()
old_dirty = (
    '\tif ((hfsmp->hfs_flags & HFS_READ_ONLY) == 0 && hfsmp->jnl == NULL &&\n'
    '\t    (SWAP_BE32(vhp->attributes) & kHFSVolumeUnmountedMask) == 0) {\n'
    '\t\tif (HFS_MOUNT_DEBUG) {\n'
    '\t\t\tprintf("hfs_mounthfsplus: cannot mount dirty non-journaled volumes\\n");\n'
    '\t\t}\n'
    '\t\treturn (EINVAL);\n'
    '\t}'
)
new_dirty = (
    '\tif ((hfsmp->hfs_flags & HFS_READ_ONLY) == 0 && hfsmp->jnl == NULL &&\n'
    '\t    (SWAP_BE32(vhp->attributes) & kHFSVolumeUnmountedMask) == 0) {\n'
    '#if defined(ARM64_BOARD_CONFIG_SWEET)\n'
    '\t\tif (vfs_flags(hfsmp->hfs_mp) & MNT_ROOTFS) {\n'
    '\t\t\tprintf("hfs_mounthfsplus: SWEET: mounting dirty non-journaled root read-write\\n");\n'
    '\t\t} else {\n'
    '#endif\n'
    '\t\tif (HFS_MOUNT_DEBUG) {\n'
    '\t\t\tprintf("hfs_mounthfsplus: cannot mount dirty non-journaled volumes\\n");\n'
    '\t\t}\n'
    '\t\treturn (EINVAL);\n'
    '#if defined(ARM64_BOARD_CONFIG_SWEET)\n'
    '\t\t}\n'
    '#endif\n'
    '\t}'
)
if old_dirty not in vt:
    raise SystemExit(f'HFS dirty-mount check not found in {vfsutils}')
vfsutils.write_text(vt.replace(old_dirty, new_dirty, 1))

# 3. Apply patch 0041: rate-limit hardlink bit repair console spam (Linux mkipadosroot hardlinks)
cat_path = target / 'core' / 'hfs_catalog.c'
ct = cat_path.read_text()
helper_anchor = '/*\n * cat_lookupbykey - lookup a catalog node using a cnode key\n */\n'
helper_code = (
    '#if defined(ARM64_BOARD_CONFIG_SWEET)\n'
    'static int\n'
    'sweet_hfs_link_note(void)\n'
    '{\n'
    '\tstatic volatile SInt32 notes;\n'
    '\treturn OSIncrementAtomic(&notes) < 8;\n'
    '}\n'
    '#else\n'
    '#define sweet_hfs_link_note() 1\n'
    '#endif\n\n'
)
if helper_anchor not in ct:
    raise SystemExit(f'cat_lookupbykey anchor not found in {cat_path}')
ct = ct.replace(helper_anchor, helper_code + helper_anchor, 1)
for msg in (
    'printf ("hfs: set hardlink bit on vol=%s cnid=%u inoid=%u\\n", hfsmp->vcbVN, cnid, ilink);',
    'printf ("hfs: clear hardlink bit on vol=%s cnid=%u\\n", hfsmp->vcbVN, cnid);',
    'printf ("hfs: set linkcount=1 on vol=%s cnid=%u old=%u\\n", hfsmp->vcbVN, cnid, attrp->ca_linkcount);',
):
    if msg in ct:
        ct = ct.replace(msg, f'if (sweet_hfs_link_note()) {{ {msg} }}', 1)
cat_path.write_text(ct)

# 4. Advertise unencrypted Class D (4) Data Protection semantics when !CONFIG_PROTECT on SWEET
# so keybagd, dirs_cleaner, and containermanagerd succeed on fcntl(F_GET/SETPROTECTIONCLASS)
# and setattrlist(ATTR_CMN_DATA_PROTECT_FLAGS).
vnops_path = target / 'core' / 'hfs_vnops.c'
vn = vnops_path.read_text()
old_getattr_cp = (
    '#if CONFIG_PROTECT\n'
    '\tif (VATTR_IS_ACTIVE(vap, va_dataprotect_class)) {\n'
    '    \t\tvap->va_dataprotect_class = cp->c_cpentry ? CP_CLASS(cp->c_cpentry->cp_pclass) : 0;\n'
    '    \t\tVATTR_SET_SUPPORTED(vap, va_dataprotect_class);\n'
    '    \t} \t\n'
    '#endif'
)
new_getattr_cp = (
    '#if CONFIG_PROTECT\n'
    '\tif (VATTR_IS_ACTIVE(vap, va_dataprotect_class)) {\n'
    '    \t\tvap->va_dataprotect_class = cp->c_cpentry ? CP_CLASS(cp->c_cpentry->cp_pclass) : 0;\n'
    '    \t\tVATTR_SET_SUPPORTED(vap, va_dataprotect_class);\n'
    '    \t} \t\n'
    '#elif defined(ARM64_BOARD_CONFIG_SWEET)\n'
    '\tif (VATTR_IS_ACTIVE(vap, va_dataprotect_class)) {\n'
    '\t\tvap->va_dataprotect_class = 4; /* PROTECTION_CLASS_D */\n'
    '\t\tVATTR_SET_SUPPORTED(vap, va_dataprotect_class);\n'
    '\t}\n'
    '#endif'
)
old_setattr_cp = (
    '#if CONFIG_PROTECT\n'
    '\tVATTR_SET_SUPPORTED(vap, va_dataprotect_class);\n'
    '\tif (!error && VATTR_IS_ACTIVE(vap, va_dataprotect_class))\n'
    '\t\terror = cp_vnode_setclass(vp, vap->va_dataprotect_class);\n'
    '#endif'
)
new_setattr_cp = (
    '#if CONFIG_PROTECT\n'
    '\tVATTR_SET_SUPPORTED(vap, va_dataprotect_class);\n'
    '\tif (!error && VATTR_IS_ACTIVE(vap, va_dataprotect_class))\n'
    '\t\terror = cp_vnode_setclass(vp, vap->va_dataprotect_class);\n'
    '#elif defined(ARM64_BOARD_CONFIG_SWEET)\n'
    '\tVATTR_SET_SUPPORTED(vap, va_dataprotect_class);\n'
    '#endif'
)
if old_getattr_cp not in vn or old_setattr_cp not in vn:
    raise SystemExit(f'HFS CONFIG_PROTECT blocks not found in {vnops_path}')
vn = vn.replace(old_getattr_cp, new_getattr_cp, 1).replace(old_setattr_cp, new_setattr_cp, 1)
vnops_path.write_text(vn)

vt = vfsutils.read_text()
old_mnt_cp = (
    '#else\n'
    '\t\t/* If CONFIG_PROTECT not built, ignore CP */\n'
    '\t\tvfs_clearflags(hfsmp->hfs_mp, MNT_CPROTECT);\t\n'
    '#endif'
)
new_mnt_cp = (
    '#elif defined(ARM64_BOARD_CONFIG_SWEET)\n'
    '\t\tvfs_setflags(hfsmp->hfs_mp, MNT_CPROTECT);\n'
    '#else\n'
    '\t\t/* If CONFIG_PROTECT not built, ignore CP */\n'
    '\t\tvfs_clearflags(hfsmp->hfs_mp, MNT_CPROTECT);\t\n'
    '#endif\n'
    '\t} else {\n'
    '#if defined(ARM64_BOARD_CONFIG_SWEET) && !CONFIG_PROTECT\n'
    '\t\tvfs_setflags(hfsmp->hfs_mp, MNT_CPROTECT);\n'
    '#endif'
)
if old_mnt_cp not in vt:
    raise SystemExit(f'HFS MNT_CPROTECT block not found in {vfsutils}')
vfsutils.write_text(vt.replace(old_mnt_cp, new_mnt_cp, 1))

vfsops_path = target / 'core' / 'hfs_vfsops.c'
vo = vfsops_path.read_text()
old_vo1 = '#if CONFIG_PROTECT\n        \tattrp->validattr.commonattr |= ATTR_CMN_DATA_PROTECT_FLAGS;\n#endif // CONFIG_PROTECT'
new_vo1 = '#if CONFIG_PROTECT || defined(ARM64_BOARD_CONFIG_SWEET)\n        \tattrp->validattr.commonattr |= ATTR_CMN_DATA_PROTECT_FLAGS;\n#endif // CONFIG_PROTECT'
old_vo2 = '#if CONFIG_PROTECT\n\t\tattrp->nativeattr.commonattr |= ATTR_CMN_DATA_PROTECT_FLAGS;\n#endif // CONFIG_PROTECT'
new_vo2 = '#if CONFIG_PROTECT || defined(ARM64_BOARD_CONFIG_SWEET)\n\t\tattrp->nativeattr.commonattr |= ATTR_CMN_DATA_PROTECT_FLAGS;\n#endif // CONFIG_PROTECT'
if old_vo1 not in vo or old_vo2 not in vo:
    raise SystemExit(f'HFS ATTR_CMN_DATA_PROTECT_FLAGS blocks not found in {vfsops_path}')
vfsops_path.write_text(vo.replace(old_vo1, new_vo1, 1).replace(old_vo2, new_vo2, 1))
PY

if command -v xcrun >/dev/null 2>&1; then
    hfs_sdk=$(xcrun --sdk macosx --show-sdk-path)
    cp "$hfs_sdk/usr/include/TargetConditionals.h" bsd/sweet_hfs/
fi
echo "Staged hfs-704.40.4.0.1 ($hfs_sha) with SWEET RW-root + hardlink fixes for XNU 26.1"
