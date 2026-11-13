# Upstream

- [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel), base commit
  `5ed3151d6ac7995a04bd2bc83d960fdeb3d7cd87`, XNU 8796.121.2.
  `input/sys/hakios.input.h` is an excerpt from its kernel header.
  Kernel additions: APSL 2.0; upstream notices and license in `licenses/`.
- Fire code comes from the local port. Original comments retained; no new blanket license assigned.

Register references from `mt8163/android_kernel_amazon_karnak`, commit
`65a221f465b34d967ca0ec115569ca71dd136a96`:

- [mt81xx-sd.c](https://github.com/mt8163/android_kernel_amazon_karnak/blob/65a221f465b34d967ca0ec115569ca71dd136a96/drivers/mmc/host/mt81xx-sd.c)
- [i2c-mt65xx.c](https://github.com/mt8163/android_kernel_amazon_karnak/blob/65a221f465b34d967ca0ec115569ca71dd136a96/drivers/i2c/busses/i2c-mt65xx.c)
- [gt9xx.c](https://github.com/mt8163/android_kernel_amazon_karnak/blob/65a221f465b34d967ca0ec115569ca71dd136a96/drivers/input/touchscreen/gt9xx.c)
- [clk-mt8163.c](https://github.com/mt8163/android_kernel_amazon_karnak/blob/65a221f465b34d967ca0ec115569ca71dd136a96/drivers/clk/mediatek/clk-mt8163.c)
- [ddp_reg.h](https://github.com/mt8163/android_kernel_amazon_karnak/blob/65a221f465b34d967ca0ec115569ca71dd136a96/drivers/misc/mediatek/video/mt8163/dispsys/ddp_reg.h)
- [ddp_irq.c](https://github.com/mt8163/android_kernel_amazon_karnak/blob/65a221f465b34d967ca0ec115569ca71dd136a96/drivers/misc/mediatek/video/mt8163/dispsys/ddp_irq.c)
- [ddp_ovl.c](https://github.com/mt8163/android_kernel_amazon_karnak/blob/65a221f465b34d967ca0ec115569ca71dd136a96/drivers/misc/mediatek/video/mt8163/dispsys/ddp_ovl.c)
- [ddp_rdma.c](https://github.com/mt8163/android_kernel_amazon_karnak/blob/65a221f465b34d967ca0ec115569ca71dd136a96/drivers/misc/mediatek/video/mt8163/dispsys/ddp_rdma.c)
- [irq-mtk-sysirq.c](https://github.com/mt8163/android_kernel_amazon_karnak/blob/65a221f465b34d967ca0ec115569ca71dd136a96/drivers/irqchip/irq-mtk-sysirq.c)
