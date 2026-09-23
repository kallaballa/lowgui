# lowgui

A clean-room reimplementation of the OpenCV `highgui` module image viewer on top of Plan-V4D. It maps window names to Plan instances and feeds them images through a `SinkSource` implementation, giving you a Qt-style interactive viewer without the Qt dependency.

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)
[![C++20](https://img.shields.io/badge/C++20-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B20)
[![OpenCV](https://img.shields.io/badge/OpenCV-5.x-orange.svg)](https://opencv.org/)

## Table of contents

- [Overview](#overview)
- [Features](#features)
- [Building](#building)
- [Usage](#usage)
- [Architecture](#architecture)
- [Testing](#testing)
- [Compatibility & limitations](#compatibility--limitations)
- [License](#license)

## Overview

`lowgui` is a drop-in replacement for OpenCV's highgui image viewing: programs that call `namedWindow` / `imshow` / `waitKey` keep working, but the Qt backend is replaced by a single native (GLFW) window that lays every viewer window out in a grid and renders it with NanoVG through the Plan-V4D runtime. Each logical window keeps its own zoom/pan state, status bar, trackbars and mouse callbacks, so the interactive highgui/Qt experience is preserved.

## Features

- **Drop-in highgui API** — `namedWindow`, `imshow`, `waitKey` / `waitKeyEx` / `pollKey`, `destroyWindow` / `destroyAllWindows`, `hasWindow`, `resizeWindow`, `moveWindow`, `setWindowTitle`, `readFramebuffer`
- **Mouse & keyboard** — `setMouseCallback` (MOVE / DOWN / UP / DBLCLK / WHEEL / HWHEEL with Qt-style `flags`), `getMouseWheelDelta`; key delivery covers ASCII codes plus X11 keysyms for arrows, Home/End, PageUp/PageDown, Insert/Delete, F1–F12 and the numpad
- **Trackbars & buttons** — `createTrackbar` with per-window and global control-panel registries, `get/setTrackbarPos`, `setTrackbarMin/Max`; Qt push buttons, checkboxes and radioboxes via `createButton`
- **Window properties** — `setWindowProperty` / `getWindowProperty` for FULLSCREEN, AUTOSIZE, ASPECT_RATIO and VISIBLE, `getWindowImageRect`, plus transient `displayOverlay` / `displayStatusBar` messages
- **Interactive viewer** — scroll-wheel zoom around the cursor, left-drag pan, middle-click deep zoom, right-click context menu with Qt's 11 actions, status bar with pixel readout, deep-zoom RGB overlays, open/save dialogs, help overlay and full-screen toggle
- **Rendering** — NanoVG with fit-to-viewport scaling, multiple windows arranged in a grid, texture re-upload only when an image changes, and a headless/offscreen path backed by `readFramebuffer()`
- **Image handling** — same depth/channel conversions as highgui (1/3/4 channels; 8U plus 8S/16U/16S/32F/64F); unsupported input is skipped with a logged warning

## Building

```bash
./build.sh -t plan+v4d+lowgui -b debug
```

On first run the script initializes the Plan-V4D submodule and clones the OpenCV tree (SSH first, HTTPS fallback), then configures and builds. Useful options:

- `-b release|debug|relwithdeb|asan|ubsan|tsan` — build configuration
- `-j N` — parallel build jobs
- `-r` — force a fresh cmake configure

Binaries land under `opencv/build/bin`.

## Usage

### Minimal example

```cpp
#include <opencv2/lowgui/lowgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/core/utility.hpp>
#include <iostream>

int main(int argc, char** argv) {
    std::string imgFile = argc > 1 ? argv[1] : cv::samples::findFile("lena.png");
    cv::Mat img = cv::imread(imgFile);
    if (img.empty()) return 1;

    cv::lowgui::Lowgui::namedWindow("demo");
    cv::lowgui::Lowgui::imshow("demo", img);
    std::cout << "Arrow keys pan, +/- zoom, Esc closes, Ctrl+S saves.\n";
    cv::lowgui::Lowgui::waitKey(0);

    cv::lowgui::Lowgui::destroyAllWindows();
    return 0;
}
```

A larger interactive demo lives in `samples/lowgui_demo.cpp`.

### Viewer controls

| Input | Effect |
| --- | --- |
| Scroll wheel | zoom in/out around the cursor |
| Left-drag | pan |
| Middle-click | deep zoom |
| Right-click | Qt context menu (pan, zoom, save, copy, toggle panels, …) |
| `Ctrl` + arrows | pan |
| `Ctrl` + `+` / `-` | zoom in / out |
| `Ctrl` + `0` / `Ctrl` + `Z` | zoom x1 (reset) |
| `Ctrl` + `X` | deep zoom (x30) |
| `Ctrl` + `F` | fit to window |
| `Ctrl` + `P` | toggle control panel |
| `Ctrl` + `O` / `Ctrl` + `S` / `Ctrl` + `Shift` + `S` | open image / save image / save view |
| `Ctrl` + `C` | copy image to clipboard (via `xclip`) |
| `Esc` / `Alt` + `F4` | close dialogs / quit |

## Architecture

1. **WindowManager** (`window_manager.hpp`) — thread-safe singleton managing window state (name, flags, title, viewport, image buffer, mouse callback, trackbars, properties, transient messages) plus the global control panel. Windows are owned via `shared_ptr` so concurrent accessors never dangle.
2. **LowguiRootPlan** (`src/lowgui_root_plan.hpp`) — the root Plan-V4D plan behind the long-lived render engine. Each frame it reads the current window set, arranges the grid, and draws via NanoVG; it dispatches mouse/key edges, owns the offscreen framebuffer snapshots, and runs the ImGui menu bar, dialogs, shortcuts and context menu.
3. **SinkSource** (`sink_source.hpp`) — a thread-safe buffer that images can be pushed into from any thread and converted into a `cv::v4d::Source`. The per-window image buffers are the primary path; `SinkSource` is the extension point.
4. **Lowgui API** (`lowgui.hpp` / `src/lowgui.cpp`) — public highgui-style functions. `waitKey` starts (once per process) the render/event-loop engine and coordinates key waits and framebuffer snapshots against it. Key codes are mapped by `src/lowgui_input.hpp`.

## Testing

### Unit tests (no display server required)

```bash
./opencv/build/bin/opencv_test_lowgui --gtest_filter=-*Rendering*
```

The `waitKey`-based API tests run with zero windows, so they never start the real-display engine.

### Offscreen rendering (no display server with Mesa llvmpipe)

```bash
LIBGL_ALWAYS_SOFTWARE=1 ./opencv/build/bin/opencv_test_lowgui_offscreen
# or: xvfb-run -a ./opencv/build/bin/opencv_test_lowgui_offscreen
```

The offscreen suite forces the headless engine, renders images through the `readFramebuffer()` snapshot path and asserts on the captured pixels and `waitKey(0)` semantics. Input is injected via `gwe::push(...)`.

### Full rendering tests (legacy) under Xvfb

```bash
xvfb-run -a ./opencv/build/bin/opencv_test_lowgui --gtest_filter=*Rendering*
```

### Full system tests (QEMU)

All tests also run inside a QEMU VM for full isolation: `./scripts/build-vm-image.sh` (requires sudo) builds a Debian 12 image with Mesa llvmpipe, then `./scripts/vm-boot-and-test.sh` mounts the source tree via virtio-9p, runs both test binaries inside the VM, and copies JUnit XML results back out.

CI (GitHub Actions) runs the QEMU workflow plus an Xvfb workflow on push/PR to `main`/`master`. The VM image is cached and only rebuilt when the provisioning scripts change.

## Compatibility & limitations

- Key codes match Qt's X11 values, including the numpad (`KP_Add`/`KP_Subtract` at 65451/65453). `Ctrl`-prefixed combinations are suppressed from `waitKey` since ImGui consumes them for shortcuts, mirroring Qt. Modifiers are tracked for mouse-callback flags but never delivered as key codes.
- There is exactly one native (GLFW) window hosting the viewer grid, and the V4D render loop can only be started once per process. Closing the native window behaves like `destroyAllWindows`: the engine keeps running, `waitKey*` returns `-1` while no windows exist, and creating new windows resumes rendering. `destroyWindow` / `destroyAllWindows` without closing the native window are safe. The engine only ends via File→Quit, `request_finish`, or `SIGINT`/`SIGTERM`.
- `imshow` with an unsupported depth (`CV_32S` etc.) or channel count is skipped for that window with a logged warning rather than crashing the engine.
- `WINDOW_GUI_NORMAL` windows get no context menu; "Save view as…" currently saves the window's *source* image.
- In offscreen/headless mode input arrives only through injected events (`gwe::push(...)`); there is no keyboard source.

## License

Apache 2.0