# lowgui

A clean-room reimplementation of the OpenCV `highgui` module image viewer on top of Plan-V4D.
It maps window names to Plan-instances and feeds them images through a `SinkSource` implementation.

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)
[![C++20](https://img.shields.io/badge/C++20-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B20)
[![OpenCV](https://img.shields.io/badge/OpenCV-5.x-orange.svg)](https://opencv.org/)

## Features

- `cv::lowgui::Lowgui::namedWindow` — create a named viewer window
- `cv::lowgui::Lowgui::imshow` — push an image to a named window
- `cv::lowgui::Lowgui::waitKey` — enter the event loop / render frame
- `cv::lowgui::Lowgui::destroyWindow` / `destroyAllWindows` — cleanup
- NanoVG-based rendering with fit-to-viewport scaling
- Support for multiple windows arranged in a grid layout

## Architecture

The implementation consists of:

1. **WindowManager** (`window_manager.hpp/cpp`) — singleton that manages window data (name, flags, image buffer, viewport). Thread-safe with mutex and condition variable. Windows are owned via `shared_ptr` so concurrent accessors never dangle.

2. **LowguiRootPlan** (`lowgui_root_plan.hpp`) — the root Plan-V4D plan behind the long-lived render engine. Each frame it re-reads the current window set from the WindowManager, arranges the windows in a grid, and draws each one through NanoVG with fit-to-viewport scaling (handling 8U as well as 16S/16U/32S/32F/64F depth conversions like highgui's `imshow`). It also owns the offscreen framebuffer snapshot mechanism used by `readFramebuffer()`.

3. **SinkSource** (`sink_source.hpp`) — a Source-like buffer that can be pushed images to from external threads. The primary image path is the WindowManager's per-window image buffers; `SinkSource` is provided as an extension point.

4. **Lowgui API** (`lowgui.hpp/cpp`) — public API mimicking OpenCV's highgui functions. `waitKey` starts (once per process) a long-lived render/event-loop engine and coordinates framebuffer snapshots and key waits against it.

## Building

```bash
./build.sh -t plan+v4d+lowgui -b debug
```

## Testing

### Local tests (non-rendering)

```bash
./build.sh -t plan+v4d+lowgui -b debug
./bin/opencv_test_lowgui --gtest_filter=-*Rendering*
```

### Local tests (offscreen rendering)

Requires no display server when Mesa llvmpipe is available:

```bash
LIBGL_ALWAYS_SOFTWARE=1 ./bin/opencv_test_lowgui_offscreen
```

Or under Xvfb:

```bash
xvfb-run -a ./bin/opencv_test_lowgui_offscreen
```

### Local tests (rendering, legacy)

Rendering tests require a display server. Use Xvfb for headless runs:

```bash
xvfb-run -a ./bin/opencv_test_lowgui --gtest_filter=*Rendering*
```

### Full system tests (QEMU)

All tests, including rendering, run inside a QEMU VM for full isolation. The VM uses a Debian 12 rootfs with Mesa llvmpipe for software OpenGL.

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

The `run-tests-in-vm.sh` script inside the VM mounts the source tree via virtio-9p, builds with `build.sh -t plan+v4d+lowgui`, and executes both `opencv_test_lowgui` and `opencv_test_lowgui_offscreen`. JUnit XML results are copied to the output mount.

## CI

GitHub Actions runs the QEMU workflow and an xvfb workflow on push/PR to `main`/`master`. The VM image is cached and rebuilt only when provisioning scripts change.

## Sample

```cpp
#include <opencv2/lowgui/lowgui.hpp>
#include <opencv2/imgcodecs.hpp>

int main(int argc, char** argv) {
    std::string imgFile = argc > 1 ? argv[1] : cv::samples::findFile("lena.png");
    cv::Mat img = cv::imread(imgFile);
    if (img.empty()) return 1;

    cv::lowgui::Lowgui::namedWindow("demo");
    cv::lowgui::Lowgui::imshow("demo", img);
    cv::lowgui::Lowgui::waitKey(0);

    cv::lowgui::Lowgui::destroyAllWindows();
    return 0;
}
```

## License

Apache 2.0