#!/bin/bash
# Build the Sapphire XNU 26.1 (xnu-12377) boot image for iPadOS 26.1 (iPad11,6 / J171aAP):
#
#   ./tools/ipados26-boot.sh [--readonly] [--console] [--amp] [--gpu] [--metal-stub] [--cellular] [--ipad-canvas]
#       [--autoreboot SECS] [--ufsroot PART] [--gpu-fw DIR] [--ipa-fw DIR] KERNEL [OUT.img] [extra boot-args]
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)

rw="ufsrw=1 rootrw=1"
con="sweetfb_noconsole=1"
ufsroot="iosroot"
gpu_mode="cpu"
gpu_delay=""
autoreboot_args=""
gpufw=()
adt_opts=()
cpus=4
while [[ ${1:-} == --* ]]; do
	case $1 in
	--readonly) rw="" ;;
	--console) con="" ;;
	--amp) adt_opts+=(--amp); cpus=8 ;;
	--gpu) gpu_mode="gpu" ;;
	--metal-stub) gpu_mode="metal-stub" ;;
	--gpu-delay) shift; gpu_delay="$1" ;;
	--cellular) adt_opts+=(--cellular) ;;
	--ipad-canvas) adt_opts+=(--ipad-canvas) ;;
	--home-button) adt_opts+=(--home-button) ;;
	--autoreboot) shift; autoreboot_args="sweet_autoreboot=$1 sweet_reboot=0 ufslogsecs=1" ;;
	--ufsroot) shift; ufsroot="$1" ;;
	--ipa-fw)
		shift
		fwdir=$(realpath "$1")
		[[ -f $fwdir/ipa_fws.mdt && -f $fwdir/ipa_fws.elf ]] || { echo "$fwdir/ipa_fws.{mdt,elf} not found" >&2; exit 1; }
		gpufw+=(--ipa-mdt "$fwdir/ipa_fws.mdt" --ipa-elf "$fwdir/ipa_fws.elf")
		;;
	--gpu-fw)
		shift
		fwdir=$(realpath "$1")
		[[ -f $fwdir/a630_sqe.fw ]] || { echo "$fwdir/a630_sqe.fw not found" >&2; exit 1; }
		gpufw+=(--gpu-sqe "$fwdir/a630_sqe.fw")
		[[ -f $fwdir/a610_zap.mdt ]] && gpufw+=(--gpu-zap-mdt "$fwdir/a610_zap.mdt")
		[[ -f $fwdir/a610_zap.b02 ]] && gpufw+=(--gpu-zap-seg "$fwdir/a610_zap.b02")
		;;
	*) echo "unknown option $1" >&2; exit 2 ;;
	esac
	shift
done

if [[ -z $gpu_delay ]]; then
	if [[ -n $autoreboot_args ]]; then gpu_delay=2; else gpu_delay=5; fi
fi
case $gpu_mode in
gpu) gpu_args="gpu=3 gpu_delay=$gpu_delay gpu_mmu=1 gpu_service=1 sweetfb_gpucopy=1 sweetmetal=2 sweetgl=1 sweet_glload=1 gpu_div=1 sweetfb_direct=0 gpu_mhz=1260 glwait=0" ;;
metal-stub) gpu_args="gpu=3 gpu_delay=$gpu_delay gpu_mmu=1 gpu_service=1 sweetfb_gpucopy=1 sweetmetal=1 sweetgl=1 sweet_glload=1 gpu_div=1 sweetfb_direct=0 gpu_mhz=1260 glwait=0" ;;
*) gpu_args="sweetmetal=0 sweetgl=0 sweet_glload=0" ;;
esac

[[ $# -ge 1 ]] || { sed -n '2,5p' "$0" >&2; exit 2; }
kernel=$(realpath "$1")
if [[ -n ${2:-} ]]; then
	mkdir -p "$(dirname "$2")"
	out="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
else
	out="$repo/shim/out/ipados26-boot.img"
fi
extra=${3:-}
cd "$repo"

[[ -f research/ipsw-ipados26/j171aap.adt ]] || {
	echo "missing research/ipsw-ipados26/j171aap.adt (run tools/ipados26_im4p_aea.py first)" >&2
	exit 1
}

mkdir -p shim/out
python3 tools/ipados26_mkadt.py shim/out/ipados26-sapphire.adt --highmem \
	${gpufw[@]+"${gpufw[@]}"} ${adt_opts[@]+"${adt_opts[@]}"}

# Default: CPU rendering first (sweetmetal=0 sweetgl=0 sweet_glload=0) at native 1080x2400 (sweetfb_native=1);
# pass --gpu for Adreno 610 + Tweaks!
python3 tools/mkpayload.py "$kernel" shim/out/ipados26-sapphire.adt shim/out/payload-ipados26.bin \
	--cmdline "debug=0xc serial=3 cpus=$cpus keepsyms=1 -noprogress rd=ufsroot0 ufsroot=$ufsroot unentitled_ios_sim_launch=1 ignition_level=1 $rw launchd_unsecure_cache=1 amfi_get_out_of_my_way=1 cs_enforcement_disable=1 wdt=-1 ufslogsecs=1 $con sweettouch_rx=8640 sweettouch_ry=9600 sweettouchdelay=10 sweetusb=2 modem=0 wifi=1 sweetfb_native=1 sweetfb_hz=120 $gpu_args sweetbl=0 sweet_splash=0 $autoreboot_args $extra"

shim/build.sh ../shim/out/payload-ipados26.bin
[[ $out == "$repo/shim/out/shim-boot.img" ]] || cp shim/out/shim-boot.img "$out"
sha256sum "$out"
