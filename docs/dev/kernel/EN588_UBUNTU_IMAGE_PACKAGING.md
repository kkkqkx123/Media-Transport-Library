# EN588：将预编译 Ubuntu 镜像与当前内核打包

## 背景

EN588 SDK 只支持 `buildroot / debian / yocto` 作为 `RK_ROOTFS_SYSTEM`。
Ubuntu 以预编译整机镜像（`GB-RK3588-ubuntu22.04-*.img`）形式提供，
不参与 SDK 的内核/u-boot/rootfs 构建链路。

因此，若需要“当前修改后的内核 + Ubuntu rootfs”，应使用 Rockchip 打包工具
将预编译 Ubuntu 镜像解包，替换其中的启动相关部件，再重新打包。

## 输入

- 当前 EN588 构建产物：
  - `output/firmware/boot.img`
  - `output/firmware/uboot.img`
  - `output/firmware/MiniLoaderAll.bin`
  - `output/firmware/parameter.txt`
- 预编译 Ubuntu 镜像：
  - `GB-RK3588-ubuntu22.04-20261110-100123-gnome-opensource-EN.img`
- Rockchip 打包工具：
  - `tools/linux/Linux_Pack_Firmware/rockdev/rkImageMaker`
  - `tools/linux/Linux_Pack_Firmware/rockdev/afptool`
  - `tools/linux/Linux_Pack_Firmware/rockdev/rk3588-package-file`

## 流程

1. 解包 Ubuntu 镜像，得到 `rootfs.img`、`misc.img`、`recovery.img`、
   `oem.img`、`userdata.img` 等分区镜像。
2. 新建打包目录，放入 `package-file`。
3. 将 Ubuntu 镜像中的 rootfs 及辅助分区镜像作为 rootfs/oem/userdata 等来源。
4. 将当前 SDK 构建产物中的 `boot.img`、`uboot.img`、`MiniLoaderAll.bin`、
   `parameter.txt` 覆盖到打包目录。
5. 使用 `afptool -pack` 生成 firmware 包，再用 `rkImageMaker -RK3588`
   生成最终 `update.img`。

## 注意事项

- Ubuntu 镜像体积较大，解包与打包过程耗时较长，应在 devs 上使用 `setsid`
  与日志文件执行。
- `package-file` 中的路径必须与打包源目录结构一致；若源目录为 `Image/`，
  条目应写为相对文件名，而不是 `Image/...`。
- Ubuntu rootfs 的内核模块来自其预编译内核；当前内核若仅在上游 defconfig
  基础上增量开启能力，通常可继续使用 Ubuntu rootfs 中的模块。
- 打包结果只替换启动链路与内核启动镜像，不重新编译 Ubuntu rootfs。

## 参考结果

- 产物：`/data/kkkqkx/pack-ubuntu/update.img`
- 版本来源：`-g2ab7b8c11`（内核）+ Ubuntu 22.04 rootfs
- 打包状态：`afptool` 与 `rkImageMaker` 均返回成功
