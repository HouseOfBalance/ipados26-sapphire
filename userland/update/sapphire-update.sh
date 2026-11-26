#!/var/jb/bin/bash
# sapphire-update: applies Sapphire updates left in the inbox (by the Windows/Linux installer's update mode;
# later, downloaded from GitHub). Run at every boot by sapphire-firstboot.sh, before anything else of ours.
#
# An update is an inbox entry named *.supdate: a .tar.gz with update.sh at its top (and whatever files it
# brings). It is unpacked to /var/tmp/sapphire-update and run as `bash update.sh DIR` as root. Its sha256
# (from the inbox header, checked against the bytes) goes into /var/root/.sapphire-updates once update.sh
# exits 0, so each update runs once; a failed one is tried again at the next boot (at most 3 times).
export PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/usr/bin:/bin:/usr/sbin:/sbin
CAT=/redwood/bin/sapphire-inboxcat
DONE=/var/root/.sapphire-updates
TRIES=/var/root/.sapphire-update-tries
LOG=/var/root/sapphire-update.log
PUBKEY=/redwood/share/sapphire-update.pub
ALLOW_UNSIGNED=/var/root/.sapphire-allow-unsigned-updates
MAX_ENTRIES=15

log() { echo "$(date '+%F %T') $*" >> "$LOG"; echo "sapphire-update: $*" > /dev/console 2>/dev/null; }

hdr=""
W=""
cleanup() {
	[ -n "$W" ] && rm -rf -- "$W"
	[ -n "$hdr" ] && rm -f -- "$hdr"
}
trap cleanup EXIT

verify_sig() {
	local data="$1" sig="$2"
	[ -s "$PUBKEY" ] && [ -s "$sig" ] || return 1
	openssl pkeyutl -verify -pubin -inkey "$PUBKEY" -rawin -in "$data" -sigfile "$sig" >/dev/null 2>&1 || \
		openssl dgst -sha256 -verify "$PUBKEY" -signature "$sig" "$data" >/dev/null 2>&1
}

find_companion_sig() {
	local want="$1.sig" dst="$2" j ej nj offj lenj
	for (( j = 0; j < count; j++ )); do
		ej=$(( 24 + 256 * j ))
		nj=$(dd if="$hdr" bs=1 skip=$(( ej + 56 )) count=192 2>/dev/null | tr -d '\0')
		[ "$nj" = "$want" ] || continue
		offj=$(le "$ej" 8)
		lenj=$(le $(( ej + 8 )) 8)
		case "$offj:$lenj" in *[!0-9:]*) continue ;; esac
		if [ "$offj" -ge 4096 ] && [ "$lenj" -gt 0 ] && [ "$lenj" -le 4096 ]; then
			$CAT $(( S + offj )) "$lenj" > "$dst" 2>/dev/null && return 0
		fi
	done
	return 1
}

[ -x "$CAT" ] || exit 0
S=$($CAT start) || exit 0
case "$S" in ''|*[!0-9]*) exit 0 ;; esac

hdr=$(mktemp /var/tmp/sapphire-update-hdr.XXXXXX) || exit 0
$CAT "$S" 4096 > "$hdr" 2>/dev/null || exit 0
[ "$(head -c 8 "$hdr")" = SPHIPAQ1 ] || exit 0
le() { od -An -t u"$2" -j "$1" -N "$2" "$hdr" | tr -d ' \n'; }   # OFF SIZE (4 or 8), little-endian
count=$(le 12 4)
case "$count" in ''|*[!0-9]*) exit 0 ;; esac
if [ "$count" -gt "$MAX_ENTRIES" ]; then
	log "clamping inbox entry count $count to $MAX_ENTRIES"
	count=$MAX_ENTRIES
fi
touch "$DONE" "$TRIES"

for (( i = 0; i < count; i++ )); do
	e=$(( 24 + 256 * i ))
	name=$(dd if="$hdr" bs=1 skip=$(( e + 56 )) count=192 2>/dev/null | tr -d '\0')
	case "$name" in *.supdate) ;; *) continue ;; esac
	off=$(le "$e" 8)
	len=$(le $(( e + 8 )) 8)
	case "$off" in ''|*[!0-9]*) continue ;; esac
	case "$len" in ''|*[!0-9]*) continue ;; esac
	if [ "$off" -lt 4096 ] || [ "$len" -eq 0 ]; then
		log "update $name: invalid inbox extent (off=$off len=$len)"
		continue
	fi
	sha=$(od -An -tx1 -j $(( e + 16 )) -N 32 "$hdr" | tr -d ' \n')
	grep -q "^$sha" "$DONE" && continue
	tries=$(grep -c "^$sha" "$TRIES" || true)
	if [ "$tries" -ge 3 ]; then
		continue
	fi
	echo "$sha $(date +%s)" >> "$TRIES"
	log "update $name ($len bytes, $sha): reading"
	W=$(mktemp -d /var/tmp/sapphire-update.XXXXXX) || { log "mktemp -d failed"; break; }
	ARCHIVE="$W/pkg.tgz"
	EXTRACT="$W/unpacked"
	mkdir -p "$EXTRACT"
	if ! $CAT $(( S + off )) "$len" > "$ARCHIVE"; then
		log "update $name: the inbox could not be read"
		rm -rf -- "$W"; W=""
		continue
	fi
	got=$(shasum -a 256 "$ARCHIVE" 2>/dev/null | cut -d' ' -f1)
	[ -n "$got" ] || got=$(sha256sum "$ARCHIVE" | cut -d' ' -f1)
	if [ "$got" != "$sha" ]; then
		log "update $name: sha256 mismatch ($got)"
		rm -rf -- "$W"; W=""
		continue
	fi
	if ! tar -xzpf "$ARCHIVE" -C "$EXTRACT" 2>>"$LOG" || [ ! -f "$EXTRACT/update.sh" ] || [ -L "$EXTRACT/update.sh" ]; then
		log "update $name: not an update (no regular update.sh)"
		rm -rf -- "$W"; W=""
		continue
	fi
	verified=0
	if find_companion_sig "$name" "$W/archive.sig" && verify_sig "$ARCHIVE" "$W/archive.sig"; then
		verified=1
	elif [ -f "$EXTRACT/update.sh.sig" ] && verify_sig "$EXTRACT/update.sh" "$EXTRACT/update.sh.sig"; then
		verified=1
	elif [ -f "$ALLOW_UNSIGNED" ] && [ ! -L "$ALLOW_UNSIGNED" ]; then
		log "update $name: WARNING allowing unsigned update via $ALLOW_UNSIGNED"
		verified=1
	fi
	if [ "$verified" -ne 1 ]; then
		log "update $name: refused (missing or invalid cryptographic signature against $PUBKEY)"
		rm -rf -- "$W"; W=""
		continue
	fi
	if bash "$EXTRACT/update.sh" "$EXTRACT" >> "$LOG" 2>&1; then
		echo "$sha $name $(date +%s)" >> "$DONE"
		log "update $name: applied"
	else
		log "update $name: update.sh failed ($?)"
	fi
	rm -rf -- "$W"; W=""
done
exit 0
