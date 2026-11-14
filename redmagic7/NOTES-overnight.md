# RedMagic 7: overnight notes (2026-11-11 → 12)

## State
- iOS 16 reaches the lock screen (v5d boot image + tweaked root). Log: `logs/ring6-lockscreen.txt`.
- Phone is on Android (slot b). boot_a still holds v5d.

## Ready to test: touch (v6)
- `payload/shim-boot-v6.img` = kernel v6 + boot arg `rmtouch=1`. Not flashed.
- Hardware (live DT + phone): Goodix **GT9897** (Berlin) at I2C `0x5d` on QUP SE4 (`i2c@990000`, Android bus 7),
  SDA gpio16 / SCL gpio17, reset gpio20, IRQ gpio21 (same as the POCO), AVDD = RPMh `ldoc3` (pm8350c_l3, 3.3 V),
  IOVDD = load switch gpio89. Reports in 1080x2400.
- Kernel change (`xnu-rm/bsd/dev/mondrian_touch.c`, all under `rmtouch=1`):
  bit-banged open-drain I2C on gpio16/17 (`rm_i2c_*`), Berlin framing (32-bit BE address, repeated-start read),
  power order IOVDD→AVDD→reset, then the POCO's `confirm` / `ic_info` / `coor_mode` / report thread unchanged.
  The firmware is in the part's flash: no upload. Coordinates scaled ×(14400/1080, 32000/2400) for MondrianHID.
- Log lines to look for: `RM TOUCH: powered`, `RM TOUCH: id BERLIN ... patch 9897`, `report thread started`,
  then `MONDRIAN TOUCH: report N finger ...` when touching.
- If `no ACK`: try `rmtouch_half=5000` (slower), check the `before:` pin dump (Android leaves gpio16/17 in qup4 function).
- Firmware/config/driver copied from the phone: `firmware-phone/touch/` (goodix_firmware*.bin, goodix_cfg_group*.bin, goodix_core.ko).

## Boot time (17 min to lock screen)
- SpringBoard + lsd start at t=34 s; SpringBoard blocks in `LSApplicationRecord` init (symbolized with
  `ipsw dyld a2s`, slide 0x2cc20000) until **installd** is launched at t=1047 s, then unblocks the same second.
- The first boot with tweaks was cut off at 8 min, so the app database had not been written yet. The next boot will
  show whether it persists (writable root). If it is still ~17 min: find what demand-launches installd (lsd XPC
  `com.apple.mobile.installd`?) and consider a launchd RunAtLoad for installd via the tweak script.
- Also noisy: bluetoothd and fairplayd.H2 relaunch every ~5 s (120+ times); candidates to disable in launchd.plist.

## Fan (next driver)
- MCU on QUP SE16 I2C (`i2c@884000`, Android bus 12, addr `0x2a`), pins gpio60/61 (SE16 I2C active).
- Power: pm8350c_l11 via `regulator_set_voltage(FAN_VREG_VOLTAGE)` + enable (fan_core.c:1103-1119) → RPMh `ldoc11`.
- Reset: **PM8350C GPIO6** (PMIC GPIO over SPMI, active low). The kernel has no SPMI access; the shim has `spmi.c`,
  so the reset release can be done in the shim before the kernel, or SPMI ported into the kernel.
- Vendor ID: TLMM gpio7 (HuaYing vs TaiDa).
- Commands: PWM `{0x08, duty}`; RPM: `0x01`, wait 1 s, `0x03`, read `0x02`; current `0x06`; temp `0x07`; FW ver `0x09`.
  MCU firmware fan_fw_05/06.h is only for updates.
- Plan: reuse the touch bit-bang I2C (generalize `rm_i2c_*` to take pins/address), RPMh `ldoc11` on, shim SPMI for
  GPIO6, then a small kernel thread: temp from tsens or a fixed duty to start; later a sysctl / IOKit knob.

## GPU (later)
- Our firmware headers are already in the kernel (`mondrian_{gmu,sqe,zap}_fw.h` from this phone); `gpu=0` for now.
  Same chip id as the POCO (Adreno 730 0x7030002). Try `gpu=1 gpupower=1 gmustart=1` only after touch works.

## v7: automatic return (built 2026-11-12, not flashed)
- `payload/shim-boot-v7.img` = kernel v6 + `mondrianreset=1500`: after 25 min the kernel flushes the log ring, sets the
  IMEM restart reason to fastboot (0x146aa000+0x65c, matches live.dts) and resets via PSCI.
- The BCB write to `misc` is refused by the UFS guard (writes outside the iOS file are blocked), so landing in fastboot
  depends on ABL reading the IMEM reason. SM8450 Linux also stores the reason in PMIC SDAM (nvmem), which we can't reach
  without SPMI. If ABL ignores IMEM, the phone reboots into slot a again; the slot retry counter may then fall back to b.
- Once in fastboot, the Mac side runs `fastboot -s <adb-serial> set_active b && fastboot -s <adb-serial> reboot`, then pulls the ring.

## 2026-11-12 daytime results (v7–v8e)
- Touch WORKS (v8b+): ic_info is at 0x10068 on GT9897 (not 0x10070); coor-mode command must go over I2C
  (mondrian_fw_write_big dispatches to rm_goodix_write). The IRQ line (gpio21) never asserts, so
  `mondriantouchpoll=1` polls the report block every vsync wake (~100 Hz, zero NACKs). IRQ path still to fix.
- Reset timer: `mondrianreset=600` now counts with a spin on mach_absolute_time (tsleep/delay deadlines stall).
  It fires, but ABL ignores the IMEM restart reason and the misc BCB write is refused by the UFS guard, so the phone
  does not land in fastboot by itself. Slot-a retry count stays 7: no ABL fallback either. Manual Power+Vol Down.
- Temps (TEMP line every ~30 s, tsens0/1): boot peak 80–84 °C at t≈70–80 s, idle 50–53 °C.
- Fan: kernel does Nubia's order (SPMI: GPIO6 low, ldoc11 3312 mV on incl. mode, 100 ms, GPIO6 high); readback
  confirms GPIO6 = output high (status 0x80, mode 0x1, out 0x80, en 0x80), ldoc11 reads back enabled 3312 mV.
  gpio60/61 (SE16 I2C) can be driven low and read back. Yet a full 0x08–0x77 scan at ~45 kHz gets zero ACKs.
  Android drives the MCU fine at 12-002a (fan_enable/fan_speed_level sysfs). Fan spins (MCU default or floating PWM).
  Next ideas: use the GENI SE16 controller instead of bit-bang; compare pin CFG/pull with Android while fan on;
  check whether the shim's early release (before power) leaves the MCU in a bad state despite the kernel pulse.
