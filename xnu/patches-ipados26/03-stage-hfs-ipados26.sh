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
PY

if command -v xcrun >/dev/null 2>&1; then
    hfs_sdk=$(xcrun --sdk macosx --show-sdk-path)
    cp "$hfs_sdk/usr/include/TargetConditionals.h" bsd/sweet_hfs/
fi
echo "Staged hfs-704.40.4.0.1 ($hfs_sha) with SWEET RW-root + hardlink fixes for XNU 26.1"
