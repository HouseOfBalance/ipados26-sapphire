#!/bin/bash
# install-app.sh IPA [PHONE]: install Huella.ipa onto iPadOS 26.1 over USB SSH using /redwood/bin/sapphire-ipa-helper.
set -e
ipa=$1
P=${2:-root@192.168.7.2}
O="-o ConnectTimeout=6 -i $HOME/.ssh/sapphire_phone"
[ -f "$ipa" ] || { echo "usage: install-app.sh IPA [PHONE]"; exit 2; }
scp -q $O "$ipa" $P:/var/mobile/Documents/Huella.ipa
ssh $O $P /var/jb/bin/bash -s <<'EOF'
export PATH=/redwood/bin:/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/usr/bin:/bin:/usr/sbin
chown 501:501 /var/mobile/Documents/Huella.ipa
if [ -x /redwood/bin/sapphire-ipa-helper ]; then
	if /redwood/bin/sapphire-ipa-helper install /var/mobile/Documents/Huella.ipa force; then
		echo "Huella installed via sapphire-ipa-helper"
		exit 0
	fi
fi
t=$(mktemp -d)
cd "$t" && unzip -q /var/mobile/Documents/Huella.ipa
rm -rf /Applications/Huella.app
mv "$t/Payload/Huella.app" /Applications/Huella.app
chown -R root:wheel /Applications/Huella.app; chmod 755 /Applications/Huella.app/Huella
uicache -p /Applications/Huella.app && echo "Huella installed in /Applications (uicache)"
rm -rf "$t"
EOF
