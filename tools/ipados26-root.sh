#!/bin/bash
# Build the complete iPadOS 26.1 (23B85, iPad11,6 / J171aAP) HFS+ RootFS image (24 GiB)
# for Xiaomi Redmi Note 13 4G (sapphire), including AEA decryption, arm64e -> ARMv8.0 De-PAC,
# optional Procursus/ElleKit/OpenSSH (--with-jb), and Sapphire arm64e userland (--userland-dir)!
#
# Usage (Linux / WSL2 as root):
#   sudo bash tools/ipados26-root.sh [--with-jb] [--ssh-key PUBKEY] [--root-pass 1234] \
#       [--userland-dir userland/obj-ipados26] <iPad_10.2_2020_26.1_23B85_Restore.ipsw> <OUTDIR> [hfsplus.ko]
#
# Decryption keys unwrapped from Apple public FCS key for iPad11,6/11,7 26.1 (23B85):
#   OS       (043-54294-118.dmg.aea): base64:NxXyEsTtpTiVIW4bj5haBWUrcLAdjWbDbDzSjommmNk=
#   SystemOS (043-54733-126.dmg.aea): base64:IQSeCzXPyPvu4Iyf8V6VdR98z2oKTgdDxqLED+f74f0=
#   AppOS    (043-54062-129.dmg):     unencrypted raw APFS
set -euo pipefail

with_jb=0
jb_opts=()
while [[ ${1:-} == --* ]]; do
	case "$1" in
	--with-jb) with_jb=1; shift ;;
	--ssh-key) jb_opts+=(--ssh-key "$2"); with_jb=1; shift 2 ;;
	--root-pass) jb_opts+=(--root-pass "$2"); with_jb=1; shift 2 ;;
	--userland-dir) jb_opts+=(--userland-dir "$2"); with_jb=1; shift 2 ;;
	*) echo "Unknown option: $1" >&2; exit 2 ;;
	esac
done

[[ $# -ge 2 ]] || { sed -n '2,13p' "$0"; exit 2; }
[[ $EUID == 0 ]] || { echo "run as root (sudo)" >&2; exit 1; }

ipsw=$(realpath "$1")
outdir=$(realpath "$2")
ko=${3:-}
here=$(dirname "$(realpath "$0")")
repo=$(realpath "$here/..")
work="$outdir/work-ipados26"
img="$outdir/ipados26-root.img"

OS_AEA="043-54294-118.dmg.aea"
OS_KEY="base64:NxXyEsTtpTiVIW4bj5haBWUrcLAdjWbDbDzSjommmNk="
SYSOS_AEA="043-54733-126.dmg.aea"
SYSOS_KEY="base64:IQSeCzXPyPvu4Iyf8V6VdR98z2oKTgdDxqLED+f74f0="
APPOS_DMG="043-54062-129.dmg"

if ! grep -qw hfsplus /proc/filesystems; then
	[[ -n $ko ]] || { echo "no HFS+ in this kernel: pass path to hfsplus.ko" >&2; exit 1; }
	insmod "$ko"
fi

mkdir -p "$work" "$outdir" "$repo/research/ipsw-ipados26"

echo "[1/5] Extracting DeviceTrees and DMGs from iPadOS 26.1 IPSW..."
unzip -o -q "$ipsw" \
	"Firmware/all_flash/DeviceTree.j171aap.im4p" \
	"Firmware/all_flash/DeviceTree.j172aap.im4p" \
	"$OS_AEA" "$SYSOS_AEA" "$APPOS_DMG" \
	-d "$work"

python3 "$here/ipados26_im4p_aea.py" im4p \
	"$work/Firmware/all_flash/DeviceTree.j171aap.im4p" \
	"$repo/research/ipsw-ipados26/j171aap.adt"
python3 "$here/ipados26_im4p_aea.py" im4p \
	"$work/Firmware/all_flash/DeviceTree.j172aap.im4p" \
	"$repo/research/ipsw-ipados26/j172aap.adt"

decrypt_aea() {
	local src="$1" dst="$2" key="$3"
	if [[ -f "$dst" ]]; then
		echo "  $dst already decrypted"
		return
	fi
	if command -v ipsw >/dev/null 2>&1; then
		ipsw fw aea --key-val "$key" "$src" -o "$(dirname "$dst")"
	elif command -v aea >/dev/null 2>&1; then
		aea decrypt -i "$src" -o "$dst" -key-value "$key"
	else
		echo "ERROR: install 'ipsw' (https://github.com/blacktop/ipsw/releases) or 'aea' to stream-decrypt multi-GB .dmg.aea!" >&2
		echo "  (Key already unwrapped for you: $key)" >&2
		exit 1
	fi
}

echo "[2/5] Decrypting iPadOS 26.1 AEA images..."
decrypt_aea "$work/$OS_AEA" "$work/043-54294-118.dmg" "$OS_KEY"
decrypt_aea "$work/$SYSOS_AEA" "$work/043-54733-126.dmg" "$SYSOS_KEY"

echo "[3/5] Building 24 GiB journaled HFS+ volume from OS + SystemOS + AppOS..."
rm -f "$img" "$outdir/ipados26-root.simg"
bash "$here/mkipadosroot.sh" \
	"$work/043-54294-118.dmg" \
	"$work/043-54733-126.dmg" \
	"$work/$APPOS_DMG" \
	"$img" 24G

echo "[4/5] Applying iPadOS 26.1 bring-up tweaks + DSC offset extraction + arm64e De-PAC..."
mnt=$(mktemp -d)
fsck.hfsplus -fy "$img" | grep -v 'Illegal name' || true
mount -t hfsplus -o loop,force "$img" "$mnt"
trap 'mountpoint -q "$mnt" && umount "$mnt"; rmdir "$mnt"' EXIT
python3 "$here/ipados26-root-tweaks.py" "$mnt"

if [[ $with_jb -eq 1 ]]; then
	echo "[4b/5] Installing Procursus + Sileo + ElleKit + OpenSSH + Sapphire arm64e userland..."
	bash "$here/ipados26-jb-install.sh" --mounted "$mnt" --jb-cache "$work/jb-ipados26" \
		${jb_opts[@]+"${jb_opts[@]}"}
	# ipados26-jb-install.sh updates launchd.plist via mv (.new -> launchd.plist), breaking hardlinks;
	# re-link /System/Cryptexes/OS/System/Library/xpc/launchd.plist so all three paths match.
	if [[ -f "$mnt/private/preboot/Cryptexes/OS/System/Library/xpc/launchd.plist" && \
	      -d "$mnt/System/Cryptexes/OS/System/Library/xpc" ]]; then
		ln -f "$mnt/private/preboot/Cryptexes/OS/System/Library/xpc/launchd.plist" \
			"$mnt/System/Cryptexes/OS/System/Library/xpc/launchd.plist"
	fi
fi
umount "$mnt"

echo "[5/5] Verifying HFS+ filesystem and creating sparse image..."
fsck.hfsplus -f -y "$img" | grep -v 'Illegal name' || true
if command -v img2simg >/dev/null 2>&1; then
	img2simg "$img" "$outdir/ipados26-root.simg"
fi
ls -lh "$img"
