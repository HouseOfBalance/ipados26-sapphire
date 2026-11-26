#!/var/jb/bin/bash
# Tester bug 10. The missing iOS half of the Android app's "IPAs" page.
#
# Today: Huella/dual-boot app (IpaInbox.java) writes *.ipa entries into the inbox after iOS's volume, and
# nothing on iOS ever reads them: userland/update/sapphire-update.sh only takes *.supdate
# ("case "$name" in *.supdate) ;; *) continue"). So the IPA installer "does nothing".
#
# This: run from /redwood/boot.d (or a launchd job) once SpringBoard is up; every *.ipa entry not yet
# handled is read through kern.sweet_inbox (sapphire-inboxcat), its sha256 checked, and installed with
# native /redwood/bin/sapphire-ipa-helper on iPadOS 26.1.
# The result goes to /var/root/.sapphire-ipas ("SHA256 installed|failed NAME EPOCH"): the inbox is
# read-only from iOS (0310), so the header's per-entry state (48: 0 pending, 1 installed, 2 failed) can
# not be written here. Android then shows the state from that file instead (it already reads the HFS
# volume: Hfs.java) -> IpaPage.fill() needs a small change: state = file's line for e.sha256, else PENDING.
#
# To check on the phone before shipping: sapphire-ipa-helper CLI
# ("install <path>"; some versions want "install <path> force" for unsigned/fakesigned IPAs), and that it
# registers the app with lsd (uicache) by itself.
export PATH=/redwood/bin:/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/usr/bin:/bin:/usr/sbin:/sbin
CAT=/redwood/bin/sapphire-inboxcat
STATE=/var/root/.sapphire-ipas
LOG=/var/root/sapphire-ipa.log
LOCK=/var/root/.sapphire-ipa.lock
MAX_ENTRIES=15

log() { echo "$(date '+%F %T') $*" >> "$LOG"; echo "sapphire-ipa: $*" > /dev/console 2>/dev/null; }

sha256_file() {
    local out
    out=$(shasum -a 256 "$1" 2>/dev/null | cut -d' ' -f1)
    [ -n "$out" ] || out=$(sha256sum "$1" 2>/dev/null | cut -d' ' -f1)
    echo "$out"
}

legacy_install_ipa() {
    local ipa_file="$1"
    local ipa_name="$2"
    local ext_dir="$W/extract"
    local py3="" depac="" ldid_bin="" staged_app="" app_base=""

    mkdir -p "$ext_dir" || return 1
    py3=$(command -v python3 2>/dev/null || true)
    [ -z "$py3" ] && [ -x /var/jb/usr/bin/python3 ] && py3=/var/jb/usr/bin/python3

    if command -v unzip >/dev/null 2>&1; then
        unzip -q "$ipa_file" -d "$ext_dir" >> "$LOG" 2>&1 || return 1
    elif [ -n "$py3" ]; then
        "$py3" -m zipfile -e "$ipa_file" "$ext_dir" >> "$LOG" 2>&1 || return 1
    else
        log "$ipa_name: fallback installer missing unzip and python3"
        return 1
    fi

    for cand in "$ext_dir"/Payload/*.app; do
        if [ -d "$cand" ] && [ ! -L "$cand" ]; then
            staged_app="$cand"
            break
        fi
    done
    [ -n "$staged_app" ] || { log "$ipa_name: Payload/*.app not found"; return 1; }

    app_base=$(basename "$staged_app")
    case "$app_base" in
        ""|.*|*/*|*".."*) log "$ipa_name: unsafe app bundle name ($app_base)"; return 1 ;;
        *.app) ;;
        *) log "$ipa_name: invalid app bundle name ($app_base)"; return 1 ;;
    esac

    # Refuse bundles containing symlinks pointing outside the extracted tree (absolute or ..)
    local bad_link=0
    while IFS= read -r -d '' lnk; do
        local tgt
        tgt=$(readlink "$lnk" 2>/dev/null || true)
        case "$tgt" in
            /*|*".."*) bad_link=1; break ;;
        esac
    done < <(find "$staged_app" -type l -print0 2>/dev/null)
    [ "$bad_link" -eq 0 ] || { log "$ipa_name: refusing unsafe symlink in bundle"; return 1; }

    # De-PAC arm64e -> ARMv8.0 BEFORE ad-hoc signing with ldid -S
    if [ -f /redwood/bin/ipados26_depac.py ]; then
        depac=/redwood/bin/ipados26_depac.py
    elif [ -f /var/jb/usr/bin/ipados26_depac.py ]; then
        depac=/var/jb/usr/bin/ipados26_depac.py
    fi
    if [ -n "$py3" ] && [ -n "$depac" ]; then
        log "$ipa_name: running $depac --rewrite before ldid -S"
        "$py3" "$depac" --rewrite "$staged_app" >> "$LOG" 2>&1 || \
            log "$ipa_name: warning: ipados26_depac.py returned $?"
    fi

    ldid_bin=$(command -v ldid 2>/dev/null || true)
    [ -z "$ldid_bin" ] && [ -x /var/jb/usr/bin/ldid ] && ldid_bin=/var/jb/usr/bin/ldid
    if [ -n "$ldid_bin" ]; then
        while IFS= read -r -d '' binf; do
            local magic ent_xml="$W/ent.xml"
            magic=$(dd if="$binf" bs=1 count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')
            case "$magic" in
                cffaedfe|cefaedfe|cafebabe|bebafeca|cafebabf|bfbafeca)
                    chmod 0755 "$binf" 2>/dev/null || true
                    if "$ldid_bin" -e "$binf" > "$ent_xml" 2>/dev/null && grep -q "<plist" "$ent_xml" 2>/dev/null; then
                        "$ldid_bin" "-S$ent_xml" "$binf" >> "$LOG" 2>&1 || "$ldid_bin" -S "$binf" >> "$LOG" 2>&1 || return 1
                    else
                        "$ldid_bin" -S "$binf" >> "$LOG" 2>&1 || return 1
                    fi
                    rm -f -- "$ent_xml"
                    ;;
            esac
        done < <(find "$staged_app" -type f -print0 2>/dev/null)
    fi

    rm -rf -- "/Applications/$app_base"
    mv -- "$staged_app" "/Applications/$app_base" || return 1
    chown -R root:wheel "/Applications/$app_base" 2>/dev/null || true
    if [ -x /var/jb/usr/bin/uicache ]; then
        /var/jb/usr/bin/uicache -p "/Applications/$app_base" >> "$LOG" 2>&1 || true
    fi
    return 0
}

hdr=""
W=""
cleanup() {
    [ -n "$W" ] && rm -rf -- "$W"
    [ -n "$hdr" ] && rm -f -- "$hdr"
}
trap cleanup EXIT

# Prevent concurrent runs
exec 9>"$LOCK"
flock -n 9 || { log "Another instance running, exiting"; exit 0; }

HELPER="/redwood/bin/sapphire-ipa-helper"
[ -x "$CAT" ] || { log "Missing inboxcat"; exit 0; }
if [ -x "$HELPER" ]; then
    log "Using IPA helper: $HELPER"
else
    log "sapphire-ipa-helper missing; using legacy fallback installer"
fi

# Wait for SpringBoard and calm boot (at least 90s after boot) - capped against clock jumps
boot_time=$(sysctl -n kern.boottime 2>/dev/null | sed -n 's/.*sec = \([0-9]*\).*/\1/p')
if [ -z "$boot_time" ]; then
    boot_time=$(date +%s)
fi
now=$(date +%s)
wait_needed=$(( 90 - (now - boot_time) ))
[ "$wait_needed" -gt 90 ] && wait_needed=90
if [ "$wait_needed" -gt 0 ]; then
    log "Waiting ${wait_needed}s for calm boot (90s after boot)"
    sleep "$wait_needed"
fi

# Wait for SpringBoard with max 30s timeout (6 x 5s)
for (( i = 0; i < 6; i++ )); do
    killall -0 SpringBoard 2>/dev/null && break
    sleep 5
done

# Final check - if SpringBoard still not up, exit gracefully
killall -0 SpringBoard 2>/dev/null || { log "SpringBoard not running after wait, exiting"; exit 0; }

S=$($CAT start) || { log "Failed to start inbox read"; exit 0; }
case "$S" in ''|*[!0-9]*) log "Invalid inbox start offset"; exit 0 ;; esac

hdr=$(mktemp /var/tmp/sapphire-ipa-hdr.XXXXXX) || { log "mktemp failed"; exit 0; }
$CAT "$S" 4096 > "$hdr" 2>/dev/null || { log "Failed to read inbox header"; exit 0; }
[ "$(head -c 8 "$hdr")" = SPHIPAQ1 ] || { log "Invalid inbox header"; exit 0; }

le() { od -An -t u"$2" -j "$1" -N "$2" "$hdr" | tr -d ' \n'; }
count=$(le 12 4)
case "$count" in ''|*[!0-9]*) log "Invalid inbox entry count"; exit 0 ;; esac
if [ "$count" -gt "$MAX_ENTRIES" ]; then
    log "Clamping inbox entry count $count to $MAX_ENTRIES"
    count=$MAX_ENTRIES
fi
touch "$STATE"

installed_any=0
for (( i = 0; i < count; i++ )); do
    e=$(( 24 + 256 * i ))
    name=$(dd if="$hdr" bs=1 skip=$(( e + 56 )) count=192 2>/dev/null | tr -d '\0')
    case "$name" in *.ipa|*.IPA|*.tipa) ;; *) continue ;; esac
    off=$(le "$e" 8)
    len=$(le $(( e + 8 )) 8)
    case "$off" in ''|*[!0-9]*) continue ;; esac
    case "$len" in ''|*[!0-9]*) continue ;; esac
    if [ "$off" -lt 4096 ] || [ "$len" -eq 0 ]; then
        log "$name: invalid inbox extent (off=$off len=$len)"
        continue
    fi
    sha=$(od -An -tx1 -j $(( e + 16 )) -N 32 "$hdr" | tr -d ' \n')
    grep -q "^$sha " "$STATE" && continue

    W=$(mktemp -d /var/tmp/sapphire-ipa.XXXXXX) || { log "mktemp -d failed"; break; }
    f="$W/app.ipa"
    if ! $CAT $(( S + off )) "$len" > "$f"; then
        log "$name: the inbox could not be read"
        rm -rf -- "$W"; W=""
        continue
    fi
    got=$(sha256_file "$f")
    if [ "$got" != "$sha" ]; then
        echo "$sha failed $name $(date +%s)" >> "$STATE"
        log "$name: sha256 mismatch"
        rm -rf -- "$W"; W=""
        continue
    fi

    # Primary path: install via native /redwood/bin/sapphire-ipa-helper (includes native C De-PAC,
    # CS_CodeDirectory page rehashing, entitlement-preserving signing, and LaunchServices registration).
    # Legacy fallback (when sapphire-ipa-helper is absent): extract, ipados26_depac.py --rewrite, ldid -S.
    if [ -x "$HELPER" ]; then
        if "$HELPER" install "$f" >> "$LOG" 2>&1 || "$HELPER" install "$f" force >> "$LOG" 2>&1; then
            echo "$sha installed $name $(date +%s)" >> "$STATE"
            log "$name: installed"
            installed_any=1
        else
            rc=$?
            echo "$sha failed $name $(date +%s)" >> "$STATE"
            log "$name: sapphire-ipa-helper failed ($rc)"
        fi
    else
        if legacy_install_ipa "$f" "$name"; then
            echo "$sha installed $name $(date +%s)" >> "$STATE"
            log "$name: installed via legacy fallback"
            installed_any=1
        else
            rc=$?
            echo "$sha failed $name $(date +%s)" >> "$STATE"
            log "$name: legacy fallback install failed ($rc)"
        fi
    fi
    rm -rf -- "$W"; W=""
done

# Only run uicache if at least one IPA was newly installed, and never send SIGUSR1 to SpringBoard
# (SpringBoard does not handle SIGUSR1 and terminates on receipt).
if [ "$installed_any" -eq 1 ]; then
    /var/jb/usr/bin/uicache -a >> "$LOG" 2>&1 || true
fi

log "IPA install cycle complete (installed_any=$installed_any)"
exit 0
