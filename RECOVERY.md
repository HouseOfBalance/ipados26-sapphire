# Recovering Boot & OrangeFox on Redmi Note 13 4G (`sapphire`)

Xiaomi Redmi Note 13 4G (`sapphire` / `sapphiren`, SM6225Pro) uses A/B slots with an unlocked bootloader. Nothing below touches `iosroot` or `userdata`.

## Key Bootloader Behavior
- In recovery mode, Xiaomi's ABL boots the kernel stored in the active slot's `boot` partition (`boot_a`).
- Therefore, **never run `fastboot flash boot` with an XNU image** — always boot iPadOS 26.1 from RAM:
  ```sh
  fastboot boot ipados26-boot-gpu.img
  ```

## Restoring Recovery / Android Slot `a`
```sh
fastboot getvar current-slot
fastboot set_active a
fastboot flash boot_a ROM-boot.img
fastboot reboot recovery
```
To exit iPadOS 26.1 cleanly so the HFS+ journal is flushed, use **Settings -> General -> Shut Down** (or **Android.app**), then hold **Power + Volume Down** to enter Fastboot.
