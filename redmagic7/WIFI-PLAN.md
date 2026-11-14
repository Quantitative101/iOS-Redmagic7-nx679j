# RedMagic 7 Wi-Fi port plan (QCA6490 / WCN6855 over PCIe)

Written 2026-11-12. Nothing here is built yet.

## Reference

HackiOS's Xiaomi Mi 10 port (`ref/hackios-hackios-xnu-kernel-umi`, blobless clone;
fetch files with `git checkout HEAD -- <path>`). Mi 10 = SM8250 + **QCA6390 over PCIe0 + MHI**,
the direct predecessor of our chip, also driven by Linux ath11k. Its README reports the Wi-Fi
interface initializing on the phone. Same XNU generation (xnu-8796.121.2; ours 8796.141.3).

Stack, bottom to top:

| Layer | Mi 10 file(s) | Reuse for RedMagic |
|---|---|---|
| PCIe root complex bring-up | `devices/xiaomi-mi-10/overlays/xnu/files/bsd/dev/umi_pcie.c` (363 lines) + `umi_pcie_phy.h` | **Adapt**: same register map, new GPIOs, GCC offsets, PHY table |
| MHI host (BHI firmware load, channels, MSI) | `umi_mhi.c` (787) | Mostly as-is |
| QRTR over MHI, WLFW QMI (cal, BDF, M3) | `umi_wifi.c` (884), `drivers/wifi/hakios.wlfw.c` (545), `drivers/modem/hakios.qmi.c` | Mostly as-is; new BDF name, chip id |
| ath11k (Linux 7.0.14, unmodified) on FreeBSD LinuxKPI | `tools/mk-ath11k.sh`, `wifi/lx/src/*`, overlay `overlays/wifi/lx/src/lx_bus.c` | As-is; `hw_rev` and firmware names to WCN6855 hw2.0 |
| 802.11 station, scan/join, 802.11<->Ethernet | `wifi/lx/src/lx_80211.c` | As-is |
| WPA2-PSK supplicant | `wifi/lx/src/lx_wpa.c` | As-is (see their review `docs/driver-review/wifi-stack.md` W1–W12) |
| Station thread / scan cache | `drivers/wifi/hakios.sta.c` | As-is |
| `en0` for iOS | `drivers/net/hakios.net.c` | As-is |
| Apple80211 answers for `wifid` (Settings, status bar) | `drivers/wifi/hakios.apple80211.c` | As-is (same code the Note 8 runs as sweet_apple80211) |

The Note 8 / S7 Wi-Fi (`ref/a5xelte-herolte/repo/xnu/patches/0304-0315`) is Broadcom fullMAC; only its
top layers (sta/net/apple80211, from the same HackiOS code) overlap. Its notes on wifid behaviour are useful.

## Hardware facts (live.dts + Android dmesg)

PCIe0 (`pcie@1c00000`, `qcom,pci-msm`), **identical register map to the Mi 10**:

- PARF `0x1c00000`, PHY `0x1c06000`, DBI `0x60000000`, ELBI `0x60000f20`, iATU `0x60001000`, CONF `0x60100000`
- PERST **gpio94**, CLKREQ **gpio95** (function pcie0_clkreqn), WAKE **gpio96** (Mi 10: 79 / 80 / 81)
- SMMU SID base `0x1c00` (iommu-map 0x1c00, 0x1c01), MSI via `pcie0_msi@0x17110040`, ep-latency 10 ms
- `qcom,phy-sequence` in the node (taro PHY: different from Kona's table)
- Rails: gdsc `gcc_pcie_0_gdsc`, 1p8 `pm8350_l6`, 0p9 `pm8350_l5`, cx/mx (ARC)

Radio (`qcom,cnss-qca6490@b0000000`), PCI id **17cb:1103** (`mhi_1103`):

- WLAN_EN **gpio80**, BT_EN gpio81, SW_CTRL gpio82, XO clock gpio204
- Rails: aon `pmr735a_s2` (1.012 V), dig `pm8350_s11` (0.966 V), io `pm8350_s10` (1.8 V),
  rfa1 `pm8350c_s1` (1.9–2.1 V), rfa2 `pm8350_s12` (1.35 V), antenna switch `pmr735a_l7` (2.8 V)
- SMMU IOVA pool `0xa0000000` + 256 MiB
- Target: chip_id 0x12, family 0xb, board_id 0xff, soc_id 0x400c1211, firmware WLAN.HSP.2.0-01667 (hw2.0)
- Firmware in `/vendor/firmware_mnt/image`: `amss20.bin`, `m3.bin`, `regdb.bin`, `bdwlan.*`, `qca6490/`;
  Android loads BDF **`bdwlang.elf`** (59844 bytes) and regdb (24278). Android also runs cold-boot calibration
  (6.8 s) first; ath11k on WCN6855 can skip it (`cold_boot_calib` off).

## Phases

Each phase is one or two test boots; the log ring is the oracle. Boot arg per phase, like `umi_wifi_step`.

1. **Import & build** (no device). Copy the HackiOS bsd/dev + bsd/sys + wifi/lx pieces into
   `ref/redmagic/xnu-rm` under `rmwifi` names; build the ath11k object with mk-ath11k.sh
   (Linux 7.0.14 + FreeBSD 15.1 LinuxKPI downloads); get it linking in our kernel with every path
   behind `rmwifi=0`. Expect header/KPI drift between xnu-8796.121 and .141.
2. **Power + PCIe link** (`rmwifi=1`). RPMh votes for the six radio rails (our `mondrian_rpmh_rail`
   already does LDO/SMPS votes) + XO; WLAN_EN high; PCIe GDSC/clocks (taro GCC offsets, from the
   `clock-names` + SM8450 GCC map); PHY sequence from live.dts; PERST release; link up; read config
   space: expect vendor 0x17cb device 0x1103. Read-only to the endpoint, no bus master.
3. **SMMU + MHI** (`rmwifi=2`). Map the IOVA pool in the PCIe SMMU stream 0x1c00/0x1c01 (Mi 10 has the
   SID table code; SM8450 uses the APPS SMMU, check whether bypass is allowed); MSI; MHI BHI loads
   `amss20.bin`; reach MHI mission mode; QRTR up.
4. **WLFW** (`rmwifi=3`). QMI WLFW: capabilities (expect chip 0x12, fw HSP.2.0), BDF download
   (`bdwlang.elf` + `regdb.bin`), M3 (`m3.bin`), mode ON. Firmware ready indication.
5. **ath11k + scan** (`rmwifi=4`). ath11k core with `hw_rev` WCN6855 hw2.0 on the lx bus; WMI ready;
   a scan logs nearby APs (SSIDs hidden in the log).
6. **Join + en0** (`rmwifi=5`). Network from a local file (the Note 8 drop pattern / HackiOS
   `fw/network`), WPA2 handshake, `en0` up, DHCP, ping.
7. **wifid / Settings**. Apple80211 path: scan list in Settings, join from the UI.

## Risks

- PCIe PHY/GCC on taro: wrong clocks hang the bus. Mitigate: read-only survey step first (Mi 10's step 1).
- SMMU: SM8450's PCIe traffic goes through the APPS SMMU (`iommu-map` 0x96); if TZ forbids bypass we
  must program a context bank like the GPU's.
- UFS guard: unaffected (nothing here writes storage).
- Firmware files are Qualcomm's: stay local (`firmware-phone/wifi/`), never published.
- Security notes in HackiOS's own reviews (WPA message-1 handling, unchecked remote lengths) apply.

## Bluetooth (same chip, later)

Mi 10 also starts Bluetooth (`htbtfw20.tlv` + `htnv20.bin` over UART, overlay in
`devices/xiaomi-mi-10/overlays/userland`). Our phone's BT is the same QCA6490 core: BT_EN gpio81.

## Phase 1 status (2026-11-12): done, links, inert

- `redmagic7/wifi/build-ath11k.sh` stages HackiOS's `tools/mk-ath11k.sh` + `wifi/` + the Mi 10 overlay +
  ours (`redmagic7/wifi/overlay/`) into `redmagic7/wifi/build/` and builds
  `build/xnu-deps/build/ath11k/out/ath11k.o` (518 KB, 18 ath11k files, 0 stubs, 53 kernel imports).
  Mac quirks handled there: `sha256sum` and `ld64` wrappers, and the target forced to
  `arm64-apple-macos13.0` because the mondrian kernel is built as a macOS target.
- Our overlay `lx_bus.c`: `hw_rev = ATH11K_HW_WCN6855_HW20` when the option `wifi_wcn6855` is set.
- Kernel side copied to `ref/redmagic/xnu-rm/bsd/dev/rmwifi/` (hakios.{wifi,wlfw,lx,qmi,apple80211,sta,net}.c,
  umi_{wifi,mhi,pcie,rpmh}.c, umi_pcie_phy.h) with headers in `bsd/sys/`, all `standard` in `bsd/conf/files`.
  `rm_glue.c` stands in for HackiOS-only symbols (log, loader hooks, firmware images, GIC SPI attach, locks,
  WLAN nub). Nothing calls `hakios_wifi_init()` yet.
- Link: `makedefs/MakeInc.def` adds `$(RM_OBJECTS)`; build the kernel with
  `RM_OBJECTS=<checkout>/redmagic7/wifi/build/xnu-deps/build/ath11k/out/ath11k.o`
  (required now: without it the link misses the lx_* entry points).
- Output: `redmagic7/kernel/kernel.redmagic7.v9-wifi-inert` (17.78 MB). Not booted.
- Still to do before phase 2: `tools/mk-wlfw.sh` header (hakios.wlfw.c compiles to its stub without it),
  replace the Mi 10 numbers in umi_pcie.c/umi_wifi.c/umi_rpmh.c (GPIOs, GCC, PHY table, rails) and call
  `hakios_wifi_init()` behind boot-arg `rmwifi`.
