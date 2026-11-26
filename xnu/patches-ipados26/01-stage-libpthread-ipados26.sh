#!/bin/bash
# Stage Apple's open-source libpthread-539 (paired with xnu-12377.41.6 in macOS/iPadOS 26.1)
# for the SWEET built-in pthread kext provider.
set -euo pipefail

[[ "${MACHINE_CONFIG:-}" == SWEET ]] || exit 0
[[ -f bsd/pthread/pthread_shims.c ]] || exit 1

pthread_sha=f7a3c225bc5e7f999a4e24b8a58f6e9fb5941068
pthread_tmp=$(mktemp -d)
trap 'rm -rf "$pthread_tmp"' EXIT
git init -q "$pthread_tmp"
git -C "$pthread_tmp" fetch -q --depth 1 \
    https://github.com/apple-oss-distributions/libpthread "$pthread_sha"
git -C "$pthread_tmp" checkout -q FETCH_HEAD

python3 - "$pthread_tmp" <<'PY'
from pathlib import Path
import shutil
import sys

source = Path(sys.argv[1])
target = Path('bsd/pthread/sweet_libpthread')
if target.exists():
    shutil.rmtree(target)
for directory in ('kern', 'private', 'include'):
    shutil.copytree(source / directory, target / directory)

def replace_opt(file, old, new):
    path = target / 'kern' / file
    if path.exists():
        text = path.read_text()
        if old in text:
            path.write_text(text.replace(old, new))

replace_opt('kern_trace.h', 'vm_kernel_unslide_or_perm_external(ptr,',
            'vm_kernel_unslide_or_perm_external((vm_offset_t)ptr,')
for callback in ('psynch_wait_update_owner', 'psynch_wait_complete', 'psynch_wait_wakeup'):
    replace_opt('kern_synch.c', f'pthread_kern->{callback}(kwq,',
                f'pthread_kern->{callback}((uintptr_t)kwq,')
replace_opt('kern_support.c', '*kevent_list_out = NULL;', '*kevent_list_out = 0;')
replace_opt('kern_support.c', 'user_addr_t kevent_list = NULL;', 'user_addr_t kevent_list = 0;')
replace_opt('kern_support.c', 'workq_kevent(p, &th_addrs, NULL,', 'workq_kevent(p, &th_addrs, 0,')
replace_opt('kern_support.c',
            'panic(__func__ ": thread_set_wq_state failed: %d", error);',
            'panic("%s: thread_set_wq_state failed: %d", __func__, error);')
PY

if command -v xcrun >/dev/null 2>&1; then
    pthread_sdk=$(xcrun --sdk macosx --show-sdk-path)
    cp "$pthread_sdk/usr/include/TargetConditionals.h" bsd/pthread/sweet_libpthread/
fi
echo "Staged libpthread-539 ($pthread_sha) for SWEET XNU 26.1"
