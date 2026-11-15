# RedMagic 7 Bluetooth on iOS: scope

Status: **scoping, read-only probes only** (v180). Written 2026-11-14, after Wi-Fi started working on the same chip.

## Hardware (Android, 2026-11-14, `recon/modem-bt-ref/`)

- **Chip:** the QCA6490's Bluetooth half (DT `soc/bt_qca6490`, `qcom,qca6490`). It shares the power unit and XO with
  Wi-Fi.
- **GPIOs:** BT_EN 81, WLAN_EN 80, SW_CTRL 82, XO select 204.
- **Rails:** vdd-aon 0.966 V, rfa1 1.9–2.1 V.
- **Transport:** HCI over a UART. QUPv3 wrapper 2, serial engine 5 at **0x894000**: `qcom,msm-geni-serial-hs`,
  `/dev/ttyHS0`, clocks se-clk, m-ahb, s-ahb.
  - GCC registers (`gcc-sm8450.c`): s5 CBCR +0x2e610, m-ahb +0x2e004, s-ahb +0x2e008, wrap2 core +0x33280, core-2x
    +0x3328c, vote +0x62010.
- **Firmware** in `/vendor/bt_firmware/image`:
  - `hpbtfw20.tlv`, `hpbtfw21.tlv`;
  - NVM `hpnv20.bin` and board variants `hpnv20.bXX` / `hpnv20g.bXX`, `hpnv21.*`.
  - Not pulled to the Mac yet; same local-only rule as the Wi-Fi firmware.

## What exists elsewhere

- **iOSPlus10** (OnePlus 10 Pro, the same SM8450 + QCA6490, the same pins) has working-ish Bluetooth:
  - `patches/xnu/0330`–`0338`, `patches/xnu/README-bluetooth.md`, `docs/bluetooth-plan.md`,
    `userland/bluetooth/{negroni-bluetool.c,hcitest.c}`;
  - `negroni_bt.c`: QCA6490 power sequence and a **polled GENI UART on QUP2 SE5 (0x894000)**, the same engine as
    ours;
  - `negroni_bt_hci.c`:
    - H4, HCI reset, baud switch (vendor 0xfc48) to 3 Mbaud;
    - `hpbtfw21.tlv` download, then `hpnv21.bXX` NVM chosen by board id;
  - `negroni_bt_h4.c`: kernel control `com.apple.uart.bluetooth`, `/dev/cu.bluetooth`, `/dev/uart.bluetooth`,
    `/dev/btwake`, and QCA in-band sleep;
  - `negroni_bt_shim.c`: answers the Broadcom/Apple vendor commands bluetoothd sends, and translates legacy LE to
    extended LE;
  - **iOS side:**
    - environment `BT_HCI_TRANSPORT=H4BC:POSIX:com.apple.uart.bluetooth,3000000`;
    - an ADT node `/arm-io/bluetooth` with a non-zero `local-mac-address`;
    - a BlueTool stand-in that answers `RunBuiltinScript` with 0;
  - **status:** bluetoothd gets through init (0337); LE scanning is reported working (0338); no pairing shown.
- **HackiOS** (Mi 10, QCA6390) reports native bluetoothd with discovery and encrypted pairing (commit b7122b2).
  - Our clone is partial: only `bsd/sys/hakios.bluetooth.h` is on disk (H4 parser, in-band sleep bytes, TLV ack,
    NVM tag-17 baud fix; already in `xnu-rm/bsd/sys/`).
  - The drivers (`drivers/bluetooth/hakios.bt.c`, `devices/xiaomi-mi-10/overlays/.../umi_bluetooth.c`,
    `hakios.bluetooth.cpp`, `umi_bluetool.c`, `inject_bluetooth.c`) would need `git checkout HEAD -- <paths>`, a
    network fetch.

## Plan

1. **Census (v180, read-only):** `RM BT:` lines.
   - GCC CBCRs of the QUP2 clocks and the vote register.
   - SE5's `GENI_FW_REVISION_RO` (protocol 2 = UART), only if the AHB clocks are already on.
2. **Clocks and pins:**
   - vote the QUP2 clocks (vote register 0x62010) and set the SE5 RCG;
   - set the UART pins' function (TLMM, from Android's pinctrl `active`);
   - then read the SE registers.
3. **Power-up:** BT_EN (gpio81) high, **only after Wi-Fi's PCIe link is trained**. In v160, BT_EN high during
   Wi-Fi's power-up kept the link from training, so the order matters. Check that Wi-Fi traffic survives.
4. **HCI:** polled GENI UART at 115200, HCI reset, the QCA version command, baud switch, TLV + NVM download (iOSPlus10
   0331–0334 as reference).
5. **iOS:** `com.apple.uart.bluetooth` control + `/dev/cu.bluetooth`, `/arm-io/bluetooth` ADT node, BlueTool
   stand-in, vendor-command shim (iOSPlus10 0335–0338).

## Risks

- BT_EN shares the power unit with Wi-Fi; a bad sequence can drop the Wi-Fi link (recoverable by reboot).
- Unclocked QUP register reads can hang the bus, so step 1 reads only what is safe.
- **Licenses:** iOSPlus10 and HackiOS code is APSL/MIT as stated in those repos (check each file before copying).
  Qualcomm's Linux BT drivers are GPL-2.0: re-derive the sequences rather than pasting code.

## Extra notes (from the original session's own scope scan, 2026-11-14)

- **`live.dts` lines:** `bt_qca6490` :12630; the UART is `hsuart0` = `qup_uart@894000` :430.
- **Our kernel:** `mondrian_touch.c` has GENI/QUP code, but for wrapper 0 only (touch). It is the closest local
  starting point for a polled SE5 UART.
- **iOSPlus10 details:**
  - all 4 RPMh rails are voted on and never off;
  - HSP download: version `fc00/19`, then 3 Mbaud `fc48`, then `hpbtfw21.tlv`, board id, and `hpnv21.bin`;
  - the ADT `bluetooth` node also carries `transport-encoding` and `transport-speed`.
- **First real step:** port iOSPlus10 0330 stage A plus the 0334 clock fix:
  1. read SE5 `GENI_FW_REVISION_RO`, expecting protocol 2;
  2. toggle BT_EN;
  3. send HCI_Reset at 115200 and expect Command Complete with status 0.
