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

1. **WindowManager** (`window_manager.hpp/cpp`) — singleton that manages window data (name, flags, image buffer, viewport). Thread-safe with mutex and condition variable.

2. **WindowPlan** (`window_plan.hpp`) — V4DPlan that renders a single window's image. Converts images to RGBA and draws them using NanoVG with fit-to-viewport scaling.

3. **LowguiRootPlan** (`lowgui_root_plan.hpp`) — root V4DPlan that arranges all windows in a grid layout and renders each one.

4. **SinkSource** (`sink_source.hpp`) — a Source-like buffer that can be pushed images to from external threads.

5. **Lowgui API** (`lowgui.hpp/cpp`) — public API mimicking OpenCV's highgui functions.

## Building

```bash
./build.sh -t plan+v4d -b debug
```

## Testing

### Local tests (non-rendering)

```bash
./build.sh -t plan+v4d -b debug
./bin/opencv_test_lowgui --gtest_filter=-*Rendering*
```

### Local tests (rendering)

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

The `run-tests-in-vm.sh` script inside the VM mounts the source tree via virtio-9p, builds with `build.sh -t plan+v4d`, and executes `opencv_test_lowgui`. JUnit XML results are copied to the output mount.

## CI

GitHub Actions runs the QEMU workflow on push/PR to `main`/`master`. The VM image is cached and rebuilt only when provisioning scripts change.

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