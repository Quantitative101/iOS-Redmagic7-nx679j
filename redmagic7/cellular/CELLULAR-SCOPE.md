# RedMagic 7 cellular on iOS: scope

Status: **feasibility only, nothing built.** Written 2026-11-12 from a look at ios-redfin plus a few read-only checks on
the phone. Pick this up after the GPU (Metal) work and the other open items are in a good state.

## Goal

iOS (16, the same userland as the rest of the port) on the NX679J gets, in order:

1. **Status:** SIM detected, operator name, signal strength. Shown only in our logs at first.
2. **SMS** send and receive.
3. **Mobile data** (LTE/NR) when Wi-Fi is off.
4. **Later, maybe:** signal bars in the status bar, dual-SIM selection, voice calls (no one has a voice path yet).

Android must stay untouched. Slot b and the modem partitions are read-only from iOS, except what the modem's own
storage service legitimately writes to `modemst1`/`modemst2`. See the risk section before writing to them.

## Reference: ios-redfin (Pixel 5, SM7250)

<https://github.com/DenialOfGravity/ios-redfin>: MIT, plus APSL 2.0 for the XNU patches. Same XNU (xnu-8796.141.3), iOS
16.7.16 userland. Everything below comes from their own notes (NOTES.md, dated up to 2026-11-09); none of it is
independently verified. Their README still lists cellular as "not working yet".

How they do it:

- **No kernel baseband emulation.** A userland daemon (`wland`, `phone/wifi/wland.c`) does the work.
- **Modem boot** via TrustZone's firmware loader (PAS: init image, memory setup, segments, authenticate and reset),
  run from userland. A kernel patch (their 0094) gives userland physical memory access and secure-monitor (SCM) calls.
- **rmtfs**, the modem's storage service, serves modemst/fsg. Without it the modem dies about 40 s after boot.
- **QMI over QRTR:** a client (`phone/wifi/cell.c`) for the DMS, UIM, NAS and WDS services.
- **IPA v4.7 + GSI** (the modem's packet hardware) set up from userland (`phone/wifi/ipa.c`, `ipaqmi.c`). Data comes
  out on a `utun`.
- **CommCenter bridge** (`phone/wifi/bridge.c`): Apple's CommCenter gets a fake modem-boot handshake. A filtering
  bridge passes its baseband messages (ABP over qmux) to the real modem, with an allow/drop/error policy for each
  service and message (about 85 seen).
- **Claimed working:** LTE data (DNS, ping, TCP, browsing, about 2 Mbit/s), Cellular Data settings, SMS send.
- **Open:**
  - repeated modem ERR-FATAL in `ipa_hal.c` (two channels on distinct event rings);
  - idle data stalls;
  - SMS receive untested;
  - no voice, no signal bars;
  - "Cell utun is not published to configd: broken".

Before starting, read their NOTES.md, HANDOFF.md and `docs/redfin/port-plan.md` (branch `known-good-r57a-kernel`)
in full; the summary above was partly made from a model's digest.

## What the RedMagic 7 already has (checked 2026-11-12 on Android, read-only)

- **Modem:** remoteproc `4080000.remoteproc-mss`. The other remoteprocs are adsp, cdsp, slpi and spss.
- **Firmware in `/vendor/firmware_mnt/image`** (root needed to read it):
  - modem: `modem.mdt` / `modem.b00…`;
  - IPA: `ipa_fws.mdt` / `.b00–.b04`;
  - GSI: `fw_ipa_gsi_3.0_w.elf`.

  Keep it on the phone. Never commit vendor binaries.
- **Partitions:** `modem_a`, `modem_b`, `modemst1`, `modemst2`, `fsg`.
- **IPA:** `qcom,ipa@3e00000`, `qcom,ipa-hw-ver = 0x16` (Qualcomm's downstream version number; newer than the
  Pixel 5's v4.7, exact name to be confirmed from Nubia's kernel source). Android drivers: `ipa`, `rmnet_ipa3`,
  `qcom,msm-mhi-dev(-net)`.
- **SIM:** `persist.radio.multisim.config = dsds` (two SIMs, both active). Baseband
  `NX679J_Z69_UN_ZML1T_V612`.
- **SCM / PAS already works in our kernel:**
  - `xnu-rm/bsd/dev/mondrian_gpu.c` loads and authenticates the A730 ZAP shader (`mondrian_scm`, around line 1985;
    PAS init, mem_setup, auth around 2069–2102);
  - `mondrian_ufs.c`, `MondrianCPU.cpp` and `rmwifi/hakios.wifi.c` also make SCM calls.

  The modem is the same flow with the MSS PAS id (4 upstream; check against the vendor DT) and its own memory region.
- **Nubia kernel source** (GPL): sparse checkout at `ref/redmagic/nubia-NX679S/kernel_platform`. Add the IPA,
  remoteproc/PIL, QRTR/GLINK and rmnet directories when needed.

## Differences from redfin that matter

| Area | redfin (SM7250) | RedMagic 7 (SM8450) | Effect |
|---|---|---|---|
| IPA | v4.7, has mainline Linux support | hw-ver 0x16, mainline support unlikely (unverified) | redfin's `ipa.c` won't carry over; port from Nubia's downstream driver. Highest risk. |
| Wi-Fi | firmware runs on the modem (WLFW over QRTR) | WCN6855 on PCIe (our `rmwifi`, link training still parked) | Their Wi-Fi code isn't reusable; the QRTR/QMI plumbing is. |
| SIMs | single | dual (dsds) | CommCenter and UIM need two subscriptions, or force single SIM at first. |
| Kernel userland access | patch 0094 (physical memory + SCM to userland) | SCM in kernel only | Pick an approach (below). |
| Modem storage | rmtfs serves the phone's own partitions | same; we dual-boot so they are current | Good, but iOS writes reach Android's modem state. |

## Proposed phases

Each phase ends in one flash with a pass/fail line in the log ring, as the GPU probe does.

### Phase 0: research (Mac only, no flash)

- Read redfin's port-plan, `cell.c`, `bridge.c`, rmtfs and PAS code, and the 0094 kernel patch.
- Pull from the live device tree (Android, read-only `/proc/device-tree`):
  - the MSS node: PAS id, memory regions, interrupts, power domains and rails (cx, mss);
  - the SMP2P / GLINK / IPCC nodes for the modem edge;
  - the rmtfs shared memory region;
  - the IPA node's registers, interrupts, SMMU and its GSI memory.

  Write a generator like `wifi/gen-pcie-phy.py`.
- **Decide kernel vs userland.** Our Wi-Fi and GPU work lives in the kernel (`bsd/dev/...`). Redfin does it from
  userland. Kernel fits our flash-and-read-log loop and avoids a physical-memory-to-userland hole. Userland makes it
  easier to talk to CommCenter. Likely split:
  - **in the kernel:** PAS, rmtfs memory and GLINK/QRTR transport;
  - **in a userland daemon:** the QMI clients and the CommCenter bridge.

### Phase 1: modem boots and stays up

- In the kernel:
  - vote the modem's power rails through the RPMh lock (`mondrian_rpmh_rail`, serialized; see the touch race fix);
  - load `modem.mdt`/`.bNN` from the vendor partition, or embed through the `rm_blob` mechanism for testing, but
    keep it local;
  - PAS init, mem_setup, auth and reset.
- Handshake with the modem through SMP2P and wait for its ready signal.
- **Pass:** the modem reports it is up and no error-fatal shows within 2 minutes. It will err-fatal at about 40 s
  until Phase 2's storage service exists, so either do both in one flash or count "alive past handshake" as the pass.

### Phase 2: rmtfs + QRTR + first QMI

- GLINK transport to the modem, QRTR on top, and an rmtfs server over the `modemst1`/`modemst2`/`fsg` regions.
- **Start read-only:** serve the reads and log the writes instead of committing them, until it is clear what the
  modem writes.
- QMI client queries:
  - DMS: firmware revision, IMEI (log only a hash);
  - UIM: card status for both slots;
  - NAS: serving system, operator, signal.
- **Pass:** log lines for SIM present, operator and RSSI/RSRP. **This is the go/no-go point for the whole effort.**

### Phase 3: SMS

- WMS over QMI: send to a number the user gives, then receive.
- No CommCenter yet: a test hook in the daemon.

### Phase 4: CommCenter bridge

- Port redfin's fake modem-boot handshake and filtering ABP bridge (`bridge.c`, their policy file). Start in observe
  mode.
- Settings should show the carrier, and Messages should send SMS.

### Phase 5: mobile data (largest, riskiest)

- IPA hw 0x16 + GSI bring-up:
  - load the IPA firmware (`ipa_fws`) via PAS;
  - load the GSI firmware;
  - set up the endpoint and channel tables (from Nubia's downstream `ipa_v3`/`gsi`);
  - IPA control over QMI (service 0x31, like redfin's `ipaqmi.c`).
- WDS starts a data call, and packets come out on a `utun` (or a proper network interface published to configd,
  which redfin hasn't fixed).
- Watch for redfin's `ipa_hal` error-fatal: the notes link it to two channels on distinct event rings.

### Phase 6 (optional)

- Signal bars;
- dual-SIM switching;
- voice: an audio path through the ADSP, unexplored by anyone.

## Risks and safety

- **modemst1/2 and fsg** are Android's live modem calibration and state. A bad write can break Android's cellular
  too. Before any write:
  - take a dated backup of all three to the Mac (`redmagic7/backups/`, never committed);
  - start with log-only writes.
- **IMEI and SIM data** are personal. Log hashes or truncated values only; never commit logs that contain them.
- Modem crashes inside iOS are recoverable by reboot, but every test costs a flash plus the manual Power+Vol Down
  return.
- **Licenses:**
  - redfin's own code is MIT, so it can be copied with attribution;
  - its XNU patches are APSL 2.0, like our tree;
  - Nubia/Qualcomm Linux drivers are GPL-2.0. Port by re-deriving register sequences, as the GPU and Wi-Fi work did,
    and do not paste GPL code into the XNU tree.
- **Order:** this depends on nothing else, but Wi-Fi first is better: easier debugging, and an alternative network
  while cellular is half-built.

## Effort guess

- Phases 1–2 (go/no-go): several flash days.
- Phases 3–4: a similar amount again.
- Phase 5 (IPA data): the bulk, open-ended; redfin is still fighting crashes and stalls there on an older, better
  documented IPA.
