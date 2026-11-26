#!/bin/bash
# Build the SWEET iOS root: one case-sensitive HFS+ image holding an iOS OS
# root filesystem plus its cryptexes, for the userdata partition (rd=ufsroot0).
#
#   mkipadosroot.sh OS.dmg SYSTEMOS.dmg APPOS.dmg OUT.img [SIZE]     (SIZE default 16G)
#
# The IPSW images are raw APFS with LZBITMAP-compressed files: they are read
# through apfs-fuse (decompressing), so the HFS+ copy is uncompressed. On a
# device /private/preboot is its own volume holding the cryptexes and iOS 17-26
# mounts them directly onto /System/Cryptexes/{OS,App} (shipping empty mountpoint
# directories there in OS.dmg); here both /private/preboot/Cryptexes/{OS,App}
# and /System/Cryptexes/{OS,App} live inside the one HFS+ volume via hard links.
# Owners, modes, symlinks and hard links are preserved (rsync -aH as root).
#
# The volume is journaled (XNU replays the journal after a panic; Linux only
# mounts it read-write with -o force and leaves the journal empty). SWEET
# additions: /System/Library/Caches/com.apple.dyld hard-links the SystemOS
# cryptex shared cache (ignition_level=1 looks there), and
# /private/var/keybags/systembag.kb is the plain-plist system keybag that
# keybagd reads with /defaults/no-effaceable-storage (tools/mkadt.py).
set -euo pipefail

[[ $# -ge 4 ]] || { sed -n '2,12p' "$0"; exit 2; }
os=$(realpath "$1") sysos=$(realpath "$2") appos=$(realpath "$3") out=$4 size=${5:-16G}
[[ ! -e $out ]] || { echo "refusing to overwrite $out" >&2; exit 1; }

work=$(mktemp -d)
mounts=()
cleanup() {
	local busy=0
	for m in "${mounts[@]}"; do
		if mountpoint -q "$m"; then
			sudo umount "$m" 2>/dev/null || true
		fi
		mountpoint -q "$m" && busy=1
	done
	# Never recurse into a directory that is still mounted (the root is writable).
	if [[ $busy == 0 ]]; then
		rm -f "$work"/*.dmg
		rmdir "$work"/* "$work" 2>/dev/null || true
	else
		echo "still mounted under $work: left in place" >&2
	fi
}
trap cleanup EXIT

# apfs-fuse splits its options on ',' (e.g. in iPad11,6 paths): use links.
# Keep the .dmg name: apfs-fuse only parses UDIF images (the OS one is UDIF,
# the cryptexes are raw APFS) when the file name ends in .dmg.
apfs_mount() {
	ln -s "$1" "$work/$2.dmg"
	mkdir "$work/$2"
	# As root: a user's FUSE mount is unreadable to the root rsync below.
	sudo "$(command -v apfs-fuse)" "$work/$2.dmg" "$work/$2"
	mounts+=("$work/$2")
}
apfs_mount "$os" os
apfs_mount "$sysos" sysos
apfs_mount "$appos" appos

truncate -s "$size" "$out"
mkfs.hfsplus -s -J -v iPadOSRoot "$out"
mkdir "$work/root"
sudo mount -t hfsplus -o loop,force "$out" "$work/root"
mounts=("$work/root" "${mounts[@]}")

copy() {
	echo "copying $1 -> ${2#"$work/root"}"
	sudo mkdir -p "$2"
	sudo rsync -aH --numeric-ids "$1/" "$2/"
}
copy "$work/os/root" "$work/root"
copy "$work/sysos/root" "$work/root/private/preboot/Cryptexes/OS"
copy "$work/appos/root" "$work/root/private/preboot/Cryptexes/App"

# iOS 17-26 OS.dmg ships empty mountpoint directories at /System/Cryptexes/{OS,App}
# instead of iOS 16's symlinks to /private/preboot/Cryptexes/{OS,App}, and ignition_level=1
# skips Cryptex DMG mounting. Populate real directories at /System/Cryptexes/{OS,App}
# via hard links (zero extra blocks, canonical F_GETPATH, intact '..' traversal).
sudo test -L "$work/root/System/Cryptexes" && sudo rm -f "$work/root/System/Cryptexes"
sudo mkdir -p "$work/root/System/Cryptexes"
for c in OS App; do
	src_tree="$work/root/private/preboot/Cryptexes/$c"
	dst_tree="$work/root/System/Cryptexes/$c"
	sudo test -d "$src_tree" || continue
	sudo test -L "$dst_tree" && sudo rm -f "$dst_tree"
	sudo mkdir -p "$dst_tree"
	sudo rsync -a --numeric-ids -f '+ */' -f '- *' "$src_tree/" "$dst_tree/"
	sudo bash -c '
		set -euo pipefail
		src_tree=$1 dst_tree=$2
		cd "$src_tree"
		find . ! -type d -print0 | while IFS= read -r -d "" rel; do
			rel=${rel#./}
			src="$src_tree/$rel"
			dst="$dst_tree/$rel"
			if [[ -L "$src" ]]; then
				[[ -d "$dst" && ! -L "$dst" ]] && rmdir "$dst" 2>/dev/null || true
				rm -f "$dst"
				cp -P "$src" "$dst"
			elif [[ -f "$src" ]]; then
				ln -f "$src" "$dst"
			fi
		done
	' _ "$src_tree" "$dst_tree"
	echo "mirrored Cryptex $c -> /System/Cryptexes/$c"
done

# Shared cache where dyld and frameworks look across iOS 16 and iOS/iPadOS 17-26:
# hard-link into both System/Library/Caches/com.apple.dyld and System/Library/dyld
# under /, /System/Cryptexes/OS, and /private/preboot/Cryptexes/OS (zero extra space).
dsc_targets=(
	"$work/root/System/Library/Caches/com.apple.dyld"
	"$work/root/System/Library/dyld"
	"$work/root/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld"
	"$work/root/System/Cryptexes/OS/System/Library/dyld"
	"$work/root/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld"
	"$work/root/private/preboot/Cryptexes/OS/System/Library/dyld"
)
for dsc_rel in System/Library/Caches/com.apple.dyld System/Library/dyld; do
	dsc="$work/root/private/preboot/Cryptexes/OS/$dsc_rel"
	if sudo test -d "$dsc"; then
		sudo mkdir -p "${dsc_targets[@]}"
		sudo bash -c '
			shopt -s nullglob
			dsc=$1; shift
			for f in "$dsc"/*; do
				[[ -f "$f" && ! -L "$f" ]] || continue
				bn=${f##*/}
				for td in "$@"; do
					[[ "$f" -ef "$td/$bn" ]] && continue
					ln -f "$f" "$td/$bn" 2>/dev/null || true
				done
			done
		' _ "$dsc" "${dsc_targets[@]}"
	fi
done

# dyld does not search the cryptexes at ignition level 1: hard-link every
# cryptex file that has no counterpart in the OS root (libobjc-trampolines,
# libffi-trampolines, libstdc++, ...) to its root path.
for c in OS App; do
	(cd "$work/root/private/preboot/Cryptexes/$c" && sudo find . ! -type d ! -path './System/Library/Caches/com.apple.dyld/*' ! -path './System/Library/dyld/*') |
	while IFS= read -r f; do
		f=${f#./}
		if ! sudo test -e "$work/root/$f" && ! sudo test -L "$work/root/$f"; then
			sudo mkdir -p "$work/root/$(dirname "$f")"
			src="$work/root/private/preboot/Cryptexes/$c/$f"
			# Linux hfsplus cannot hard-link symlinks: copy those.
			if sudo test -L "$src"; then sudo cp -P "$src" "$work/root/$f"; else sudo ln "$src" "$work/root/$f"; fi
			echo "linked $c:/$f"
		fi
	done
done

# logd's log stores, normally made at restore: it checks owner _logd (272)
# and mode, plus a .migrated marker, else asks a helper and crashes
# ("BUG IN LIBTRACE: Failed to migrate directory").
# (mkdir/chown/chmod, not install -o: uutils install rejects numeric owners
# that have no passwd entry, as on Ubuntu 26.04)
db="$work/root/private/var/db"
sudo mkdir -p "$db/diagnostics/timesync" "$db/uuidtext"
sudo chown 272:272 "$db/diagnostics" "$db/uuidtext" "$db/diagnostics/timesync"
sudo chmod 0751 "$db/diagnostics"
sudo chmod 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for d in diagnostics uuidtext diagnostics/timesync; do
	sudo touch "$db/$d/.migrated"
	sudo chown 272:272 "$db/$d/.migrated"
	sudo chmod 0644 "$db/$d/.migrated"
done

# System keybag for the built-in AppleKeyStore (no passcode).
sudo mkdir -p "$work/root/private/var/keybags"
sudo chown 0:0 "$work/root/private/var/keybags"
sudo chmod 0700 "$work/root/private/var/keybags"
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	'<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' |
	sudo tee "$work/root/private/var/keybags/systembag.kb" >/dev/null
sudo chown 0:0 "$work/root/private/var/keybags/systembag.kb"
sudo chmod 0600 "$work/root/private/var/keybags/systembag.kb"

df -h "$work/root"
sudo umount "$work/root"
# Linux hfsplus decomposes some non-ASCII names (localized Shortcuts examples)
# differently from Apple's tables; let fsck_hfs rewrite them, then verify.
fsck.hfsplus -f -y "$out" | grep -v 'Illegal name' || true
fsck.hfsplus -f -n "$out" || echo "note: fsck -n header warning after the Linux mount (xnu replays the journal)"
