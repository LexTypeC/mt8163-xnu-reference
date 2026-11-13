# MT8163 XNU reference

MT8163 board support for iOS 6.1.3 on the Kindle Fire HD (2018), board `karnak`.
ARM64 XNU 8796.121.2 with native AArch32 userspace. Requires the surrounding XNU port.

| Files | Purpose |
| --- | --- |
| `storage/fire_msdc_read.h`, `fire_emmc.c` | MSDC0 PIO and XNU block devices |
| `input/fire_touch.c`, `sys/hakios.input.h` | MTK I²C/AP-DMA, Goodix polling, touch queue ABI |
| `display/fire_present*` | OVL presentation, buffer ownership and completion |
| `display/fire_display_probe.h`, `fire_*gic.inc`, `fire_scanout*` | Register guards, IRQ routing and RDMA observation |
| `geometry/` | Viewport, touch mapping and virtual Home button; source hooks in `native_home_policy.py` |
| `support/` | Measurement declarations |

## Board assumptions

- LK has initialized eMMC and the panel/DSI path. No card enumeration or panel-init code.
- MSDC0: `0x11230000`. PIO timeouts assume 13 MHz. Storage LBAs and journal sector are Fire-specific.
- Touch: I²C2 `0x11009000`, AP-DMA `0x11000280`, Goodix `0x5d`, reset GPIO45, INT GPIO22.
  DMA requires physical addresses and cache maintenance. Apply `native_home_policy.touch()` for viewport mapping.
- Display route, IRQs, buffer limits and original front `0x98800000` are karnak-specific.
  Completion requires a later qualifying OVL IRQ, not just address readback.
- Geometry: 640×960 render → 800×1200 on an 800×1280 panel, with an 80 px Home bar.
  CPU scaling; Mesa/Panfrost code is separate.

## Integration dependencies

`.inc` files require inclusion in XNU translation units.
`fire_scanout.inc` requires these definitions from `bsd/dev/fire_usb.c`:

- `fu_r32`: volatile MMIO load.
- `fu_scanout_write`: volatile MMIO store followed by `dsb sy`.
- `fu_scanout_frequency`: reads `cntfrq_el0`.

GIC files require the port's interrupt-controller implementation.

Other dependencies: kernel mapping, allocation, locking, loader logging,
profiling callbacks and shutdown hooks.

Renderer interface: `fire_present.h`. Join writers before prepare/publish;
preserve GPU/IOSurface completion and CPU read leases.
Optional diagnostic code is included.

[Upstream sources and licenses](UPSTREAM.md).
