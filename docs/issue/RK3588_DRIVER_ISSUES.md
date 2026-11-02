# RK3588 Driver / Hardware Issues

## 1. Network: YT6801 NIC driver lacks XDP support

- Affected interfaces: `enP2p33s0`, `enP3p49s0`
- Driver: `yt6801` (PCI 1F0A:6801)
- Evidence:
  - `/sys/class/net/enP2p33s0/device/driver -> ../../../../../../bus/pci/drivers/yt6801`
  - `mt_dev_xdp_init`: `Underlying driver does not support XDP in native mode`
  - Driver source `drivers/net/ethernet/yt6801` has no `ndo_bpf` / `ndo_xdp_xmit` implementation
- Impact:
  - MTL `native_af_xdp` backend cannot initialize.
  - MtlManager XDP attach fails even with XDP enabled build.
- Classification: driver capability gap, not kernel configuration issue.
- Workaround: use `kernel:` backend for MTL verification.
- Future check with new SDK:
  - Search driver source for `ndo_bpf`, `ndo_xdp_xmit`, `XDP_SETUP_PROG`.
  - Run `ip link set dev <iface> xdp obj <prog> sec xdp` or MTL AF_XDP probe.

## 2. Network: second PCIe NIC has no carrier

- Affected interface: `enP3p49s0`
- State: DOWN / no carrier
- Impact: dual-NIC and dual-host network tests are limited.
- Future check: verify link LED/cable/SFP or PHY initialization in new SDK.

## 3. DRM: no physical display connected

- Connected connector: only `Writeback-1`
- Disconnected connectors: `HDMI-A-1`, `HDMI-A-2`, `DP-1`
- Evidence: component tests report `drm: connector HDMI-A-1 not found`
- Impact:
  - DRM render-to-HDMI path cannot be visually verified.
  - `test_codec`, `test_io`, `test_pipeline` DRM render path fail.
- Workaround: adapt tests to use `Writeback-1` or connect HDMI display.
- Future check: verify connector enumeration and HDMI PHY in new SDK.

## 4. V4L2 / HDMI RX input works after HDMI source connected

- Device: `/dev/video0` = `rk_hdmirx`
- Format: 1920x1080@60 BGR3
- Status: capture and encode path verified.
- Future check: confirm hotplug and format negotiation stability.

## 5. MPP / RGA encode path works

- V4L2 capture -> RGA conversion -> MPP encode initialized successfully.
- Status: verified via `test_pipeline`.
- Future check: long-run encode stability and memory usage.

## 6. ALSA

- ALSA capture device: `hw:3,0` = `rockchip,hdmiin`
- Verified: `ffmpeg -f alsa -i hw:3,0` captured 4s non-silent audio
  (mean_volume -20.8 dB, max_volume -2.3 dB).
- `arecord` default U8 format is unsupported; use S16_LE/S24_LE/S32_LE.
- HDMI RX audio source runs at 44.1 kHz in current setup.
- Status: capture path verified; playback/display AV sync still needs display.
- Future check: playback to HDMI TX and AV sync.

## 7. PTP

- `linuxptp` 3.1 installed.
- Hardware timestamping support on YT6801 ports must be verified with new SDK/driver.
- Future check: run `ptp4l -H` / `-S` and inspect PHC availability.

## Summary for new SDK comparison

When moving to a new SDK, re-check:

1. YT6801 XDP capability (`ndo_bpf` / `ndo_xdp_xmit`).
2. Second NIC carrier and PHY status.
3. DRM connector enumeration (HDMI-A-1/2, DP-1).
4. V4L2 HDMI RX format stability.
5. MPP/RGA encode stability.
6. ALSA playback/capture.
7. PTP hardware timestamping.
