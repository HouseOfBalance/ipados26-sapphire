#!/usr/bin/env python3
"""Generate an Apple Device Tree (.adt) for Xiaomi Redmi Note 13 4G (sapphire)
booting iPadOS 26.1 (iPad11,6 / J171aAP Wi-Fi or iPad11,7 / J172aAP Cellular).

Usage:
  python3 tools/ipados26_mkadt.py OUT.adt [--highmem] [--amp] [--cellular] [--ipad-canvas] [--gpu-sqe ...]
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt

# Sapphire (SM6225 "khaje") hardware layout — exact match with tools/mkadt.py
PHYS_BASE, MEM_SIZE = 0xB0000000, 0x4D000000
MID_PHYS_BASE = int(os.environ.get("MIDBASE", "0x80000000"), 16)
MID_MEM_SIZE = int(os.environ.get("MIDSIZE", "0x1c0000000"), 16)
HIGH_PHYS_BASE, HIGH_MEM_SIZE = 0x140000000, 0x100000000  # 4 GiB max (shim clamps automatically on 6 GB phones)
# /arm-io children are relative to SOC_BASE; must NOT be 0!
SOC_BASE = 0x01000000
GICD, GICD_SIZE = 0x0F200000, 0x10000
GICR, GICR_SIZE = 0x0F300000, 0x100000
UFS, UFS_SIZE = 0x04804000, 0x3000
FB, FB_W, FB_H, FB_STRIDE = 0x5C000000, 1080, 2400, 1080 * 4
FB_SIZE = 0x00F00000
TIMEBASE = 19200000
SWEET_ECID = 0x5357454554323631  # 'SWEET261'
VTIMER_INTID = 19

QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE = 0x08000000, 0x080A0000, 62500000
QEMU_VTIMER_INTID = 27

BOOT_MANIFEST_HASH = hashlib.sha384(b'sapphire ipados 26.1 boot manifest').digest()

IPAD_ADT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../research/ipsw-ipados26')
J171A_ADT = os.path.join(IPAD_ADT_DIR, 'j171aap.adt')
J172A_ADT = os.path.join(IPAD_ADT_DIR, 'j172aap.adt')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def load_ipad_product(path):
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if c.name == 'product':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')


def build(seed=None, qemu=False, highmem=False, midmem=False, ipad_canvas=False,
          gpu_fw=(), amp=False, cellular=False, home_button=False):
    phys_base, mem_size = ((MID_PHYS_BASE, MID_MEM_SIZE) if midmem else
                           (HIGH_PHYS_BASE, HIGH_MEM_SIZE) if highmem else (PHYS_BASE, MEM_SIZE))
    gicd, gicr, timebase = (QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE) if qemu else (GICD, GICR, TIMEBASE)
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 2000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    adt_src = J172A_ADT if cellular else J171A_ADT
    board_model = 'J172aAP' if cellular else 'J171aAP'
    target_type = 'J172a' if cellular else 'J171a'
    machine_id = 'iPad11,7' if cellular else 'iPad11,6'

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('firmware-version', cstr('sapphire-shim-26')),
        ('system-firmware-version', cstr('sapphire-shim-26')),
        ('boot-type', cstr('local')),
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        ('crypto-hash-method', cstr('sha2-384')),
        # iPad Retina is @2x (artwork-scale-factor = 2 in J171aAP /product)
        ('display-scale', u32(2)),
        ('display-rotation', u32(0)),
        ('display-corner-radius', u32(0)),
        # Apple A12 Bionic (t8020) chip-id expected by iPad11,6/11,7 iPadOS 26.1 userland
        ('unique-chip-id', struct.pack('<Q', SWEET_ECID)),
        ('chip-id', u32(0x8020)),
        # Dev-fused / Hactivation tuple (0,0,1,1) + OS 26 security environment
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('effective-production-status-sep', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
        ('system-trusted', u32(1)),
        ('protected-data-access', u32(1)),
        ('amfi-allows-trust-cache-load', u32(1)),
        ('unrestricted-subsystem-root', u32(1)),
        ('kernel-ctrr-to-be-enabled', u32(1)),
    ] + list(gpu_fw), [
        node('memory-map', [ph()]),
        node('boot-object-manifests', [ph()]),
        node('secure-boot-hashes', [ph()]),
        node('dynamic-object-map', [ph()]),
        node('carveout-memory-map', [ph()]),
        node('asmb', [ph(), ('lp-sip0', u64(0x6F)), ('lp-sip1', u32(1))]),
    ])

    cpu_nodes = []
    for core in range(1 if qemu else 8 if amp else 4):
        silver = core >= 4
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('qcom,kryo260') + cstr('qcom,kryo470') + cstr('ARM,v8')),
            ('reg', u32(core if qemu else (core & 3) if silver else 0x100 | core)),
            ('cpu-id', u32(core)),
            ('cluster-id', u32(1 if silver else 0)),
            ('cluster-type', u32(ord('E') if silver else ord('P')) if amp else u32(0)),
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core & 3)),
            ('l2-cache-id', u32(1 if silver else 0)),
            ('l3-cache-id', u32(0)),
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(1900800000 if silver else 2803200000)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1866000000)),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)), ('#size-cells', u32(0))], cpu_nodes)

    bb_nodes = []
    if cellular:
        bb_props = [
            ph(),
            ('device_type', cstr('baseband')),
            ('compatible', cstr('baseband,ice18')),
            ('device-imei', bytes(32)),
            ('region-sku', cstr('syscfg/RSKU/0x40,zeroes/0x40')),
        ]
        if os.path.exists(J172A_ADT):
            j172, _ = adt.parse(open(J172A_ADT, 'rb').read())
            src = next((c for c in j172.children if c.name == 'baseband'), None)
            have = {p[0] for p in bb_props}
            for p in (src.props if src else []):
                if p[0] not in have and p[0] not in ('AAPL,phandle', 'name', 'interrupt-parent', 'interrupts'):
                    bb_props.append((p[0], p[-1]))
        bb_nodes.append(node('baseband', bb_props))

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(QEMU_VTIMER_INTID if qemu else VTIMER_INTID)),
        ('reg', u64(gicd - SOC_BASE, GICD_SIZE, gicr - SOC_BASE, GICR_SIZE)),
    ])

    # Default 1080x2400 @2x = 540x1200 pt (1200x540 landscape for iPadOS 26 windowing, 1:1 panel scanout).
    # --ipad-canvas presents native 1620x2160 iPad 10.2" canvas (scaled down by SweetFramebuffer).
    cw, ch = (1620, 2160) if ipad_canvas else (1080, 2400)
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [
        gic,
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        node('wlan', [ph(), ('device_type', cstr('wlan')), ('compatible', cstr('topaz,wcn3988'))]),
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            ('canvas-width', u32(cw)),
            ('canvas-height', u32(ch)),
        ]),
    ])

    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(FB, FB_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
    ])

    ufs = node('sweet-ufs', [
        ph(),
        ('compatible', cstr('qcom,ufshc')),
        ('reg', u64(UFS, UFS_SIZE)),
    ])

    defaults = node('defaults', [
        ph(),
        ('no-effaceable-storage', u32(1)),
        ('kern.maxvnodes', u32(10000)),
        ('kern.maxkfsevents', u32(1024)),
    ])

    # home-button-type = 2 enables modern iPad Pro/Air gesture Home Bar & Dock on Sapphire's
    # buttonless touchscreen (pass --home-button for classic home-button-type = 0).
    buttons = node('buttons', [
        ph(),
        ('device_type', cstr('buttons')),
        ('compatible', cstr('buttons')),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(0 if home_button else 2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    osenv = node('osenvironments', [ph()], [
        node('embedded-darwinos-environment', [ph()]),
        node('recovery-environment', [ph()]),
    ])

    product_props = load_ipad_product(adt_src)
    if product_props:
        product_props = [(k, cstr('sRGB') if k == 'artwork-display-gamut' else v) for k, v in product_props]
    product = [node('product', [ph()] + product_props)] if product_props else []

    return node('device-tree', [
        ph(),
        ('compatible', cstr(board_model) + cstr(machine_id) + cstr('xiaomi,sapphire') +
                       cstr('xiaomi,sweet') + cstr('qcom,khaje') + cstr('AppleARM')),
        ('model', cstr(machine_id)),
        ('target-type', cstr(target_type)),
        ('target-sub-type', cstr(board_model)),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons, osenv] + bb_nodes + product + ([] if qemu else [ufs]))


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser(description='Generate Apple Device Tree (.adt) for Sapphire iPadOS 26.1')
    ap.add_argument('out', help='Output .adt path')
    ap.add_argument('--qemu', action='store_true')
    ap.add_argument('--highmem', action='store_true')
    ap.add_argument('--midmem', action='store_true')
    ap.add_argument('--ipad-canvas', action='store_true')
    ap.add_argument('--amp', action='store_true')
    ap.add_argument('--cellular', action='store_true')
    ap.add_argument('--home-button', action='store_true')
    ap.add_argument('--dump', action='store_true')
    ap.add_argument('--gpu-sqe')
    ap.add_argument('--gpu-zap-mdt')
    ap.add_argument('--gpu-zap-seg')
    ap.add_argument('--ipa-mdt')
    ap.add_argument('--ipa-elf')
    args = ap.parse_args()
    gpu_fw = []
    for val, prop in ((args.gpu_sqe, 'gpu-sqe-firmware'), (args.gpu_zap_mdt, 'gpu-zap-metadata'),
                      (args.gpu_zap_seg, 'gpu-zap-segment'), (args.ipa_mdt, 'ipa-fws-metadata'),
                      (args.ipa_elf, 'ipa-fws-image')):
        if val:
            blob = open(val, 'rb').read()
            gpu_fw.append((prop, blob))
            print(f'{prop}: {len(blob)} bytes', file=sys.stderr)
    root = build(
        qemu=args.qemu,
        highmem=args.highmem,
        midmem=args.midmem,
        ipad_canvas=args.ipad_canvas,
        gpu_fw=gpu_fw,
        amp=args.amp,
        cellular=args.cellular,
        home_button=args.home_button,
    )
    blob = adt.serialize(root)
    open(args.out, 'wb').write(blob)
    if args.dump:
        adt.dump(adt.parse(blob)[0])
    print(f'{args.out}: {len(blob)} bytes ({("iPad11,7" if args.cellular else "iPad11,6")} iPadOS 26.1)', file=sys.stderr)
