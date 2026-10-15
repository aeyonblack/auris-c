# Auris power budget: what was done on 15 October 2026

**Goal:** make the imager draw less power so it runs from a power bank, without lowering the frame rate or the quality.

**Branch:** `perf/power-budget` in `~/src/auris-c-power` on the BeagleBone. This is a separate git worktree of the auris-c repository; your working folder `~/src/auris-c` (branch `feat/cmb-config`, with its uncommitted changes) was not touched. The imager service still runs your existing build until you switch it (see "Trying it and switching over").

## Summary

| Measure (same recording replayed as the audio input, 40 s) | Before | After |
|---|---|---|
| Imager CPU | 89.9% of one core | 64.3% of one core |
| Whole board busy (both cores) | 45.6–48.2% | 33.6–33.8% |
| Audio processing per active 10.7 ms step | 6.0 ms | 2.0 ms |
| Display packing per frame | 5.7 ms | 1.7 ms |
| Memory in use after boot | 266 MB | 116 MB |

The imager now uses about 28% less CPU, with no lower frame rate and the same localisation results. Power draw was **not** measured: that needs the USB meter between the power bank and the cable.

## Where the CPU time went before

Measured on the board with the imager running at 20 frames per second (the `fps 20` in `configs/runtime_settings.txt`):

| Job | Cost | Share of one core |
|---|---|---|
| Camera image processing (raw sensor image to colour picture), in software | 23 ms per frame | about 46%, always |
| Audio processing (SRP-PHAT), while there is sound | 6.0 ms per 10.7 ms step | up to about 56% |
| Display packing (scale, rotate, convert the picture) | 5.7 ms per frame | about 11% |
| Background services (Node-RED, VS Code server, Docker) | busy only at boot | under 1% afterwards |

Also found: this kernel (5.10.168-ti) has no CPU frequency control, so the cores always run at full speed and "capping the CPU" is not available. Power can only drop by giving the CPU less work.

## 1. Background services turned off

**What:** disabled and stopped the services the imager does not use:

```bash
sudo systemctl disable --now nodered.service bb-code-server.service docker.socket docker.service containerd.service
```

- `nodered`: Node-RED, the browser-based wiring tool from the BeagleBoard image
- `bb-code-server`: the VS Code server on port 3000
- `docker.socket`, `docker`, `containerd`: Docker, which had no containers

**Effect:** memory in use dropped from 266 MB to 116 MB. Steady-state CPU barely changed: once booted, Node-RED had used 4 s of CPU in 12 minutes. The saving is mostly during boot and in memory.

**To undo:** `sudo systemctl enable --now` with the same five names.

## 2. Display: let the display hardware do the scaling

**Before:** every frame, the CPU scaled the 640×360 picture up to the panel's full 720×1280, rotated it, and wrote all 3.7 MB into display memory. Display memory is uncached, so writing to it is slow: that was the 5.7 ms.

**After:** the CPU writes the picture at its own size after rotation, 360×640, which is a quarter of the pixels. The display controller has scalers in some of its pipelines; one of them scales the picture to 720×1280 on the way to the panel, at no CPU cost. The normal full-screen layer stays black underneath.

**Details**
- `frontends/display/src/auris_display.c`:
  - looks for overlay planes that can show this picture format on the panel;
  - tries them in order until one accepts the scaled picture. On this board plane 40 refuses and plane 46 accepts.
  - If none accepts, it falls back to the old full-size CPU path. Startup messages say which path is in use.
- **For comparison:** setting `AURIS_DISPLAY_CPU_SCALE=1` forces the old CPU path.
- `frontends/display/src/auris_display_pack.c`:
  - the fit rectangle is now a shared function, `auris_display_fit`;
  - the packer works out each column's source once per frame instead of once per pixel. That on its own made no measurable difference; the time is in the memory writes.

**Measured:** packing 5.7 ms to 1.7 ms per frame. Imager CPU 71–72% to 64% of a core (with the audio change already in).

**Not checked by me:** how it looks on the screen. The hardware scaler smooths the picture slightly where the old path repeated pixels. If anything looks wrong, start the imager with `AURIS_DISPLAY_CPU_SCALE=1` to compare.

## 3. Audio: compute only what the map uses

**Before:** for every audio step with sound, the SRP-PHAT stage worked out each microphone pair's correlation curve with an inverse FFT of 8192 points. That is 28 pairs × 2 halves of the height band = **56 transforms**. It then read only about 235 points of each curve, the lags that the directions in the map correspond to. Most of the work was thrown away.

**After**
- **Correlation:** a new function `auris_gcc_phat_lags` works out the curve only at the lags that are needed. It sums the frequency bins directly, using a table of sines and cosines made once (`auris_fft_reserve_lags`). It uses the processor's NEON vector instructions.
- **Map search (`auris_srp.c`):**
  - keeps each pair's curve only over its own range of lags;
  - no longer does a division for every lookup;
  - adds up the scores one pair at a time across all map cells, so the data stays in the processor's cache.
  - The order in which values are added is unchanged.

**Results stay the same:** replaying all 18 recordings in `recordings/room1` and `room2` through the old and new versions:
- In 2 to 9 frames per recording, the third decimal place of the sub-cell position differs (about a thousandth of a cell).
- Every direction matches to six decimal places.
- The active/quiet decisions are identical.

**Measured**

| | Before | After |
|---|---|---|
| All 18 recordings, replay time | 81.6 s | 28.1 s (2.9× faster) |
| Per active audio step | 6.0 ms | 2.0 ms |

Files changed: `core/inc/auris_spectrum.h`, `core/src/auris_spectrum.c`, `core/src/auris_srp.c`.

## Tests

Written first, then the changes:
- `tests/test_gcc_lags.c`: the new correlation must match the full inverse FFT at every lag (within 2×10⁻⁶), with the same validity and weight. It covers band-limited and full spectra, with and without gains and per-bin weights.
- `tests/test_srp_reference.c`: map scores against a reference built the plain way from full curves, with and without the height band and the diffuse-field gains. It passed on the old code first, which confirms the reference is right.
- `tests/test_display.c`: added exact-size packing for every rotation, and `auris_display_fit`.

**Result: 41 of 41 tests pass** (Debug build with sanitizers).

## Commits on `perf/power-budget`

| Commit | What |
|---|---|
| `98757dd` | Snapshot of your uncommitted `feat/cmb-config` work, so the branch starts from what was running |
| `045aec0` | The new tests |
| `5e78759` | Test fix: valid frame sizes |
| `83b35da` | Audio: GCC-PHAT only at the lags the map reads |
| `29f67bc` | Display: scaling on a display-controller plane |
| this document | `docs/power-budget-2026-10-15.md` |

## Trying it and switching over

**Run the new version once by hand** (from your folder, so it uses your settings):

```bash
sudo systemctl stop auris
cd ~/src/auris-c && ~/src/auris-c-power/build-release/apps/imager_scope --array cmb --output display --rotation 270 --pan 3 --tilt 3 --diffuse 1
```

Press Ctrl-C to stop it, then `sudo systemctl start auris` to go back.

**Make the service use it:** merge the branch into your working branch, or copy the changed files across, then rebuild `build-release` as usual. The service runs `~/src/auris-c/build-release/apps/imager_scope`.

## Not done, and next steps

- **Power draw not measured.** Put the R90 USB meter between the power bank and the cable, and run the power-bank test with the imager running.
- **The camera is now the largest load:** about 23 ms of software image processing per frame, about 46% of one core. Moving it onto the chip's hardware image processor (the VPAC) is the next big saving.
- **Frame rate:** 20 fps is set in the runtime settings. Lowering it would save more, but was left alone as a trade-off, not an optimisation.
