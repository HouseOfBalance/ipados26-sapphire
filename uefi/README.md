# Mu-Silicium UEFI for `ipados26-sapphire` (Redmi Note 13 4G)

Port of [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) to Xiaomi Redmi Note 13 4G (`sapphire` / `sapphiren`, SM6225Pro) chaining directly into our XNU 26.1 shim (`shim/`):

`ABL fastboot boot -> BootShim -> UEFI DXE -> SapphireXnuBoot.c -> shim/ + XNU 26.1 payload`

## Build
```sh
git clone https://github.com/Project-Silicium/Mu-Silicium && cd Mu-Silicium
git checkout $(cat ../uefi/BASE) && git submodule update --init --recursive
git apply ../uefi/mu-silicium.patch && cp -r ../uefi/files/* .
cp -r Silicium-ACPI/Platforms/Xiaomi/tapas Silicium-ACPI/Platforms/Xiaomi/sapphire
cp /path/to/sapphire-live.dtb Resources/DTBs/sapphire.dtb
python3 build_uefi.py -d sapphire -r RELEASE
../tools/ipados26-boot.sh --gpu --gpu-fw ~/work/gpu-fw ../xnu/out-ipados26/kernel.development.sweet out.img
python3 ../uefi/mkuefi-v4.py Mu-sapphire.img ipados26-uefi.img ../shim/out/Image Resources/DTBs/sapphire.dtb
fastboot boot ipados26-uefi.img
```
