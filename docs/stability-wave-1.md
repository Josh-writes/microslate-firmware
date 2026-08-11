# Stability Wave 1 — reconnect + reboot hardening

Targets the two reported symptoms: **BLE keyboard drops / won't reconnect** and
**random reboots / freezes**. Note-level data-integrity bugs (oversized-note truncation,
the missing crash-recovery the README advertises, silent save failures) are real but
deferred to a later wave — they aren't the current pain.

## Diagnosis

| Symptom | Root cause found |
|---------|------------------|
| Drops / won't reconnect | `bleConnectTask` ran `NimBLEDevice::deleteBond(addr)` on **every** connect attempt, forcing a full re-pair each time. A keyboard that dropped mid-session often couldn't come back. |
| Won't reconnect (hard) | Auto-reconnect is gated on `connectTaskHandle == nullptr`. A wedged connect task never clears it → device never retries until a manual reboot. |
| Random freezes | No watchdog anywhere. Any hang (BLE host stall, display BUSY pin, SD wedge) froze the device until the manual 5s-BACK restart. |
| Random reboots | `bleState` is shared across the BLE host task, callbacks, and main loop but was non-`volatile` → cached/missed transitions and race-driven reboots. |

## Changes

1. **Task watchdog** (`main.cpp`) — 30s TWDT on the loop task, fed each iteration,
   released before deep-sleep prep. A hang becomes a clean ~30s reboot. 30s sits above
   the longest loop-task blocking op (the 10s e-ink BUSY waits); the BLE connect runs on
   its own unwatched task. `idle_core_mask = 0` so automatic light sleep can't false-trip it.
2. **Connect-task deadlock recovery** (`ble_keyboard.cpp`) — timestamp each connect task;
   if it runs >30s, assume the NimBLE host wedged, release the gate, and drop back to
   `DISCONNECTED` so auto-reconnect resumes. Best-effort (the task self-deletes when it
   unwinds); the main loop keeps running throughout.
3. **`bleState` / `connectTaskHandle` → `volatile`** — stop the compiler caching
   cross-task shared state.
4. **Bond churn fix** (`ble_keyboard.cpp`) — clear stale cross-session bonds **once at
   boot** instead of before every connect. The first connect establishes a bond that then
   persists, so in-session reconnects are fast encrypted reconnects with no re-pairing.
   Keeps the documented protection against the post-unclean-shutdown security-state crash.
5. **Fork CI** (`.github/workflows/build.yml`) — builds the merged `.bin` as an artifact
   on push / PR / manual run. No tags, no release, no website push, no secrets.

## How to test (macOS, no toolchain)

1. Push this branch to the fork → the **Build (CI)** workflow runs.
2. Download the `microslate-firmware` artifact, unzip.
3. Flash `microslate-ci.bin` at offset `0x0` via the web flasher
   (<https://espressif.github.io/esptool-js/>) in Chrome.
4. Rollback anytime: reinstall the official build at typeslate.com/tools/microslate.

## Falsification criteria (review after ~1 week of daily use)

This wave is a **success** only if, vs. the current official build:
- In-session keyboard drops reconnect on their own (no manual re-pair), **and**
- Freezes that needed the 5s-BACK restart are gone or self-recover within ~30s.

If drops still need a manual re-pair, change #4's assumption is wrong (the keyboard isn't
honoring the persisted bond) — escalate to per-keyboard bonded-reconnect-with-fallback.
If reboots increase, suspect the watchdog (#1) false-tripping during light sleep — revisit
`idle_core_mask` / timeout before anything else.
