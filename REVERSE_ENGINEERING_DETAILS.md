# Reverse-Engineering the Silead SIL6250 / Petaic Fingerprint Sensor

How this driver stack was built, end to end — from "Linux sees no fingerprint
device at all" to a working `fprintd` login on the Huawei MateBook X Pro 2024.

This document is the narrative companion to the code. It explains _why_ each
component (`kernel/`, `lib/`, `tools/`, `fprint-driver/`) exists and how each
fact it encodes was recovered. Nothing here is needed to _use_ the driver — see
the top-level `README.md` for that — but everything here is needed to _trust_,
_audit_, or _extend_ it.

---

## 0. TL;DR

- The sensor is a **Silead GSL6150r touch ASIC + HDSC HC32F460 MCU** module,
  branded **Petaic**, exposed to the OS as the ACPI device **`SIL6250`**. It is
  **not** USB and **not** a normal SPI fingerprint reader.
- Host↔sensor transport is an **EC-arbitrated shared-memory mailbox** at a fixed
  physical address, with two GPIO "doorbell" strobes and one GPIO interrupt. The
  kernel module `sil6250.ko` (`kernel/`) is a thin broker for exactly these three
  resources and nothing else.
- All command framing, a **TLS-PSK secure channel**, and the **image capture
  loop** live in userspace (`lib/`, `libsil6250`). The PSK ring was recovered
  from the Windows service binary; this unit uses the key `shiba`.
- The sensor is **Match-on-Host**: it streams raw 64×80 images and does _no_
  matching itself. There is **no firmware calibration** — the raw frame is
  byte-identical to what Windows captures (I think, kinda hard to test).
- The 3.2 × 4 mm patch is too small for classical minutiae matching. The Windows
  matcher is a **local-feature keypoint + descriptor + geometric-verification**
  algorithm (SIFT-class), which we reverse-engineered and reimplemented
  **clean-room** in `lib/petaic_sift.c`.
- Single-frame match-on-host on this tiny sensor is intrinsically marginal
  (~30 % single-frame FRR is the _sensor ceiling_ — the original Windows matcher
  hits it too); the shipped stack reaches usable accuracy with a quality gate,
  multi-frame verify, and an enroll-diversity gate.

---

## 1. Identifying the hardware

The starting symptom: `lsusb` shows no fingerprint device, and there is no
Goodix `27c6:xxxx` USB sensor. The reader is not on USB at all.

The device is on the **ACPI/platform** side. In the DSDT, node `\_SB_.SPBA`
declares two candidate hardware IDs and switches between them at runtime based on
an Embedded Controller field:

```asl
Device (SPBA)
{
    Name (_HID, "GXFP5130")      ; Goodix sibling (fallback)
    Name (_CID, "GXFP5130")
    Method (_INI, 0, NotSerialized)
    {
        If ((HWEP == 0x02))      ; EC reports which sensor is fitted
        {
            _HID = "SIL6250"     ; ← the Petaic/Silead part on this unit
            _CID = "SIL6250"
        }
    }
    Method (_STA, 0, NotSerialized)
    {
        If ((HFTE && HWEP)) { Return (0x0F) } Else { Return (Zero) }
    }
    Method (_CRS, 0, Serialized) { /* see §2 */ }
}
```

On this laptop the EC field `HWEP == 0x02`, so the live HID is **`SIL6250`** and
`_STA` returns `0x0F` (present + enabled). The same DSDT supports a Goodix
GXFP5130 variant on other units — a fact that turned out to be enormously useful
(see §3.2).

`SIL6250` is a **Petaic Co., Ltd. touch fingerprint sensor** (per Windows driver
databases). This is what `kernel/sil6250.ko` binds to via its ACPI match table.

---

## 2. The ACPI resources — what the kernel module claims

The `_CRS` of `\_SB_.SPBA` hands out exactly four resources:

```asl
Memory32Fixed (ReadWrite, 0xFE800000, 0x00001000)   ; 4 KB MMIO window
GpioInt (Level, ActiveHigh, ExclusiveAndWake, ...)  ; EC → host "RX ready"
GpioIo  (Exclusive, ..., IoRestrictionOutputOnly)   ; output strobe #1
GpioIo  (Exclusive, ..., IoRestrictionOutputOnly)   ; output strobe #2
```

The early guesses (recorded for honesty) were wrong in instructive ways:

- The 4 KB window was first assumed to be an SPI-controller BAR or a register
  file. It is neither — it is a **mailbox / dual-port RAM** (§3).
- The two output GPIOs were first labelled "power" and "chip-select". They are
  actually **handshake strobes** (`write_done`, `read_done`), not power
  (§3.3). Driving them high and pulsing reset never powered anything on, because
  nothing on the host side powers the sensor — the EC does.

These three resource _kinds_ (one MMIO window, one input IRQ, two output
strobes) are the entire hardware contract, and they are all `kernel/sil6250.ko`
exposes: `mmap()` for the window, `SIL6250_SET_GPIO` for the two strobes,
`SIL6250_WAIT_IRQ` for the interrupt. The UAPI is `kernel/sil6250_uapi.h`.

---

## 3. The transport: an EC-arbitrated shared-memory mailbox

This was the hardest single thing to pin down, and it was cross-checked three
independent ways.

### 3.1 Why raw access returns garbage

Reading `0xFE800000` directly on Linux returns all `0xFF`, and writes don't
stick. The window is **only coherent inside an EC-granted transaction** — the
SoC↔EC eSPI link decodes the window as a mailbox, and outside a transaction it
floats. So the transport is not "map memory and poke registers"; it is a
**doorbell protocol** arbitrated with the EC.

### 3.2 The Goodix sibling gave us a verified reference

Because the same `\_SB_.SPBA` node, the same `0xFE800000/0x1000` window, and the
same GPIO layout are shared with the **Goodix GXFP5130** variant, a pre-existing
open-source GXFP5130 Linux driver
([Void755/gxfp_linux_driver](https://github.com/Void755/gxfp_linux_driver)) —
written for the _sibling_ sensor on the _same laptop family_ — served as a
Rosetta stone for the platform transport. It
is real, compiling kernel code that gets ACKs from the mailbox, so the framing it
encodes is **verified, not inferred**:

- Window is **split**: **TX at offset `0x000`**, **RX at offset `0x200`**.
- MMIO accesses are **8-byte (qword) aligned**.
- The two output GPIOs are **`write_done` / `read_done` strobes** with specific
  pulse timings; the `GpioInt` is the EC→host "RX ready" line.
- "EC arbitration" is **not** an EC-register command — it is a host-side critical
  section plus the GPIO strobes (`get_spb_permission` / `put_spb_permission` are
  a mutex lock/unlock; `notify_ec` toggles a strobe).

The Petaic sensor shares this **outer** platform transport (the `0xF0` wrapper,
the TX/RX split, the strobes, the EC critical section) but uses a **different
inner command framing** (§4) — Goodix's MP/Goodix-body layer is replaced by
Petaic's `0x5A` framing.

### 3.3 Static RE of the Windows driver confirmed the model

The correct Windows driver (see §3.4) was disassembled. The transport functions
matched the sibling-driver model exactly:

- `get_spb_permission` checks a static flag and enters a critical section;
  `put_spb_permission` leaves it. → a host-side lock, **no EC register poke**.
- `notify_ec(gpio_index, high_ms, post_ms)` toggles one of the two `GpioIo`
  outputs (index 0 = `write_done` after a TX, index 1 = `read_done` after an RX).
- `wait_for_irq` waits on the `GpioInt`.
- `shm_read3` / `shm_write3` copy qword-aligned data to/from the mapped window
  (writes at `+0x000`, reads at `+0x200`).

So the per-command sequence the userspace driver implements
(`lib/petaic_transport.c`) is:

```
lock (get_spb_permission)
  write request packet → window +0x000
  pulse write_done strobe                (notify_ec, index 0)
  wait for GpioInt                       (EC → host: response ready)
  read response packet(s) ← window +0x200
  pulse read_done strobe                 (notify_ec, index 1)
  verify checksum
unlock (put_spb_permission)
```

`lib/petaic_transport.c` owns this whole transaction; the kernel only provides
the three primitives. `tools/petaic_record` is the bring-up smoke test for it.

### 3.4 Picking the right Windows driver

Three Petaic driver CABs from the Microsoft Update Catalog were triaged:

| Driver version | Bus / matching                         | Verdict                       |
| -------------- | -------------------------------------- | ----------------------------- |
| 5.203.0.5      | USB, Match-on-Chip (GD32W515)          | wrong — USB-only, stubbed SPI |
| 4.0.110.2      | USB, Match-on-Chip                     | wrong — USB-only, stubbed SPI |
| **3.9.5.23**   | **ACPI/SHM, Match-on-Host (HC32F460)** | **correct**                   |

Two things distinguished the correct one:

1. Its INF declares `ACPI\SIL6250` and `ACPI\GXFP5130` (the USB INFs declare only
   `USB\VID_28E9&PID_…`).
2. Its identity string reads `MoH,POA,SPI,HC32F460JEUA,GSL6150r` — **Match-on-
   Host**, HDSC HC32F460 MCU, GSL6150r ASIC — versus the USB builds'
   `MoC,POA,USB-HID,GD32W515TIQ6,GSL6150`.

The **Match-on-Host** finding is the pivotal architectural fact: the sensor
streams raw images and does no matching, so a Linux driver must supply the entire
matching engine, not just transport. That is why this repo has a `lib/` with a
matcher in it at all.

---

## 4. Packet framing and the command set

The bring-up command protocol was recovered from static RE and then confirmed
**byte-for-byte** against a live trace (§9) and against the real hardware.

A standard command is a fixed **27-byte frame** (zero-padded to 32 on the wire):
an 8-byte outer header, an 8-byte inner header, a fixed 7-byte zero payload
region, and a 4-byte little-endian checksum.

```
Outer (offset 0x00):
  0x00  F0            magic
  0x01  13 / 10       outer length/type
  0x02..0x06  00..    reserved
  0x07  NN            transaction sequence (increments per command)

Inner (offset 0x08):
  0x08  5A            inner marker
  0x09  cc            command code
  0x0A  00/01/02/...  direction / usage (control / read / host→sensor write)
  0x0B  04            access width — always 0x04; the sensor NAKs other widths
  0x0C..0x0F  BE32    TX payload length (big-endian)
  0x10..      payload region
  tail   LE32         checksum = one's-complement sum of inner header + region
```

Three corrections cost real debugging time and are worth recording:

1. **Access width is always `0x04`.** An earlier "8-bit width" reading was a red
   herring caused by a frame that was _also_ too short.
2. **The BE32 field is the TX payload length, not the response length.** They are
   independent `ec_transfer` arguments that merely coincide for some commands.
3. **The frame must be physically 27 bytes** even when the payload is zero; the
   firmware rejects short frames regardless of checksum. This is
   `PETAIT_STD_PAYLOAD_REGION = 7` in `lib/petaic_proto.c`.

The command set actually used by the stack:

| Cmd            | Meaning                                                          |
| -------------- | ---------------------------------------------------------------- |
| `0x1b`         | init / firmware probe → fw **0.0.32.15** (`0x200f`)              |
| `0x14`         | get sensor info → **64×80**, image size 5120                     |
| `0x11`         | **finger-detect poll** (`0x01` down / `0x00` up) — the workhorse |
| `0x00`         | **TLS handshake records** (host→sensor write, dir `0x02`)        |
| `0x22`         | TLS read (sensor→host, dir `0x04`)                               |
| `0x37`         | image-transfer control (one-shot arm)                            |
| `0x38`         | encrypted-image bulk record (TLS application data)               |
| `0x20`         | module info (id `80 33 5f 9c aa`) — **not** calibration          |
| `0x13`, `0x21` | host-side no-ops (`SendCmdToFirmware … ignore this cmd`)         |

Frame build/parse/checksum is `lib/petaic_proto.c`.

---

## 5. The TLS-PSK secure channel

The image path is wrapped in **TLS-PSK-WITH-AES-256-GCM-SHA384** (the firmware
embeds mbedTLS; the **host is the TLS server**, the sensor MCU is the client).
Handshake records ride over cmd `0x00` (write) / cmd `0x22` (read); each TLS
record is split into a 5-byte header write followed by a body write, with the
data starting at inner offset 7.

### Recovering the keys

The PSK is **not** per-device and **not** the mbedTLS default `Client_identity`.
The host holds a **ring of four PSKs keyed by identity**, and the identities are
dog-breed names. The ring was recovered two ways that agreed byte-for-byte:

1. Unsealing the Windows service's at-rest blob (DPAPI-NG, `LOCAL=machine` — the
   weakest binding, so any local process can unseal it).
2. The **same bytes are hardcoded constants in the Windows service binary** — the
   DPAPI sealing was only obfuscation of compiled-in constants.

| Identity       | 32-byte PSK (hex)                                                  |
| -------------- | ------------------------------------------------------------------ |
| `shiba`        | `7350103739bfbc8e68d6c9a8942799343d82c72f015d00620f79142cdfc34c81` |
| `saintbernard` | `ee9abb5a2b9ec34a81664b53c2cfcdd855f20a622c4da2e8f51ee24e9510dd2b` |
| `chihuahua`    | `587d3f962e3d7ea1f08c0fb79c03784d9fec2d1f97f76c7f5d2f66ed432d9fe9` |
| `bordercollie` | `10585a35ac1e78ce4f308de7352dd1af62539500dbe71be215d7ab51ae9fe340` |

These are **identical on every unit running this driver build**, so the Linux
stack hardcodes all four and lets the handshake select the right one. On a fresh
/unprovisioned unit the sensor presents an **all-zero 32-byte identity**, so the
strategy is: register all four keys, and the one whose client Finished MAC
verifies is the one in use. On this hardware that is **`shiba`** — confirmed
_cryptographically_ by a completing handshake, not by guesswork.

Note the identity is a _key-id label_ (derived as `SHA384(provisioning-random)`
truncated to 32 bytes) and is independent of which PSK the sensor holds — a
subtlety that initially looked like "the dog name should appear on the wire" but
doesn't.

The handshake is `tools/petaic_tls` (smoke test) and `lib/petaic_engine.c`
(production path, mbedTLS server BIOs wrapping the mailbox transport).

---

## 6. The capture loop and its non-obvious traps

Each of the following cost a debugging round-trip and is now baked into
`lib/petaic_engine.c`:

1. **Finger detection is `0x11` polling, never the GpioInt.** The interrupt is
   **level-triggered** and fires spuriously on a residual assertion left by the
   prior transaction ("phantom fingers"). Poll cmd `0x11` instead.
2. **Order is load-bearing: poll `0x11` until finger → arm `0x37` → read `0x38`.**
   `0x37` is a one-shot arm; _any_ `0x11` poll between `0x37` and `0x38` stales
   it and the bulk read times out.
3. **`0x37` answers without an IRQ** (a dedicated "no IRQ" transport flag).
4. **`0x38`'s first read may return a "still capturing" status** before the real
   TLS record — re-issue until the payload begins with a TLS content type
   (`17`/`16`).
5. **The second record is a cmd-`0x00` read-only continuation** with no request
   frame. The level IRQ can re-fire and re-read the same record, so the loop
   dedups by rejecting any window whose head equals the last accepted record.
6. **Two records decrypt to 5124 plaintext** = a 5120-byte 64×80 image + a 4-byte
   trailing one's-complement checksum, which is verified.
7. **Never re-arm `0x37` mid-stream** — a stray `0x37` swallows a `0x38` record
   and desyncs GCM (MAC failure); recover by re-handshaking, not re-arming.
8. A raw image needs **no** image-key exchange — that step is template/engine
   keying, not image transport.

Multiple captures work cleanly on one TLS session (the two records land exactly
on a GCM record boundary), so the engine grabs N frames per touch without
re-handshaking. `tools/petaic_capture` is the standalone capture; `tools/qlive`
exercises the full live enroll/verify path.

---

## 7. No firmware calibration — and the host-side pipeline

A major time-saver was proving a **negative**: there is **no firmware
calibration step to replicate**.

- `EngineAdapterSelectCalibrationFormat` returns `E_NOTIMPL`.
- `0x13` / `0x21` are explicitly `ignore this cmd` (never sent to firmware).
- `0x20` is just module info, not calibration.
- The raw 5120-byte frame the sensor streams is **byte-identical to what Windows
  captures**.

Everything — enhancement and matching — happens **host-side** in the proprietary
Windows engine. Its logged sub-steps are: **`remove_line`** (a column
fixed-pattern-noise destripe), a noise check, a quality/coverage gate, and an
anti-spoof score.

The raw frame is dominated by coherent **per-column fixed-pattern noise** (sensor
multiplexing produces a periodic ~8-column dip). Destriping drops the per-column
mean std from ~8.6 to ~5.3 and exposes a real but weak ridge peak (~508 dpi
pitch). This destripe is the `pm_destripe` front-end shared by both matchers in
`lib/petaic_match.c`.

---

## 8. The matcher problem

This is where the project nearly went wrong twice, so the reasoning is worth
preserving.

### 8.1 Why minutiae matching is structurally impossible here

The standard libfprint image path runs NBIS minutiae extraction + bozorth3. On
this **3.2 × 4 mm, 64×80** patch a single frame yields only **0–3 minutiae**
(measured across multiple scales and enhancements; Gabor enhancement made it
_worse_). bozorth3 needs ~12+ overlapping minutiae. So the `FpImageDevice` path
is a dead end. That is why the libfprint driver subclasses **`FpDevice`** (the
match-on-chip pattern) and stores templates as `FPI_PRINT_RAW`, with matching
done on the host CPU — see `fprint-driver/sil6250.c`.

### 8.2 First attempt: correlation (NCC) — and why it failed

Windows clearly matches single 64×80 frames (it enrolls 14 individually-matched
samples, no mosaicing), which is only possible with a **correlation/pattern**
matcher. So the first implementation (`lib/petaic_match.c`) was best-shift
**normalized cross-correlation** over a destriped-frame gallery, with a local-
normalize bandpass that proved load-bearing (it killed cross-finger blob
correlation).

Offline it looked great (FAR 0 %, FRR ~6 %). But that was measured on **frozen
frames from a single touch** — near-identical images, trivially matchable. On a
real **second finger over separate physical presses** through the driver it
collapsed:

|            | genuine (enrolled finger) | impostor (other finger) |
| ---------- | ------------------------- | ----------------------- |
| NCC scores | floor **0.218**           | ceiling **0.233**       |

The distributions **overlap** — no threshold separates them. Separate presses
carry elastic deformation and pressure/moisture variation that frozen frames do
not. This is a fundamental discrimination limit of whole-frame NCC, not a tuning
bug. **The lesson — validate on separate presses, never frozen frames — shaped
everything after.**

---

## 9. Reverse-engineering the Windows matcher

The fix was to recover the _actual_ algorithm the Windows matcher uses, which is
exactly the discrimination NCC lacks.

### 9.1 Triage

The matcher DLL is a **pure algorithm library** — its only imports are
`kernel32` plus `advapi32` (and every advapi32 import is WPP tracing). No
biometric-framework, HID, SPI, or device dependencies. That makes it both an
ideal RE target and runnable under Wine for use as an oracle.

From ~37 k strings its class is unmistakable: **not** correlation, but a **local
feature-keypoint + rich-descriptor matcher with RANSAC-style geometric
verification** — note the giveaway symbol `feature_match_80x64_ml` (named for our
exact geometry) and `len_reject_fa_Inlinenum5_coarse_Rematch` (false-accept
rejection by **geometric inlier count ≥ 5**).

### 9.2 The interface (ABI)

The matcher exposes an 11-slot vtable mapping almost 1:1 onto libfprint's
enroll/verify model: `EnrolStart → EnrolAddImage×N → EnrolGetTemplate`,
`IdentifyImage` (verify), `UpdateTemplate` (template adaptation),
`TemplatePack/UnPack` (serialize), `ImageQualityJudge` (the quality gate). The
shared image struct is `{int16 width@0, int16 height@2, void* data@0x10,
uint32 flags@0x18}`.

### 9.3 A byte-level oracle under Wine

Rather than trust the disassembly alone, the DLL was driven directly under Wine
on our captured frames, to serve as a **ground-truth oracle** for the clean-room
reimplementation. The non-obvious parts:

- Every DLL function pointer needs `ms_abi` (winelib is System-V; the DLL is
  Win64) — without it, arguments land in the wrong registers.
- The DLL's inline WPP logger dereferences an uninitialised trace block under
  Wine and corrupts a non-volatile register; patching each logger entry to a bare
  `ret` neutralises it.
- The real blocker was the **image load**: neither enroll nor identify copies the
  caller's image into the matcher — the Windows engine does that upstream. The
  matcher reads from two internally-allocated **68×84** buffers (our 64×80 + a
  2-pixel border). Padding the raw frame to 68×84 and copying it into both
  buffers before each call made the oracle produce real scores.

With that, the oracle gave a clean ground-truth ROC on clearly-different fingers:
genuine frames overlapping the gallery saturate the score (~102); **every
impostor scored 0 → FAR 0 %** — the separation NCC could never produce.

### 9.4 The algorithm, decompiled

The detector and both descriptor kernels and the graph matcher were decompiled to
a clean-room spec:

- **Detector** (`generateFeatureEx`): a **12-level scale pyramid** (geometric
  ratio 1.2), Gaussian-smoothed per level, Harris-style keypoints into a 28-byte
  struct, deduplicated, top **200** by response. Each keypoint carries
  `(orientation, x, y)` plus a patch index.
- **s-descriptor**: **classic SIFT-128** — 4×4 spatial grid × 8 orientation bins,
  oriented sampling, trilinear interpolation, two-threshold (0.7 keep / 0.4
  clamp) block normalization. 512 bytes/keypoint.
- **u-descriptor**: a coarse **4×4×5 = 80-float** regional companion capturing
  low-frequency ridge flow. 320 bytes/keypoint.
- **Matcher** (`_fingerIdentify` → `createUndirectGraph`): descriptor
  nearest-neighbour matches form keypoint **pairs**; a graph-consistency check
  (Floyd-Warshall over a matched-pair distance matrix) plus translation/RANSAC
  inlier counting; **accept iff geometric inliers ≥ 5**.

So the descriptor is **SIFT-128 (+ a coarse 80-D companion)** and the verify is
**NN + Lowe-ratio + geometric inlier ≥ 5 with graph consistency** — all public,
well-understood, patent-clean building blocks (SIFT's patent expired ~2020;
Harris is 1988; RANSAC is 1981).

---

## 10. The clean-room matcher (what ships)

`lib/petaic_sift.c` is a from-scratch, pure-C (libm only) reimplementation of the
recovered design — **no DLL at runtime**, upstreamable to libfprint:

- multi-scale **Harris** detection (NMS, ≤200 keypoints),
- 36-bin orientation assignment,
- **canonical SIFT-128** descriptor (4×4×8, trilinear, unit-norm + 0.2 clamp),
- nearest-neighbour + **Lowe ratio** matching,
- geometric **pairwise distance + orientation consistency (greedy max-clique)**
  verification — this mirrors the DLL's `createUndirectGraph` and proved more
  robust to press deformation than plain translation-RANSAC,
- the **inlier count is the score**; accept at **≥ 5** (the DLL's exact rule).

The DLL oracle was the validation reference throughout: the clean-room matcher
was tuned until its score separation matched the oracle's on the captured frame
sets.

A clean-room analogue of the quality gate (`ImageQualityJudge`) also lives here
as `ps_quality()` — mean **orientation-tensor coherence** over textured blocks —
which cleanly separates weak presses from firm ones.

The offline tuning harnesses are `tools/petaic_sift_roc` (SIFT matcher) and
`tools/petaic_roc` (the legacy NCC matcher), both operating on PGM sets with no
hardware needed.

---

## 11. Accuracy: the sensor ceiling, honestly

On a realistic same-finger dataset (15 separate genuine presses, core centred,
lift between each) versus 10 clearly-different impostor presses:

| Matcher                              | FRR (genuine rejected) | FAR (impostor accepted) |
| ------------------------------------ | ---------------------- | ----------------------- |
| Windows DLL, its own decision        | 29 %                   | **40 %** (permissive)   |
| Windows DLL, thresholded for FAR 0 % | 43 %                   | 0 %                     |
| **`petaic_sift`, threshold 5**       | **29 %**               | **0 %**                 |

The decisive finding: **the production Windows matcher is also marginal on
separate presses** — thresholded for FAR 0 %, our clean-room matcher is
comparable to, or slightly ahead of, the original. **~30–40 % single-frame FRR is
the real sensor ceiling**, set by the 64×80 patch size and press variability, not
by the reimplementation. Geometry tuning is exhausted at that point; further
gains have to come from the _capture_ side.

---

## 12. Capture-side accuracy: getting to daily-driver usable

Three levers, in priority order, push the effective FRR down:

1. **Quality / coverage gate** (`ps_quality`, threshold ~0.52). Turns weak
   presses into a bounded "lift & retry" instead of a hard reject. Offline this
   moved FRR from 13 % to ~0 % on the clean set (it gates _capture quality_, not
   identity — the matcher still rejects impostors independently). Wired into the
   driver's capture path with a small re-press budget.
2. **Enroll-diversity gate.** The big early win for real-world margin. Two coupled
   changes spread the gallery across the pad: a **lift-between-stages** wait (each
   stored frame is a fresh press, not a burst from one continuous touch) and a
   **diversity reject** (drop a new frame that is >0.95 NCC-similar to a kept
   one). This pushed live verify scores well clear of threshold.
3. **Multi-frame verify** (default 3 frames/touch, take the best inlier count,
   early-exit on the first accept). Live-tested: **FAR stays 0 %**, FRR clearly
   improved, accepts return on frame 1. But the gain is **marginal** — frames
   within one continuous touch are nearly identical, so the residual rejects are
   _off-region_ presses that miss gallery coverage entirely.

**Template adaptation** (the DLL's `learn=1` / `UpdateTemplate` — fold good
verify frames into the gallery) is the real remaining FRR lever, but it is
**deferred**: stock `fprintd` 1.94 only persists prints on enroll, never on
verify, so any in-memory gallery growth is discarded on the next login. Closing
this would need a driver-owned sidecar gallery or an enroll-update path; the
trade-offs are documented in the code comments.

---

## 13. How the RE maps onto the shipped stack

Each layer is as thin as it can be, and each encodes a specific block of the
findings above:

```
  GNOME/KDE Settings, PAM (sudo/login)
            │  D-Bus
          fprintd                              §12 (persistence limits)
            │
        libfprint ── fprint-driver/sil6250.c   §8.1 FpDevice + FPI_PRINT_RAW
            │                  │ links
            │            ┌─────┴────────────────────────────────┐
            │            │  lib/  (libsil6250)                   │
            │            │  • petaic_proto   mailbox framing  §4 │
            │            │  • petaic_transport  EC mailbox    §3 │
            │            │  • petaic_engine  TLS-PSK+capture §5,6│
            │            │  • petaic_match   destripe + NCC §7,8 │
            │            │  • petaic_sift    clean-room SIFT §10  │
            │            └─────┬────────────────────────────────┘
            │                  │ /dev/sil6250  (mmap + ioctl)
            │            kernel/sil6250.ko   resource broker  §2,3
            │                  │ ACPI platform device "SIL6250"
        hardware ─────────────┘  mailbox window + GPIO strobes + IRQ
```

- **`kernel/`** — the EC mailbox transport contract (§2–§3), and _only_ that: no
  framing, no crypto, no matching.
- **`lib/`** — framing (§4), the EC transaction (§3.3), the TLS-PSK channel (§5),
  the capture loop with all its traps (§6), the destripe (§7), and both the
  legacy NCC (§8.2) and the shipped clean-room SIFT (§10) matchers.
- **`tools/`** — the bring-up and validation harnesses (`petaic_record` §3.3,
  `petaic_tls` §5, `petaic_capture`/`qlive` §6, `petaic_roc`/`petaic_sift_roc`
  §10–§11).
- **`fprint-driver/`** — the `FpDevice` adapter and the libfprint overlay (§8.1).

---

## 14. Methodology and tooling

- **ACPI**: DSDT/SSDT decompilation to find the device, its HID-switch logic, and
  its `_CRS` (§1–§2).
- **A verified sibling driver**: the open-source Goodix GXFP5130 driver
  ([Void755/gxfp_linux_driver](https://github.com/Void755/gxfp_linux_driver))
  for the same window/GPIOs gave a _compiling, ACK-getting_ reference for the
  platform transport (§3.2) — the single biggest de-risking input.
- **Static RE** of the correct Windows driver (disassembly + decompilation) for
  the EC arbitration model, framing, and command set (§3.3–§4).
- **A live WPP/ETW trace** of the Windows driver during bring-up + enrollment
  (~90 k decoded lines) to confirm framing, the command sequence, the TLS flow,
  and the host-side pipeline **byte-for-byte** (§4–§7).
- **Windows secret extraction** (DPAPI-NG unseal, cross-checked against hardcoded
  constants in the service binary) for the PSK ring (§5).
- **A Wine-hosted oracle** of the matcher DLL — driven directly on our own
  captured frames — as the ground-truth reference for the clean-room matcher
  (§9.3).
- **Decompilation** of the detector, both descriptor kernels, and the graph
  matcher into a clean-room spec (§9.4), then a from-scratch reimplementation
  validated against the oracle (§10).

A recurring discipline throughout: **prove negatives** (no firmware calibration,
no usable minutiae, no real EC-register command) and **validate on realistic
data** (separate presses and a genuinely different second finger, never frozen
frames) — both saved the project from confidently shipping something that only
worked in the lab.

---

## 15. Open items and known limits

- **~30 % single-frame FRR is the sensor ceiling** — the original Windows matcher
  hits it too. Daily usability comes from the capture-side gates (§12), not from
  pushing the matcher further.
- **Template adaptation is deferred** — no persistence path through stock
  `fprintd` 1.94 (§12).
- **Discovery** is via the `FP_SIL6250` environment variable (a systemd drop-in
  exports it to `fprintd`), because the mailbox node is neither `hidraw` nor
  `spidev` and libfprint's udev backend can't enumerate it. Full udev
  auto-discovery would need a libfprint-core patch and is deferred to keep the
  forked surface to a single adapter file.
- **The richer dual descriptor** (the matcher's coarse 80-D companion, plus its
  `median_quantizationLocal` preprocessing) is recovered but not yet ported; it
  is the highest-effort remaining lever if the capture-side gates ever prove
  insufficient.
- **PSK rotation risk**: the four keys are constants in this driver build and are
  identical across units, but a future firmware/driver revision could ship a
  different ring (cheap to re-extract if so).
