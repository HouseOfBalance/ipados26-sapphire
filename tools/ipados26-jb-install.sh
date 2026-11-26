#!/bin/bash
# Automated Procursus Rootless + Sileo + ElleKit + OpenSSH (root/1234 + key) + Sapphire Userland
# installer for iPadOS 26.1 (ipados26-root.img), including full arm64e -> ARMv8.0 De-PAC & rehash!
#
# Usage (Linux / WSL2 as root):
#   sudo bash tools/ipados26-jb-install.sh <ipados26-root.img | --mounted /mnt/dir> \
#       [--ssh-key ~/.ssh/id_ed25519.pub] \
#       [--root-pass 1234] \
#       [--userland-dir ~/ipados26-sapphire/userland/obj-ipados26] \
#       [--jb-cache ~/work/jb-ipados26] \
#       [--ko ~/modules/hfsplus.ko]
set -euo pipefail

[[ $# -ge 1 ]] || { sed -n '2,11p' "$0"; exit 2; }
[[ $EUID == 0 ]] || { echo "run as root (sudo)" >&2; exit 1; }

here=$(dirname "$(realpath "$0")")
repo=$(realpath "$here/..")

mounted_mode=0
target=""
if [[ "${1:-}" == "--mounted" ]]; then
	mounted_mode=1
	target=$(realpath "$2")
	shift 2
else
	target=$(realpath "$1")
	shift
fi

ssh_key=""
root_pass="1234"
userland_dir=""
jb_cache=""
ko=""

while [[ $# -gt 0 ]]; do
	case "$1" in
	--ssh-key) ssh_key=$(realpath "$2"); shift 2 ;;
	--root-pass) root_pass="$2"; shift 2 ;;
	--userland-dir) userland_dir=$(realpath "$2"); shift 2 ;;
	--jb-cache) jb_cache=$(realpath "$2"); shift 2 ;;
	--ko) ko=$(realpath "$2"); shift 2 ;;
	*) echo "Unknown option: $1" >&2; exit 2 ;;
	esac
done

if [[ -z "$jb_cache" ]]; then
	if [[ $mounted_mode -eq 1 ]]; then
		jb_cache="/tmp/jb-ipados26-cache"
	else
		jb_cache="$(dirname "$target")/jb-ipados26-cache"
	fi
fi
mkdir -p "$jb_cache"

# 1. Build Procursus + Sileo + ElleKit + OpenSSH tree via userland/jb/mk-jb.sh if not cached
if [[ ! -x "$jb_cache/root/var/jb/usr/sbin/sshd" ]]; then
	echo "[JB 1/5] Fetching & staging Procursus + Sileo + ElleKit + OpenSSH into $jb_cache..."
	JB_OUT="$jb_cache" bash "$repo/userland/jb/mk-jb.sh"
else
	echo "[JB 1/5] Reusing cached Procursus tree at $jb_cache/root/var/jb"
fi

mnt=""
cleanup() {
	if [[ $mounted_mode -eq 0 && -n "$mnt" ]]; then
		mountpoint -q "$mnt" && umount "$mnt"
		rmdir "$mnt" 2>/dev/null || true
	fi
}
trap cleanup EXIT

if [[ $mounted_mode -eq 1 ]]; then
	mnt="$target"
else
	if ! grep -qw hfsplus /proc/filesystems; then
		[[ -n "$ko" ]] || { echo "no HFS+ in kernel: pass --ko /path/to/hfsplus.ko" >&2; exit 1; }
		insmod "$ko"
	fi
	mnt=$(mktemp -d)
	mount -t hfsplus -o loop,force "$target" "$mnt"
fi

test -d "$mnt/private/var"
jb="$mnt/private/var/jb"

echo "[JB 2/5] Installing /private/var/jb, /Applications, and /bin real binaries..."
rm -rf "$jb"
mkdir -p "$jb"
(cd "$jb_cache/root/var/jb" && tar -cf - .) | (cd "$jb" && tar -xpf - --same-owner)

for d in var/mobile var/mobile/Library var/mobile/Library/Caches var/mobile/Library/Preferences \
         var/mobile/Documents var/lib/apt/sileolists var/lib/apt/sileolists/partial var/log usr/local/bin etc/apt/apt.conf.d; do
	mkdir -p "$jb/$d"
done
chown -R 501:501 "$jb/var/mobile"
chown 501:501 "$jb/var/lib/apt/sileolists" "$jb/var/lib/apt/sileolists/partial"
[[ -f "$jb/Applications/Sileo.app/giveMeRoot" ]] && chmod 4755 "$jb/Applications/Sileo.app/giveMeRoot"
# Sileo rootless Procursus marker + apt root sandbox (NOTES.md:543,550)
touch "$jb/.installed_dopamine"
chown 0:0 "$jb/.installed_dopamine"
chmod 644 "$jb/.installed_dopamine"
cat > "$jb/etc/apt/apt.conf.d/20sapphire-sandbox.conf" <<'APT_EOF'
APT::Sandbox::User "root";
APT_EOF
chmod 644 "$jb/etc/apt/apt.conf.d/20sapphire-sandbox.conf"

# Copy prefix apps into /Applications (LaunchServices requirement on HFS+ root)
for app in "$jb"/Applications/*.app; do
	[[ -d "$app" ]] || continue
	n=$(basename "$app")
	rm -rf "$mnt/Applications/$n"
	(cd "$(dirname "$app")" && tar -cf - "$n") | (cd "$mnt/Applications" && tar -xpf - --same-owner)
done

# Real file copies in /bin (Linux hfsplus file symlinks are not followed by XNU exec/dyld)
mkdir -p "$mnt/bin"
for c in sh bash chmod cp mkdir mv rm; do
	if [[ -e "$mnt/bin/$c" && ! -L "$mnt/bin/$c" ]]; then
		continue
	fi
	for cand in "$jb/bin/$c" "$jb/usr/bin/$c"; do
		if [[ -f "$cand" ]]; then
			rm -f "$mnt/bin/$c"
			cp -p "$(readlink -f "$cand")" "$mnt/bin/$c"
			chmod 755 "$mnt/bin/$c"
			chown 0:0 "$mnt/bin/$c"
			break
		fi
	done
done

echo "[JB 3/5] Configuring OpenSSH (port 22 & 2222, root/${root_pass}) + USB root shell (192.168.7.2:2323)..."
mkdir -p "$jb/etc/ssh"
for t in rsa ecdsa ed25519; do
	if [[ ! -f "$jb/etc/ssh/ssh_host_${t}_key" ]]; then
		ssh-keygen -q -t "$t" -N "" -C "" -f "$jb/etc/ssh/ssh_host_${t}_key"
	fi
	chmod 600 "$jb/etc/ssh/ssh_host_${t}_key"
	chmod 644 "$jb/etc/ssh/ssh_host_${t}_key.pub"
	chown 0:0 "$jb/etc/ssh/ssh_host_${t}_key" "$jb/etc/ssh/ssh_host_${t}_key.pub"
done

cfg="$jb/etc/ssh/sshd_config"
if [[ -f "$cfg" ]]; then
	sed -i 's/^#*UsePAM.*/UsePAM no/' "$cfg"
	sed -i 's/^#*PermitRootLogin.*/PermitRootLogin yes/' "$cfg"
	sed -i 's/^#*PasswordAuthentication.*/PasswordAuthentication yes/' "$cfg"
fi
cat >> "$cfg" <<'SSHD_EOF'

UsePAM no
PermitRootLogin yes
PasswordAuthentication yes
PubkeyAuthentication yes
SSHD_EOF

# Set root & mobile password hash in /var/jb/etc/master.passwd and pre-build OpenBSD-format
# /var/jb/etc/{pwd.db,spwd.db,passwd} offline via tools/mkpwdb.py (libiosexec only reads OpenBSD db 1.85!).
# Traditional DES crypt ("openssl passwd -salt sa") is accepted by BOTH Apple libSystem crypt() and libxcrypt.
if [[ -f "$jb/etc/master.passwd" && -n "$root_pass" ]]; then
	python3 - "$jb/etc/master.passwd" "$root_pass" "$here/mkpwdb.py" <<'PY'
import importlib.util, os, subprocess, sys
mp_path, pw, mkpwdb_py = sys.argv[1], sys.argv[2], sys.argv[3]
pwhash = None
try:
    out = subprocess.run(['openssl', 'passwd', '-salt', 'sa', pw],
                         check=True, capture_output=True, text=True).stdout.strip()
    if len(out) == 13 and out.startswith('sa'):
        pwhash = out
except Exception:
    pass
if not pwhash:
    try:
        import crypt
        pwhash = crypt.crypt(pw, 'sa')
    except Exception:
        pass
if pwhash:
    lines = open(mp_path, 'r', encoding='utf-8').read().splitlines()
    out_lines = []
    for ln in lines:
        if ln.startswith('root:') or ln.startswith('mobile:'):
            parts = ln.split(':')
            if len(parts) == 10:
                parts[1] = pwhash
                ln = ':'.join(parts)
        out_lines.append(ln)
    with open(mp_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(out_lines) + '\n')
    os.chmod(mp_path, 0o600)
    print("master.passwd updated for root & mobile (DES crypt, universal libSystem/libxcrypt)")

spec = importlib.util.spec_from_file_location('mkpwdb', mkpwdb_py)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
cnt = mod.generate_pwdb(mp_path, os.path.dirname(mp_path))
print(f"Pre-built {os.path.dirname(mp_path)}/{{pwd.db,spwd.db,passwd}} ({cnt} entries)")
PY
	chown 0:0 "$jb/etc/master.passwd" "$jb/etc/pwd.db" "$jb/etc/spwd.db" "$jb/etc/passwd"
fi

if [[ -n "$ssh_key" && -f "$ssh_key" ]]; then
	for hspec in "$mnt/private/var/root:0:0" "$jb/var/root:0:0" "$jb/var/mobile:501:501"; do
		IFS=: read -r hdir huid hgid <<< "$hspec"
		install -d -m 700 -o "$huid" -g "$hgid" "$hdir/.ssh"
		install -m 600 -o "$huid" -g "$hgid" "$ssh_key" "$hdir/.ssh/authorized_keys"
	done
fi

# Root-FS launchers under /redwood/bin invoked via /bin/sh (launchd refuses RunAtLoad jobs whose
# ProgramArguments[0] lives inside /var/jb!). NOTE: do NOT run Procursus's FreeBSD pwd_mkdb on
# /var/jb/etc/master.passwd because it overwrites OpenBSD pwd.db with v4 records libiosexec cannot read.
mkdir -p "$mnt/redwood/bin" "$mnt/redwood/etc"
if [[ -f "$repo/devices/sapphire/metal.args" ]]; then
	cp -p "$repo/devices/sapphire/metal.args" "$mnt/redwood/etc/metal.args"
	chmod 644 "$mnt/redwood/etc/metal.args"
	chown 0:0 "$mnt/redwood/etc/metal.args"
fi
if [[ -f "$here/ipados26_depac.py" ]]; then
	cp -p "$here/ipados26_depac.py" "$mnt/redwood/bin/ipados26_depac.py"
	chmod 755 "$mnt/redwood/bin/ipados26_depac.py"
	chown 0:0 "$mnt/redwood/bin/ipados26_depac.py"
fi
cat > "$jb/usr/local/bin/sapphire-jbprep.sh" <<'JBPREP'
#!/bin/sh
export PATH="/var/jb/usr/local/bin:/var/jb/usr/sbin:/var/jb/usr/bin:/var/jb/sbin:/var/jb/bin:/redwood/bin:/usr/sbin:/usr/bin:/sbin:/bin"
if [ -f /redwood/etc/metal.args ] && [ ! -f /var/tmp/metal.args ]; then
	cp -p /redwood/etc/metal.args /var/tmp/metal.args 2>/dev/null || true
fi
mkdir -p /var/tmp/hakios-metal-cache 2>/dev/null || true
if [ ! -f /var/jb/var/.sapphire_prep_done ]; then
	[ -x /var/jb/usr/libexec/firmware ] && /var/jb/usr/libexec/firmware >/var/jb/var/log/prep.log 2>&1 || true
	for pkg in debianutils apt dash bash zsh vi; do
		[ -x "/var/jb/Library/dpkg/info/$pkg.postinst" ] && \
			"/var/jb/Library/dpkg/info/$pkg.postinst" configure 99999 >>/var/jb/var/log/prep.log 2>&1 || true
	done
	touch /var/jb/var/.sapphire_prep_done
fi
if [ -x /redwood/bin/sapphire-ipa-install.sh ]; then
	/var/jb/bin/bash /redwood/bin/sapphire-ipa-install.sh >/dev/null 2>&1 &
fi
exit 0
JBPREP
chmod 755 "$jb/usr/local/bin/sapphire-jbprep.sh"
chown 0:0 "$jb/usr/local/bin/sapphire-jbprep.sh"
cp -p "$jb/usr/local/bin/sapphire-jbprep.sh" "$mnt/redwood/bin/sapphire-jbprep-run.sh"

cat > "$mnt/redwood/bin/sapphire-sshd-run.sh" <<'SSHDRUN'
#!/bin/sh
export PATH="/var/jb/usr/local/bin:/var/jb/usr/sbin:/var/jb/usr/bin:/var/jb/sbin:/var/jb/bin:/redwood/bin:/usr/sbin:/usr/bin:/sbin:/bin"
port="${1:-22}"
exec /var/jb/usr/sbin/sshd -D -e -p "$port"
SSHDRUN
chmod 755 "$mnt/redwood/bin/sapphire-sshd-run.sh" "$mnt/redwood/bin/sapphire-jbprep-run.sh"
chown 0:0 "$mnt/redwood/bin/sapphire-sshd-run.sh" "$mnt/redwood/bin/sapphire-jbprep-run.sh"

# Optional: Stage Sapphire arm64e Userland binaries/tweaks/Metal/Audio bundles if --userland-dir was passed
if [[ -n "$userland_dir" && -d "$userland_dir" ]]; then
	echo "[JB 3b/5] Staging Sapphire arm64e userland from $userland_dir..."
	mkdir -p "$mnt/redwood/bin" "$mnt/redwood/lib" "$mnt/redwood/etc" "$mnt/redwood/launchd" "$jb/usr/lib/TweakInject"
	[[ -d "$userland_dir/etc" ]] && cp -a "$userland_dir/etc/." "$mnt/redwood/etc/"
	[[ -d "$userland_dir/bin" ]] && cp -a "$userland_dir/bin/." "$mnt/redwood/bin/"
	[[ -d "$userland_dir/lib" ]] && cp -a "$userland_dir/lib/." "$mnt/redwood/lib/"
	if [[ -f "$mnt/redwood/lib/eagl.dylib" && ! -f "$mnt/redwood/lib/libredwood-gl.dylib" && -f "$mnt/redwood/lib/libOSMesa.8.dylib" ]]; then
		cp -p "$mnt/redwood/lib/eagl.dylib" "$mnt/redwood/lib/libredwood-gl.dylib"
	fi
	[[ -d "$userland_dir/tweaks" ]] && cp -a "$userland_dir/tweaks/." "$jb/usr/lib/TweakInject/"
	[[ -d "$userland_dir/launchd" ]] && cp -a "$userland_dir/launchd/." "$mnt/redwood/launchd/"
	for mb in HakiosMetal.bundle SapphireMetal.bundle; do
		if [[ -d "$userland_dir/bundles/$mb" ]]; then
			mkdir -p "$mnt/System/Library/Extensions"
			rm -rf "$mnt/System/Library/Extensions/$mb"
			cp -R "$userland_dir/bundles/$mb" "$mnt/System/Library/Extensions/"
		fi
	done
	if [[ -d "$userland_dir/bundles/RedwoodAudio.driver" ]]; then
		for hal_dir in "$mnt/Library/Audio/Plug-Ins/HAL" "$mnt/System/Library/Audio/Plug-Ins/HAL"; do
			mkdir -p "$hal_dir"
			rm -rf "$hal_dir/RedwoodAudio.driver"
			cp -R "$userland_dir/bundles/RedwoodAudio.driver" "$hal_dir/"
			chown -R 0:0 "$hal_dir/RedwoodAudio.driver"
		done
	fi
	for uapp in "$userland_dir"/apps/*.app; do
		[[ -d "$uapp" ]] || continue
		an=$(basename "$uapp")
		rm -rf "$mnt/Applications/$an"
		cp -R "$uapp" "$mnt/Applications/$an"
		chown -R 0:0 "$mnt/Applications/$an"
		[[ "$an" == "SapphireTools.app" ]] && chmod 6755 "$mnt/Applications/$an/SapphireTools"
		[[ "$an" == "Android.app" ]] && chmod 6755 "$mnt/Applications/$an/Android"
	done
	chown -R 0:0 "$mnt/redwood"
	chmod -R 755 "$mnt/redwood/bin" "$mnt/redwood/lib"
	[[ -f "$mnt/redwood/bin/sapphire-ipa-helper" ]] && chmod 4755 "$mnt/redwood/bin/sapphire-ipa-helper"
fi

echo "[JB 4/5] Registering OpenSSH, USB shell & Sapphire daemons in launchd.plist..."
for lp in "$mnt/System/Library/xpc/launchd.plist" \
          "$mnt/private/preboot/Cryptexes/OS/System/Library/xpc/launchd.plist"; do
	[[ -f "$lp" ]] || continue
	xattr_tmp=$(mktemp)
	getfattr -d -m - --absolute-names "$lp" > "$xattr_tmp" 2>/dev/null || true
	python3 - "$lp" "$mnt/redwood/launchd" <<'PY'
import glob, os, plistlib, sys
lp, rw_ld = sys.argv[1], sys.argv[2]
p = plistlib.load(open(lp, 'rb'))
d = p.setdefault('LaunchDaemons', {})
sysd = '/System/Library/LaunchDaemons/'
jb_path = '/var/jb/usr/local/bin:/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/var/jb/sbin:/redwood/bin:/usr/bin:/bin:/usr/sbin:/sbin'

d[sysd + 'org.sapphire.jbprep.plist'] = {
    'Label': 'org.sapphire.jbprep',
    'ProgramArguments': ['/bin/sh', '/redwood/bin/sapphire-jbprep-run.sh'],
    'RunAtLoad': True,
    'AbandonProcessGroup': True,
    'UserName': 'root',
    'EnvironmentVariables': {'PATH': jb_path, 'HOME': '/var/root'},
}
d[sysd + 'com.openssh.sshd.plist'] = {
    'Label': 'com.openssh.sshd',
    'ProgramArguments': ['/bin/sh', '/redwood/bin/sapphire-sshd-run.sh', '22'],
    'RunAtLoad': True, 'KeepAlive': True, 'ThrottleInterval': 15, 'UserName': 'root',
    'POSIXSpawnType': 'Interactive',
    'EnvironmentVariables': {'PATH': jb_path, 'HOME': '/var/root'},
    'StandardOutPath': '/var/jb/var/log/sshd-22.log',
    'StandardErrorPath': '/var/jb/var/log/sshd-22.log',
}
d[sysd + 'com.openssh.sshd2222.plist'] = {
    'Label': 'com.openssh.sshd2222',
    'ProgramArguments': ['/bin/sh', '/redwood/bin/sapphire-sshd-run.sh', '2222'],
    'RunAtLoad': True, 'KeepAlive': True, 'ThrottleInterval': 30, 'UserName': 'root',
    'POSIXSpawnType': 'Interactive',
    'EnvironmentVariables': {'PATH': jb_path, 'HOME': '/var/root'},
    'StandardOutPath': '/var/jb/var/log/sshd-2222.log',
    'StandardErrorPath': '/var/jb/var/log/sshd-2222.log',
}
d[sysd + 'org.sapphire.usbshell.plist'] = {
    'Label': 'org.sapphire.usbshell',
    'ProgramArguments': ['/bin/sh', '-c', 'exec /bin/bash -i 2>&1'],
    'Sockets': {'Listener': {'SockNodeName': '192.168.7.2', 'SockServiceName': '2323',
                             'SockFamily': 'IPv4', 'SockType': 'stream'}},
    'inetdCompatibility': {'Wait': False},
    'UserName': 'root', 'POSIXSpawnType': 'Interactive', 'SessionCreate': True,
    'EnvironmentVariables': {'PATH': jb_path, 'HOME': '/var/root', 'TERM': 'xterm-256color'},
}

if os.path.isdir(rw_ld):
    for pl in sorted(glob.glob(os.path.join(rw_ld, '*.plist'))):
        job = plistlib.load(open(pl, 'rb'))
        d[sysd + os.path.basename(pl)] = job

with open(lp + '.new', 'wb') as f:
    plistlib.dump(p, f, fmt=plistlib.FMT_BINARY)
print(f"{lp}: {len(d)} LaunchDaemons configured")
PY
	chown --reference="$lp" "$lp.new"
	chmod --reference="$lp" "$lp.new"
	mv "$lp.new" "$lp"
	[[ -s "$xattr_tmp" ]] && setfattr --restore="$xattr_tmp" 2>/dev/null || true
	rm -f "$xattr_tmp"
done
# Keep /System/Cryptexes/OS/System/Library/xpc/launchd.plist hard-linked after mv (.new -> launchd.plist)
if [[ -f "$mnt/private/preboot/Cryptexes/OS/System/Library/xpc/launchd.plist" && \
      -d "$mnt/System/Cryptexes/OS/System/Library/xpc" ]]; then
	ln -f "$mnt/private/preboot/Cryptexes/OS/System/Library/xpc/launchd.plist" \
		"$mnt/System/Cryptexes/OS/System/Library/xpc/launchd.plist"
elif [[ -f "$mnt/System/Library/xpc/launchd.plist" && \
        -d "$mnt/System/Cryptexes/OS/System/Library/xpc" ]]; then
	ln -f "$mnt/System/Library/xpc/launchd.plist" \
		"$mnt/System/Cryptexes/OS/System/Library/xpc/launchd.plist"
fi

# Force LaunchServices to register /Applications/* (Sileo, SapphireTools, Huella) on next boot
rm -f "$mnt"/private/var/mobile/Containers/Data/InternalDaemon/*/Library/Caches/*com.apple.LaunchServices-*.csstore 2>/dev/null || true

echo "[JB 5/5] Running arm64e -> ARMv8.0 De-PAC & CodeDirectory rehash on /private/var/jb, /Applications, /bin, /redwood, Metal & Audio bundles..."
for sub in "$jb" "$mnt/Applications" "$mnt/bin" "$mnt/redwood" \
           "$mnt/System/Library/Extensions/HakiosMetal.bundle" \
           "$mnt/System/Library/Extensions/SapphireMetal.bundle" \
           "$mnt/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver" \
           "$mnt/System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver"; do
	if [[ -e "$sub" ]]; then
		python3 "$here/ipados26_depac.py" --rewrite "$sub"
	fi
done
sync
echo "ipados26-jb-install.sh: DONE (SSH ports 22 & 2222 user=root pass=${root_pass}, USB shell 192.168.7.2:2323)"
