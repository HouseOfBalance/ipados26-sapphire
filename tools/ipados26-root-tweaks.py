#!/usr/bin/env python3
"""Apply SWEET/Sapphire bring-up tweaks AND arm64e->ARMv8.0 De-PAC to a mounted
iPadOS 26.1 (23B85, iPad11,6/11,7) HFS+ root image.

Usage:
  sudo python3 tools/ipados26-root-tweaks.py MOUNTED_ROOT [--skip-depac]
"""
import glob
import os
import plistlib
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ipados26_depac as depac
import ipados26_dsc_offsets as dsc_offsets
import ipados26_ca_metallib

BUILD, VERSION = '23B85', '26.1'
MOBILE = 501

# Daemons in iPadOS 26.1 that crash-loop without Apple SEP / ANE / Exclaves / Baseband, plus
# diskarbitrationd (must not probe, fsck, remount, or unmount HFS+ iosroot or Android/Qualcomm
# GPT partitions on /dev/disk0-1; removing the job leaves com.apple.DiskArbitration.diskarbitrationd
# unregistered in Mach bootstrap so DASessionCreate fails fast with BOOTSTRAP_UNKNOWN_SERVICE
# instead of blocking callers on synchronous MIG mach_msg RPCs).
DISABLED_DAEMON_SUBSTRINGS = (
    'com.apple.CommCenter',
    'com.apple.biometrickitd',
    'com.apple.seputil',
    'com.apple.seserviced',
    'com.apple.aned',
    'com.apple.aneuserd',
    'com.apple.exclaves',
    'com.apple.tightbeam',
    'com.apple.diskarbitrationd',
)


def chown_safe(path, uid, gid, follow_symlinks=True):
    try:
        if follow_symlinks:
            if hasattr(os, 'chown'):
                os.chown(path, uid, gid)
        elif hasattr(os, 'lchown'):
            os.lchown(path, uid, gid)
    except PermissionError:
        if hasattr(os, 'geteuid') and os.geteuid() == 0:
            raise
    except OSError:
        if follow_symlinks and hasattr(os, 'geteuid') and os.geteuid() == 0:
            raise


def load(path):
    with open(path, 'rb') as f:
        return plistlib.load(f)


def save(path, data, uid, gid, mode, fmt=plistlib.FMT_BINARY):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        plistlib.dump(data, f, fmt=fmt)
    chown_safe(path, uid, gid)
    os.chmod(path, mode)


def merge_pref(root, name, values):
    path = os.path.join(root, 'private/var/mobile/Library/Preferences', name + '.plist')
    data = load(path) if os.path.exists(path) else {}
    data.update(values)
    save(path, data, MOBILE, MOBILE, 0o600)
    print('pref', name, sorted(values))


def patch_launchd(root):
    candidates = [
        os.path.join(root, 'System/Library/xpc/launchd.plist'),
        os.path.join(root, 'private/preboot/Cryptexes/OS/System/Library/xpc/launchd.plist'),
        os.path.join(root, 'System/Cryptexes/OS/System/Library/xpc/launchd.plist'),
    ]
    seen_cache_inodes = set()
    disabled_labels = {'com.apple.diskarbitrationd'}
    for path in candidates:
        if not os.path.exists(path) or os.path.islink(path):
            continue
        st = os.stat(path)
        ino_key = (st.st_dev, st.st_ino)
        if st.st_ino and ino_key in seen_cache_inodes:
            continue
        if st.st_ino:
            seen_cache_inodes.add(ino_key)
        plist = load(path)
        jobs = plist.get('LaunchDaemons', {})
        renamed = 0
        for job in jobs.values():
            if '_PanicOnCrash' in job:
                job['_PanicOnCrasX'] = job.pop('_PanicOnCrash')
                renamed += 1
            if '_PanicOnNonZeroExit' in job:
                job['_PanicOnNonZeroExiX'] = job.pop('_PanicOnNonZeroExit')
                renamed += 1
        removed = [
            k for k, j in jobs.items()
            if any(sub in k or sub in j.get('Label', '') for sub in DISABLED_DAEMON_SUBSTRINGS)
        ]
        for k in removed:
            lbl = jobs[k].get('Label')
            if lbl:
                disabled_labels.add(lbl)
            del jobs[k]
        save(path, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)
        print(f'{os.path.relpath(path, root)}: {renamed} panic flags disarmed, {len(removed)} daemons removed')

    # Also mark standalone diskarbitrationd LaunchDaemon plists Disabled=True on disk
    da_plists = [
        os.path.join(root, 'System/Library/LaunchDaemons/com.apple.diskarbitrationd.plist'),
        os.path.join(root, 'private/preboot/Cryptexes/OS/System/Library/LaunchDaemons/com.apple.diskarbitrationd.plist'),
        os.path.join(root, 'System/Cryptexes/OS/System/Library/LaunchDaemons/com.apple.diskarbitrationd.plist'),
    ]
    seen_da_inodes = set()
    for da_path in da_plists:
        if not os.path.isfile(da_path) or os.path.islink(da_path):
            continue
        st = os.stat(da_path)
        ino_key = (st.st_dev, st.st_ino)
        if st.st_ino and ino_key in seen_da_inodes:
            continue
        if st.st_ino:
            seen_da_inodes.add(ino_key)
        with open(da_path, 'rb') as f:
            raw = f.read()
        fmt = plistlib.FMT_BINARY if raw[:8] == b'bplist00' else plistlib.FMT_XML
        da_data = plistlib.loads(raw)
        if da_data.get('Disabled') is not True:
            da_data['Disabled'] = True
            save(da_path, da_data, st.st_uid, st.st_gid, st.st_mode & 0o7777, fmt)
            print(f'{os.path.relpath(da_path, root)}: Disabled=True')

    # Seed launchd system-domain override database so launchctl/launchd keeps disabled daemons off
    launchd_db_dir = os.path.join(root, 'private/var/db/com.apple.xpc.launchd')
    os.makedirs(launchd_db_dir, exist_ok=True)
    chown_safe(launchd_db_dir, 0, 0)
    os.chmod(launchd_db_dir, 0o755)
    disabled_plist = os.path.join(launchd_db_dir, 'disabled.plist')
    disabled_data = load(disabled_plist) if os.path.isfile(disabled_plist) else {}
    for lbl in sorted(disabled_labels):
        disabled_data[lbl] = True
    save(disabled_plist, disabled_data, 0, 0, 0o644, plistlib.FMT_XML)
    print(f'launchd disabled.plist seeded ({len(disabled_data)} labels)')


def setup_containers(root):
    # Ensure /bin/sh exists as a real executable copy of /bin/bash (not a Linux hfsplus symlink)
    sh_path = os.path.join(root, 'bin/sh')
    bash_path = os.path.join(root, 'bin/bash')
    if os.path.isfile(bash_path) and not os.path.isfile(sh_path):
        if os.path.islink(sh_path):
            os.unlink(sh_path)
        shutil.copyfile(bash_path, sh_path)
        chown_safe(sh_path, 0, 0)
        os.chmod(sh_path, 0o755)
        print('staged /bin/sh from /bin/bash')

    base = os.path.join(root, 'private/var/containers')
    for sub, uid in (('Bundle', 33), ('Data/System', 0), ('Shared/SystemGroup', 0), ('Temp', 0)):
        d = os.path.join(base, sub)
        if not os.path.isdir(d):
            os.makedirs(d)
            chown_safe(d, uid, uid)
            os.chmod(d, 0o755)
            print('mkdir', d)

    # Pre-create /private/var/mobile/Containers hierarchy owned by mobile:mobile (501:501) so
    # containermanagerd_system (uid=0) never creates Containers or Data owned by root:501, which
    # would block containermanagerd (uid=501) from creating Data/InternalDaemon or Shared/AppGroup.
    mob_base = os.path.join(root, 'private/var/mobile/Containers')
    for rel in (
        '',
        'Data',
        'Data/Application',
        'Data/InternalDaemon',
        'Data/PluginKitPlugin',
        'Data/TempDir',
        'Shared',
        'Shared/AppGroup',
        'Staging',
        'Temp',
    ):
        d = os.path.join(mob_base, rel) if rel else mob_base
        os.makedirs(d, exist_ok=True)
        chown_safe(d, MOBILE, MOBILE)
        os.chmod(d, 0o755)
    print('mobile Containers hierarchy ready (501:501):', mob_base)

    # Persistent Adreno 610 ir3 Metal shader cache across reboots (checked first by HakiosMetal / HakiosMetalCA)
    shader_cache = os.path.join(root, 'private/var/db/hakios-metal-cache')
    os.makedirs(shader_cache, exist_ok=True)
    chown_safe(shader_cache, 0, 0)
    os.chmod(shader_cache, 0o1777)
    print('shader cache ready:', shader_cache)

    # Stage default per-device Adreno 610 Metal args and offline De-PAC helper into /redwood
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    metal_args_src = os.path.join(repo_root, 'devices/sapphire/metal.args')
    metal_args_dst = os.path.join(root, 'redwood/etc/metal.args')
    if os.path.isfile(metal_args_src) and not os.path.lexists(metal_args_dst):
        os.makedirs(os.path.dirname(metal_args_dst), exist_ok=True)
        shutil.copyfile(metal_args_src, metal_args_dst)
        chown_safe(metal_args_dst, 0, 0)
        os.chmod(metal_args_dst, 0o644)
        print('staged default metal.args:', metal_args_dst)
    depac_src = os.path.join(repo_root, 'tools/ipados26_depac.py')
    depac_dst = os.path.join(root, 'redwood/bin/ipados26_depac.py')
    if os.path.isfile(depac_src) and not os.path.lexists(depac_dst):
        os.makedirs(os.path.dirname(depac_dst), exist_ok=True)
        shutil.copyfile(depac_src, depac_dst)
        chown_safe(depac_dst, 0, 0)
        os.chmod(depac_dst, 0o755)
        print('staged ipados26_depac.py:', depac_dst)


def find_container(root, kind, identifier):
    for meta in glob.glob(os.path.join(root, 'private/var/containers', kind, '*',
                                       '.com.apple.mobile_container_manager.metadata.plist')):
        if load(meta).get('MCMMetadataIdentifier') == identifier:
            return os.path.dirname(meta)
    return None


def seed_activation(root):
    ark_payload = {
        '-ActivationState': 'Activated',
        '-BrickState': False,
        '-BuildVersion': BUILD,
        '-ProductVersion': VERSION,
        '-DataMigrationCompleted': True,
        '-PreSydroDataMigrationCompleted': True,
    }
    stale = os.path.join(root, 'private/var/containers/Data/System/mobileactivationd-sweet')
    if os.path.isdir(stale):
        shutil.rmtree(stale)

    # Seed legacy Sydro migration source so mobileactivationd's _perform_sydro_data_migration_tasks
    # migrates ActivationState=Activated cleanly into the freshly created UUID System container.
    mad_dir = os.path.join(root, 'private/var/mobile/Library/mad')
    os.makedirs(mad_dir, exist_ok=True)
    chown_safe(mad_dir, MOBILE, MOBILE)
    os.chmod(mad_dir, 0o755)
    save(os.path.join(mad_dir, 'data_ark.plist'), ark_payload, MOBILE, MOBILE, 0o600)
    print('legacy mad data_ark seeded:', mad_dir)

    c = find_container(root, 'Data/System', 'com.apple.mobileactivationd')
    if c is not None:
        d = os.path.join(c, 'Library/internal')
        os.makedirs(d, exist_ok=True)
        chown_safe(d, MOBILE, MOBILE)
        os.chmod(d, 0o755)
        save(os.path.join(d, 'data_ark.plist'), ark_payload, MOBILE, MOBILE, 0o600)
        print('system container data_ark seeded:', d)


def patch_audio_and_media(root):
    src = os.path.join(root, 'Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin')
    dst_dir = os.path.join(root, 'Library/Audio/Plug-Ins/HAL.disabled')
    if os.path.exists(src):
        os.makedirs(dst_dir, exist_ok=True)
        shutil.move(src, dst_dir)
        print('VirtualAudio.plugin disabled')
    for conf in glob.glob(os.path.join(root, 'System/Library/Frameworks/MediaToolbox.framework/*/SubServerConfiguration.plist')):
        st = os.stat(conf)
        data = load(conf)
        data['StartRecorderServer'] = False
        save(conf, data, st.st_uid, st.st_gid, st.st_mode & 0o7777, plistlib.FMT_XML)
        print('StartRecorderServer off:', os.path.relpath(conf, root))


def ensure_cryptex_paths(root):
    """Mirror /private/preboot/Cryptexes/{OS,App} into /System/Cryptexes/{OS,App}
    using real directories, hard-linked regular files, and preserved symlinks.

    iPadOS 26.1 (iOS 17+) OS.dmg ships empty mountpoint directories at /System/Cryptexes/{OS,App}
    instead of symlinks to /private/preboot/Cryptexes/{OS,App}, while ignition_level=1 skips
    mounting Cryptex DMGs. Real directories + hard links cost zero extra blocks, keep canonical
    F_GETPATH (/System/Cryptexes/{OS,App}/...) for dyld4 and LaunchServices, and preserve '..'."""
    cryptex_base = os.path.join(root, 'System/Cryptexes')
    if os.path.islink(cryptex_base):
        os.unlink(cryptex_base)
    os.makedirs(cryptex_base, exist_ok=True)
    chown_safe(cryptex_base, 0, 0)
    os.chmod(cryptex_base, 0o755)

    for name in ('OS', 'App'):
        src_root = os.path.join(root, 'private/preboot/Cryptexes', name)
        dst_root = os.path.join(cryptex_base, name)
        if os.path.islink(src_root) or not os.path.isdir(src_root):
            continue
        if os.path.islink(dst_root):
            os.unlink(dst_root)
        st_top = os.lstat(src_root)
        if not os.path.isdir(dst_root):
            os.makedirs(dst_root, exist_ok=True)
            chown_safe(dst_root, st_top.st_uid, st_top.st_gid)
            os.chmod(dst_root, st_top.st_mode & 0o7777)

        linked_files = 0
        copied_symlinks = 0
        for dirpath, dirnames, filenames in os.walk(src_root, followlinks=False):
            rel_dir = os.path.relpath(dirpath, src_root)
            dst_dir = dst_root if rel_dir == '.' else os.path.join(dst_root, rel_dir)
            if os.path.islink(dst_dir):
                os.unlink(dst_dir)
            if not os.path.isdir(dst_dir):
                st_d = os.lstat(dirpath)
                os.makedirs(dst_dir, exist_ok=True)
                chown_safe(dst_dir, st_d.st_uid, st_d.st_gid)
                os.chmod(dst_dir, st_d.st_mode & 0o7777)

            symlink_dir_set = {d for d in dirnames if os.path.islink(os.path.join(dirpath, d))}
            symlink_dirs = [d for d in dirnames if d in symlink_dir_set]
            if symlink_dirs:
                dirnames[:] = [d for d in dirnames if d not in symlink_dir_set]

            for d in dirnames:
                sd = os.path.join(dirpath, d)
                dd = os.path.join(dst_dir, d)
                if os.path.islink(dd):
                    os.unlink(dd)
                if not os.path.isdir(dd):
                    st_sub = os.lstat(sd)
                    os.makedirs(dd, exist_ok=True)
                    chown_safe(dd, st_sub.st_uid, st_sub.st_gid)
                    os.chmod(dd, st_sub.st_mode & 0o7777)

            for fn in symlink_dirs + filenames:
                sp = os.path.join(dirpath, fn)
                tp = os.path.join(dst_dir, fn)
                if os.path.islink(sp):
                    target = os.readlink(sp)
                    is_dir_link = (fn in symlink_dir_set) or os.path.isdir(sp)
                    if os.path.islink(tp):
                        if os.readlink(tp) == target:
                            continue
                        os.unlink(tp)
                    elif os.path.isdir(tp):
                        try:
                            os.rmdir(tp)
                        except OSError:
                            continue
                    elif os.path.lexists(tp):
                        continue
                    st_l = os.lstat(sp)
                    os.symlink(target, tp, target_is_directory=is_dir_link)
                    chown_safe(tp, st_l.st_uid, st_l.st_gid, follow_symlinks=False)
                    copied_symlinks += 1
                elif os.path.isfile(sp):
                    if os.path.islink(tp):
                        os.unlink(tp)
                    if not os.path.lexists(tp):
                        os.link(sp, tp)
                        linked_files += 1

        if linked_files or copied_symlinks:
            print(f'System/Cryptexes/{name}: {linked_files} files hard-linked, {copied_symlinks} symlinks copied')


def ensure_dyld_shared_cache_links(root):
    """Ensure dyld_shared_cache_arm64e* is hard-linked across System/Library/{Caches/com.apple.dyld,dyld}
    under /, /System/Cryptexes/OS, and /private/preboot/Cryptexes/OS (zero extra space)."""
    dsc_rels = ('System/Library/Caches/com.apple.dyld', 'System/Library/dyld')
    prefixes = ['private/preboot/Cryptexes/OS', 'System/Cryptexes/OS', '']
    sources = [
        os.path.join(root, pfx, rel) if pfx else os.path.join(root, rel)
        for pfx in prefixes
        for rel in dsc_rels
    ]
    src_dir = next(
        (
            s for s in sources
            if os.path.isdir(s) and any(
                os.path.isfile(os.path.join(s, fn)) and not os.path.islink(os.path.join(s, fn))
                for fn in os.listdir(s)
            )
        ),
        None,
    )
    if not src_dir:
        return

    targets = [
        os.path.join(root, 'System/Library/Caches/com.apple.dyld'),
        os.path.join(root, 'System/Library/dyld'),
        os.path.join(root, 'System/Cryptexes/OS/System/Library/Caches/com.apple.dyld'),
        os.path.join(root, 'System/Cryptexes/OS/System/Library/dyld'),
    ]
    if os.path.isdir(os.path.join(root, 'private/preboot/Cryptexes/OS')):
        targets.extend([
            os.path.join(root, 'private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld'),
            os.path.join(root, 'private/preboot/Cryptexes/OS/System/Library/dyld'),
        ])

    for tdir in targets:
        os.makedirs(tdir, exist_ok=True)
        for fn in os.listdir(src_dir):
            sp = os.path.join(src_dir, fn)
            tp = os.path.join(tdir, fn)
            if os.path.isfile(sp) and not os.path.islink(sp) and not os.path.lexists(tp):
                os.link(sp, tp)
                print(f'linked DSC -> {os.path.relpath(tp, root)}')


def run_depac_all(root):
    from collections import Counter
    ensure_cryptex_paths(root)
    ensure_dyld_shared_cache_links(root)
    stats = Counter()
    print('=== Running arm64e -> ARMv8.0 De-PAC & CodeDirectory Rehash on RootFS ===')
    # 1. Rewrite dyld_shared_cache_arm64e* (avoiding duplicate hardlinks via inode tracking)
    seen_inodes = set()
    dsc_patterns = [
        os.path.join(root, 'System/Library/dyld/dyld_shared_cache_arm64e*'),
        os.path.join(root, 'System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e*'),
        os.path.join(root, 'System/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_arm64e*'),
        os.path.join(root, 'System/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e*'),
        os.path.join(root, 'private/preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_arm64e*'),
        os.path.join(root, 'private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e*'),
    ]
    for pat in dsc_patterns:
        for p in sorted(glob.glob(pat)):
            if os.path.isfile(p) and not os.path.islink(p):
                st = os.stat(p)
                if st.st_ino in seen_inodes:
                    continue
                seen_inodes.add(st.st_ino)
                c = depac.process_dsc_file(p, rewrite=True, stats=stats)
                print(f'  DSC {os.path.relpath(p, root)}: {c} PAC insns rewritten')

    # 2. Rewrite standalone Mach-O executables/dylibs across RootFS
    macho_files = 0
    macho_insns = 0
    for dirpath, _dirs, fnames in os.walk(root):
        for fn in fnames:
            if fn.startswith('dyld_shared_cache_'):
                continue
            p = os.path.join(dirpath, fn)
            if os.path.islink(p) or not os.path.isfile(p):
                continue
            st = os.stat(p)
            if st.st_ino in seen_inodes or st.st_size < 4096:
                continue
            seen_inodes.add(st.st_ino)
            c = depac.process_macho_file(p, rewrite=True, stats=stats)
            if c > 0:
                macho_files += 1
                macho_insns += c
    print(f'  Mach-O RootFS: {macho_files} binaries updated, {macho_insns} PAC insns rewritten')
    for k, v in sorted(stats.items()):
        print(f'    {k:24s}: {v}')


def main():
    skip_depac = '--skip-depac' in sys.argv
    args = [a for a in sys.argv[1:] if a != '--skip-depac']
    if len(args) != 1 or not os.path.isdir(os.path.join(args[0], 'System')):
        sys.exit(__doc__)
    root = args[0]

    ensure_cryptex_paths(root)
    ensure_dyld_shared_cache_links(root)
    patch_launchd(root)
    setup_containers(root)
    seed_activation(root)
    patch_audio_and_media(root)

    merge_pref(root, 'com.apple.migration', {
        'LastSystemVersion': BUILD,
        'DMUserDataDisposition': {'SyntheticDidUpgrade': False, 'buildVersion': BUILD},
    })
    merge_pref(root, 'com.apple.datamigrator', {
        'DataMigrationAlreadyRan': True,
        'LastBuildVersion': BUILD,
        'LastSystemVersion': VERSION,
    })
    merge_pref(root, 'com.apple.springboard', {
        'BKNoWatchdogs': True,
        'FBLaunchWatchdogExceptions': {'com.apple.purplebuddy': 120.0},
        'MCForceShowCloudConfigurationUI': False,
        'SBDisableParallax': True,
        'SBDisableHomeScreenParallax': True,
        'SBReduceMotion': True,
        'SBChamoisWindowingEnabled': False,
    })
    merge_pref(root, 'com.apple.Accessibility', {
        'EnhancedBackgroundContrastEnabled': 1,
        'EnhanceBackgroundContrast': True,
        'ReduceMotionEnabled': 1,
        'ReduceMotion': True,
        'ReduceTransparency': True,
        'DifferentiateWithoutColor': False,
    })
    merge_pref(root, 'com.apple.QuartzCore', {
        'CADisableMinimumFrameDurationOnPhone': True,
    })
    merge_pref(root, 'com.apple.purplebuddy', {
        'SetupDone': True,
        'SetupFinishedAllSteps': True,
        'SetupVersion': 19,
        'daemonizeNeedsToRun': False,
    })
    merge_pref(root, 'com.apple.purplebuddy.notbackedup', {
        k: False for k in (
            'ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy',
            'ShouldRunKeychainSyncMiniBuddy', 'ShouldRunMesaMiniBuddy',
            'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy',
            'ShouldRunIntelligenceMiniBuddy', 'ShouldRunAppearanceMiniBuddy',
            'SilentICDPUpgrade',
        )
    })
    merge_pref(root, 'com.apple.managedconfiguration.notbackedup', {
        'LockdownActivationIndicatesCloudConfigurationAvailable': False,
    })

    if not skip_depac:
        run_depac_all(root)
    try:
        ipados26_ca_metallib.generate_ca_metallib(root)
    except Exception as exc:
        print(f'warning: QuartzCore metallib generation skipped ({exc})', file=sys.stderr)
    try:
        dsc_offsets.generate_offsets_conf(root)
    except Exception as exc:
        print(f'warning: DSC offset scan skipped ({exc})', file=sys.stderr)


if __name__ == '__main__':
    main()
