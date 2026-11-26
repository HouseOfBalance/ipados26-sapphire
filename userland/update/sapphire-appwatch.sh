#!/var/jb/bin/bash
# sapphire-appwatch: run by launchd (com.sapphire.appwatch, WatchPaths /Applications and /var/jb/Applications)
# when those folders change. uicache only when the list of app bundles really changed: uicache itself touches
# the folders, and running it on every trigger looped every ThrottleInterval (SpringBoard reloading its apps
# each time, which closed the SIM PIN prompt).
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin:/usr/sbin:/sbin
STATE=/var/root/.sapphire-appwatch.state
sleep 3
killall -0 SpringBoard 2>/dev/null || exit 0
list=$(ls -1d /Applications/*.app /var/jb/Applications/*.app 2>/dev/null || true)
now=$(printf '%s\n' "$list" | shasum -a 256 2>/dev/null | cut -d' ' -f1)
[ -n "$now" ] || now=$(printf '%s\n' "$list" | sha256sum 2>/dev/null | cut -d' ' -f1)
[ -n "$now" ] || now=$(printf '%s\n' "$list" | md5sum | cut -d' ' -f1)
if [ ! -f "$STATE" ] && [ ! -L "$STATE" ]; then
	printf '%s\n' "$now" > "$STATE"        # the first look after a boot: what is there now is registered already
	exit 0
fi
[ -L "$STATE" ] && rm -f -- "$STATE"
[ -f "$STATE" ] && [ "$(cat "$STATE")" = "$now" ] && exit 0
printf '%s\n' "$now" > "$STATE"
echo "$(date '+%F %T') apps changed: uicache"
uicache -a
