# lowgui

A clean-room reimplementation of the OpenCV `highgui` module image viewer on top of Plan-V4D.
It maps window names to Plan-instances and feeds them images through a `SinkSource` implementation.

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)
[![C++20](https://img.shields.io/badge/C++20-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B20)
[![OpenCV](https://img.shields.io/badge/OpenCV-5.x-orange.svg)](https://opencv.org/)

## Features

- `cv::lowgui::Lowgui::namedWindow` — create a named viewer window
- `cv::lowgui::Lowgui::imshow` — push an image to a named window; handles the same
  depth/channel conversions as highgui (1/3/4 channels; 8U plus 8S/16U/16S/32F/64F;
  unsupported input is skipped with a logged warning)
- `cv::lowgui::Lowgui::waitKey` / `waitKeyEx` / `pollKey` — **key delivery is
  implemented**: ASCII codes for letters/digits/symbols, X11 keysyms for
  arrows/Home/End/PageUp/PageDown/Insert/Delete/F1–F12/numpad. Modifier keys are
  tracked for mouse-callback flags but never delivered as key codes, and
  Ctrl-prefixed combinations are suppressed (ImGui/Qt shortcuts handle them)
- `cv::lowgui::Lowgui::setMouseCallback` — MOVE / L/R/M DOWN / L/R/M UP /
  L/R/M DBLCLK / MOUSEWHEEL / MOUSEHWHEEL with Qt-style `flags`
  (held buttons + Shift/Ctrl/Alt) and image-space coordinates
- `cv::lowgui::Lowgui::getMouseWheelDelta` — recover the scroll delta packed into
  a wheel event's flags (multiple of 120 per notch, signed)
- `createTrackbar` + get/set pos/min/max — per-window and control-panel
  registries, Qt name+window dedup, callbacks fire on value change
- `createButton` / `setButtonState` / `getControlButtons` — Qt push buttons,
  checkboxes and radioboxes (with `QT_NEW_BUTTONBAR` grouping), states `-1/0/1`
- `setWindowProperty` / `getWindowProperty` — FULLSCREEN, AUTOSIZE,
  ASPECT_RATIO, VISIBLE (OPENGL / TOPMOST / VSYNC are accepted no-ops)
- `getWindowImageRect` — the on-screen rect of a window's image cell
- `displayOverlay` / `displayStatusBar` — transient overlay / status messages
- Qt-style interactive viewer per window — scroll-wheel zoom around the cursor,
  left-drag pan, middle-click deep zoom, right-click context menu with Qt's 11
  actions (pan, zoom x1/x30/in/out, save image, copy to clipboard via `xclip`,
  toggle the properties/control panel), status bar with pixel readout, deep-zoom
  RGB overlays, open/save dialogs, help overlay, full-screen toggle
- NanoVG-based rendering with fit-to-viewport scaling, multiple windows arranged
  in a grid layout, and a headless/offscreen render path backed by
  `readFramebuffer()`

## Architecture

The implementation consists of:

1. **WindowManager** (`window_manager.hpp`) — singleton that manages window data
   (name, flags, title, viewport, image buffer, mouse callback, per-window
   trackbars, properties, transient messages, plus the global control-panel
   trackbars/buttons). Thread-safe with mutex and condition variable. Windows are
   owned via `shared_ptr` so concurrent accessors never dangle.

2. **LowguiRootPlan** (`src/lowgui_root_plan.hpp`) — the root Plan-V4D plan behind
   the long-lived render engine. Each frame it re-reads the current window set
   from the WindowManager, arranges the windows in a grid, and draws each one
   through NanoVG (content is only re-encoded and re-uploaded when a window's
   image changes). It dispatches the per-window mouse/key edges (worker side),
   owns the offscreen framebuffer snapshot mechanism used by `readFramebuffer()`,
   and runs the ImGui menu bar / dialogs / context menu (display side).

3. **SinkSource** (`sink_source.hpp`) — a thread-safe buffer that can be pushed
   images to from external threads. The primary image path is the WindowManager's
   per-window image buffers; `SinkSource` is provided as an extension point.

4. **Lowgui API** (`lowgui.hpp` / `src/lowgui.cpp`) — public API mimicking OpenCV's
   highgui functions. `waitKey` starts (once per process) a long-lived
   render/event-loop engine and coordinates framebuffer snapshots and key waits
   against it. Key codes are mapped by `src/lowgui_input.hpp`.

## Building

```bash
./build.sh -t plan+v4d+lowgui -b debug
```

Binaries land under `opencv/build/bin`, **not** `./bin`.

## Testing

### Local tests (non-rendering)

```bash
./build.sh -t plan+v4d+lowgui -b debug
./opencv/build/bin/opencv_test_lowgui --gtest_filter=-*Rendering*
```

The `waitKey`-based API tests use zero windows and never start the real-display
engine, so they run without a display server (CI wraps them in Xvfb).

### Local tests (offscreen rendering)

No display server required when Mesa llvmpipe is available:

```bash
LIBGL_ALWAYS_SOFTWARE=1 ./opencv/build/bin/opencv_test_lowgui_offscreen
```

Or under Xvfb:

```bash
xvfb-run -a ./opencv/build/bin/opencv_test_lowgui_offscreen
```

The offscreen suite forces the offscreen engine, renders real images through the
headless framebuffer snapshot path, and asserts on the captured pixels plus the
`waitKey(0) == -1` / `readFramebuffer()` semantics. Tests inject synthetic
keyboard/mouse events through `gwe::push(...)` so the input plumbing can be
verified without a windowing system.

### Local tests (rendering, legacy)

```bash
xvfb-run -a ./opencv/build/bin/opencv_test_lowgui --gtest_filter=*Rendering*
```

### Full system tests (QEMU)

All tests, including rendering, run inside a QEMU VM for full isolation. The VM
uses a Debian 12 rootfs with Mesa llvmpipe for software OpenGL.

```bash
# 1. Build the VM image (requires sudo)
sudo ./scripts/build-vm-image.sh

# 2. Run tests inside QEMU
./scripts/vm-boot-and-test.sh \
  --image lowgui-test.img \
  --kernel vmlinuz \
  --initrd initrd.img \
  --repo-path /home/elchaschab/devel \
  --ssh-key vm-key \
  --out-dir /tmp/qemu-out
```

The `run-tests-in-vm.sh` script inside the VM mounts the source tree via
virtio-9p, builds with `build.sh -t plan+v4d+lowgui`, and executes both
`opencv_test_lowgui` and `opencv_test_lowgui_offscreen`. JUnit XML results are
copied to the output mount.

## Qt parity and deviations

- Key codes match Qt's X11 values for arrows/special keys/F-keys and the numpad.
  One deliberate deviation: `KP_Add`/`KP_Subtract` use the values from the
  lowgui spec (65453/65451), which are swapped vs. real X11 — keep them matching
  the spec.
- Ctrl key combinations never reach `waitKey`/`waitKeyEx`: ImGui consumes them
  for shortcuts (pan, zoom, deep zoom, fit, open/save, copy, etc.), mirroring Qt.
- ImGui widgets (menu bar, dialogs, panels) capture mouse/keyboard input they
  own; they are invisible to the offscreen `readFramebuffer()` capture path.
- All windows share a single native (GLFW) window; full-screen applies to the
  whole native window, not per viewer cell.
- Windows created with `WINDOW_GUI_NORMAL` get no context menu. The menu bar /
  status bar / help overlay suppression for all-GUI_NORMAL layouts is not yet
  wired (in progress).
- In offscreen/headless mode input only arrives through `gwe::push(...)`;
  there is no keyboard source.

## Known limitations

- There is exactly one native (GLFW) window hosting the viewer grid, and the
  V4D render loop can only be started once per process. Closing the native
  window terminates the engine; after that `waitKey` returns `-1` (promptly or
  after the delay) until the process exits. `destroyWindow`/`destroyAllWindows`
  without closing the native window are safe and windows can be re-created.
- `imshow` of an image with an unsupported depth (e.g. `CV_32S`) or channel
  count (not 1/3/4) is skipped for that window with a logged warning rather than
  terminating the render engine.
- The "Save view as…" dialog currently saves the window's *source* image; the
  composited-viewport capture, the mini-map, and transient status-bar messages
  are in progress.

## CI

GitHub Actions runs the QEMU workflow and an xvfb workflow on push/PR to
`main`/`master`. The VM image is cached and rebuilt only when provisioning
scripts change.

## Sample

```cpp
#include <opencv2/lowgui/lowgui.hpp>
#include <opencv2/imgcodecs.hpp>

static void onMouse(int event, int x, int y, int flags, void*) {
    // See EVENT_* / EVENT_FLAG_* in lowgui.hpp (Qt-compatible).
}

int main(int argc, char** argv) {
    std::string imgFile = argc > 1 ? argv[1] : cv::samples::findFile("lena.png");
    cv::Mat img = cv::imread(imgFile);
    if (img.empty()) return 1;

    cv::lowgui::Lowgui::namedWindow("demo");
    cv::lowgui::Lowgui::setMouseCallback("demo", onMouse);
    cv::lowgui::Lowgui::imshow("demo", img);
    std::cout << "Arrow keys pan, +/- zoom, Esc closes, Ctrl+S saves.\n";
    cv::lowgui::Lowgui::waitKey(0);

    cv::lowgui::Lowgui::destroyAllWindows();
    return 0;
}
```

A larger interactive demo lives in `samples/lowgui_demo.cpp`.

## License

Apache 2.0