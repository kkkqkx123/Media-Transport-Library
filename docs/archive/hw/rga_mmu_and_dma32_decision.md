# RGA MMU 位宽与 dma32 分配决策

## 背景

RK3588 平台同时包含 RGA3 与 RGA2 两类 RGA 核心。`rk3588_live_tx` 的
`rga_converter` 通过 librga 自动调度（`core = 0`）将采集帧转换为 NV12，并把
NV12 池缓冲区同时提供给 MPP 编码器。

## 开发板实测结论

- RGA3（`fdb60000` / `fdb70000`）：`mmu: RK_IOMMU`，使用系统 IOMMU，可处理
  `system` dma-heap 分配的缓冲区（含 4GB 以上高位内存），属于 64 位访问能力。
- RGA2（`fdb80000`）：`mmu: RGA_MMU`，自带 MMU，仅支持 4GB 以内内存，属于 32 位
  访问能力。
- 当前 8GB 板子内存布局为 DMA zone（约 0-4GB）+ Normal zone（约 4GB 以上），
  存在高位内存，不是“只有低 4GB 内存”的布局。

## 决策

1. 不限制 RGA 只使用 RGA3。`core` 字段仅用于开发调试，生产环境不应设置；自动
   调度在需要 RGA2 特有功能/格式时会使用 RGA2。
2. 保留 `dma32_allocator`，保证 NV12 池缓冲区落在 4GB 以内，兼容 RGA2 与自动调度。
3. 之前的 “4G + 256MB 高位内存” 板子只是 RGA2 32 位限制的一个具体表现；当前板子
   仍存在高位内存，因此 dma32 仍然必要。

## 参考

- Rockchip librga FAQ：RGA2 不支持大于 4G 内存空间；多核平台应分配 4G 以内内存。
- Rockchip librga 开发指南：`core` 为高权限字段，仅建议开发调试使用。
