# HANDOFF — lowgui code-audit remediation (fresh session)

**Status:** Mid-flight on `.kilo/plans/1790084650000-code-audit-remediation.md`.
§1 and §2 are **done**; §5, §4.1, §6 are **implemented but NOT compiled or
tested**; §3 is **partially implemented and definitely NOT compiled**; §7 is
**not started**. Treat every code change below as *unverified* until you run the
gate in "VERIFY FIRST" below.

The previous debug build (`opencv/build`, `.build-type=debug`) is **stale** — it
predates all of today's edits. Rebuild from scratch before trusting anything.

---

## VERIFY FIRST (do this now)

```bash
./build.sh -t plan+v4d+lowgui -b debug -j 16
./opencv/build/bin/opencv_test_lowgui --gtest_filter=-*Rendering*
xvfb-run -a ./opencv/build/bin/opencv_test_lowgui_offscreen        # ×10 clean exit (139 is a crash)
./build.sh -t plan+v4d+lowgui -b asan -j 16
xvfb-run -a ./opencv/build/bin/opencv_test_lowgui_offscreen        # ASan clean ×3
```

- Binaries land in `opencv/build/bin`, NOT `./bin`.
- Expect compile errors/failures: §3's ImGui/control-panel code is the highest
  risk (ImGui is v1.89.9; `detail::Button` shadows `ImGui::Button` — qualify).
- Another process may re-run `./build.sh -b asan` and wipe `opencv/build`; if a
  binary vanishes, re-run the debug build.

---

## Session diff at a glance

`git status --short` (all uncommitted):

- ` M` .github/workflows/offscreen-test.yml
- ` M` README.md, samples/lowgui_demo.cpp
- ` M` scripts/build-vm-image.sh, scripts/run-tests-in-vm.sh
- ` M` src/lowgui.cpp, src/lowgui_root_plan.hpp (biggest), src/window_manager.cpp
- ` M` include/opencv2/lowgui/{lowgui,sink_source,window_manager}.hpp
- `??` LICENSE, `??` src/lowgui_input.hpp  ← **untracked; must be committed or CI breaks**
- Submodule clean/pinned: `5722d344b1b6fd9846bcf7f0d799444e4696b1f6` (heads/beta-5.x)

---

## Done (verify still holds)

### §1 CI fix (H1+L9) — files written, NOT run through actionlint/shellcheck
- `.github/workflows/offscreen-test.yml`:
  - `actions/checkout@v4` now `submodules: recursive` (xvfb + qemu jobs).
  - Deleted the `git clone ../opencv` / `../Plan-V4D` steps (build.sh bootstraps).
  - Test `run:` paths now `./opencv/build/bin/opencv_test_lowgui[_offscreen]`,
    wrapped in `timeout 300s` (ties §6.3 L7 hang guard).
  - New `lint` job: `shellcheck -S error scripts/*.sh build.sh`.
  - Cache key now includes `scripts/run-tests-in-vm.sh` (it is baked into the
    image in build-vm-image.sh:85-86; previously a stale image masked script fixes).
  - §2.4 guard: xvfb job greps for "intentionally leaked" in the pinned submodule.
- `scripts/run-tests-in-vm.sh`: restored **both** 9p mounts (`repo`, `out`) —
  the plan only mentioned `out`, but `repo` is load-bearing (`cd /workspace/lowgui`
  would be empty without it). `mkdir -p /out` before mounting. Dropped the
  `../opencv` binary lookups → `./opencv/build/bin` (+`./opencv/build` fallback).
  Removed `|| true` on the two XML copies (fail loudly); dropped the speculative
  `testing.xml`/`opencv_test_lowgui.xml` copies.

### §2 submodule durability (H2/M1) — mostly already true
- The plan's premise ("fixes not pushed") was **stale**: submodule is clean,
  synced with `origin/beta-5.x`, and both fixes are already in `plan.hpp`
  (`// intentionally leaked` :60, worker join :1264-1273). Nothing to push/pin.
- M1 decision: **keep the documented intentional leak** (one worker thread; the
  submodule lives in a repo we're not pushing mid-task). The `grep` guard in CI
  (§1) preserves the fix on fresh clones.

### §5 hygiene (H3/M6/L4/L5) — files written, unverified
- Clipboard (H3): `copyWindowToClipboard` now `cv::imencode`s to memory and
  pipes PNG bytes to `xclip -selection clipboard -t image/png` via `popen("w")`
  + `fwrite` + `pclose` exit-status check. No temp file, no symlink race, honest
  `lastImageSaveMsg` on every failure.
- License (M6): `LICENSE` added (Apache-2.0 text copied from `opencv/LICENSE`);
  SPDX header (`Apache-2.0` + copyright + clean-room note) added to the top of
  `src/*` (lowgui.cpp, lowgui_input.hpp, window_manager.cpp,
  lowgui_root_plan.hpp) and `include/opencv2/lowgui/*.hpp` (lowgui.hpp,
  window_manager.hpp, sink_source.hpp). README "License: Apache 2.0" now matches.
- VM artifacts (L4): `build-vm-image.sh:100-101` → `chmod 600 vm-key` and
  `chmod 600 IMG`, with a `SUDO_USER` chown guard so actions/cache (running as
  the non-root runner) can still read them (the old 666 existed for exactly that).
- Locking (L4): `pushImage` now pins the `WindowData` via `getWindowShared`
  (brief `mtx_` hold) then pushes under `sink->mtx` only — a big CPU/GPU copy no
  longer stalls unrelated window ops on the WindowManager mutex.
- Sample/docs (L5): demo rewritten to a single HSV rainbow gradient + bounded
  64-frame pump (was `255*255` frame loop with `++r` overflowing). README sample
  fixed: includes `<opencv2/core/utility.hpp>` for `cv::samples::findFile` (that
  is its real header — verified in `opencv/modules/core/include/.../utility.hpp`),
  dropped the unused `onMouse` forward-decl + `setMouseCallback`.

### §4.1 input (L1) — one-line fix only
- `keyToCode`: `K::KP_EQUAL` → `65469` (was `65461`, colliding with `KP_5`).

### §6 robustness (M5/M7/L7) — files written, unverified
- `waitKeyImpl` (lowgui.cpp):
  - **Offscreen path decoupled (M7):** polls `keyQueue()` FIRST; capture
    sequencing only runs when no key is pending, so a churning generation (settled
    capture may time out) cannot starve key delivery.
  - Engine-death warning (M5): first post-death call logs a `CV_LOG_WARNING`
    explaining waitKey* now returns -1 (V4D loop is one-shot). Warned once via
    a static flag.
  - Capture-timeout warning only on the blocking (`delay==0`) caller, so a timed
    poll under churn doesn't spam logs.
- `windowCount()==0` early return kept with an explicit load-bearing comment
  (L7); CI already has the `timeout 300s` safety net (§1).
- **§6 ordering note:** I ran §6 before §5's §7 tests; plan order was §5→§4→§6.
  §6 depends on nothing so this is fine.

---

## In-flight / partially done — needs compile + tests

### §3 features (M2/M3/M4/L6) — BIGGEST RISK, NOT COMPILED
Layout/render:
- `computeLayout`: skips `propVisible==0` windows entirely (no grid slot → empty
  `getWindowImageRect`); AUTOSIZE cells sized to image clamped+centered in grid
  slot; USERVIEWPORT unchanged. (Reads props under `sink->mtx`.)
- `ViewState` gained: `keepRatio` (per-frame from `wd->propKeepRatio`),
  `statusText`/`overlayText` (per-frame from TransientMsg, auto-expiring).
- `renderImage`: FREERATIO = non-uniform stretch-fill (`zoom*cell/image` per
  axis); grid + `drawDeepZoomOverlay` guarded to KEEPRATIO only.
- `fitStretch` added; initial-fit-on-new-content and Ctrl+F now branch on
  keepRatio.
- FULLSCREEN (M2): `drawWindows` aggregates `winFullscreen` across visible
  windows and calls `V4D::set(Keys::FULLSCREEN, fs)` on the worker nvg node only
  on transitions (`lastFullscreen_` member). `setWindowProperty`'s dead
  `notifySettledFrame` special-case removed.
- L6: `destroyAllWindows` now clears `controlTrackbars_`, `controlButtons_`, and
  resets `nextBarId_`.
Messages (M3):
- `renderStatusBar` appends `st.statusText`; new `renderOverlayMessage` draws
  `st.overlayText` centered in the cell (also on the headless path so captures
  can assert it).
Control panel (M4):
- New `drawTrackbarStrip(winname, vp)` — per-window SliderInt strip above the
  28px status bar; collapses to a "Trackbars..." popup trigger when
  `vp.width < 240`; drives `updateTrackbarPos(name, win, pos)`. **NOT YET
  CALLED** from guiBody (see TODO below).
- New `drawControlPanel()` — "lowgui settings" window over
  `getControlTrackbars()` + `getControlButtons()` with Qt semantics:
  push→`cb(-1,ud)`, checkbox→0/1, radiobox→exclusive within `barId`.
  **NOT YET CALLED** from guiBody (see TODO below).
Shortcuts/labels (§4.2 L2, partially done):
- Ctrl+P now toggles `showControlPanel`; Ctrl+Z added for reset-zoom; View menu
  "Zoom x1" → "Ctrl+0 | Ctrl+Z"; new "Display control panel" (Ctrl+P) menu item
  gated on `hasControlPanelContent()`. Context-menu label still "Ctrl+0" (plan
  wants "Ctrl+0|Ctrl+Z") — **TODO**.

## §7 tests (L1/L3/L6/L8) — NOT STARTED
- `test/test_lowgui_input.cpp` (add to `OPENCV_TEST_lowgui_SOURCES` at
  `CMakeLists.txt:37-41`): keyToCode table (incl. KP_EQUAL=65469 fixes),
  trackbar registry (dedupe, pos sync, callback-on-change), `destroyAllWindows`
  control-registry reset, `setButtonState` sentinel ambiguity at
  `window_manager.hpp:198-201`, readFramebuffer settled-generation stress.
- Dead-API flags (L8): `popImage`, `getImage`, `running_`, `waitForWindow`,
  `notifyAll` (window_manager.{hpp,cpp}) — give callers or mark `CV_UNUSED`.

---

## TODO list (fresh session)

1. **Compile everything now** (VERIFY FIRST). Fix whatever breaks — expect
   issues in the new ImGui code (§3) first.
2. Call `drawTrackbarStrip` for every window's cell and `drawControlPanel` from
   `guiBody` when `st.showControlPanel` is set (port the drawing into the
   correct spot — I added the helpers but have not wired the invocation).
3. Finish §4.2 label sweep: help overlay (:1509-ish) and properties dialog
   (:1483-ish) still say "Ctrl+0 / Ctrl+P: reset zoom" → change to
   "Ctrl+0 / Ctrl+Z"; help overlay "Right click: reset zoom" → context menu.
   Context-menu "Ctrl+0" → "Ctrl+0 | Ctrl+Z". Grep for "Ctrl+P" after.
4. §7 tests (list above) + add test to CMakeLists.
5. Re-run the full gate (debug 36/36 + offscreen 6/6 ×10 + ASan ×3).
6. **Commit**: `src/lowgui_input.hpp` and `LICENSE` are untracked — a fresh CI
   checkout cannot build without committing `lowgui_input.hpp`. Also `git rm`.
   — nothing tracked? `HANDOFF.md` itself is untracked (user deleted the old one;
   gitignore? `.gitignore:15` ignores `opencv/` only). Decide whether to commit.
7. Optionally validate workflow with `actionlint` / run `shellcheck -S error`
   locally before trusting the new `lint` job.

---

## Environment facts (keep — from the earlier handoff)

- Engine: V4D loop is process-global one-shot; `startEngine` once per process on
  first waitKey. `gEngineLoopAlive`, atexit join in `lowgui.cpp:102-116`.
- `windowCount()==0` early return in `waitKeyImpl` is load-bearing for the
  main-unit binary (zero windows → engine never starts). Do not remove.
- Offscreen binary forces `LOWGUI_FORCE_OFFSCREEN`; `renderHeadless` toggles
  `LOWGUI_HEADLESS_RENDER` for capture sync. Tests inject input via `gwe::push`.
- Node order (worker): reconcile → handleKeys → handleInput → nvg draw →
  capture. Modifier/button statics are worker-thread-only.
- `V4D::set(KEY,val)` is the direct property setter (fires the registered
  framebuffer callback); `V4DPlan::set(key, edge)` is the infer()-only
  transaction. FULLSCREEN is `create<false,bool>` + `setFullscreen` callback.
- ImGui is v1.89.9; `detail::Button` shadows `ImGui::Button` — qualify.
- `wm->sink->mtx` guards viewport/title/props/trackbars/messages/images;
  WindowManager `mtx_` guards the window map + control panel registry.
- Tests: main binary never starts the engine (no win) — 36/36 expected; offscreen
  6/6. The plan's DoD closes H1-H3, M1-M8, L1-L9.