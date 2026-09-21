#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OPENCV_DIR="$(dirname "$SCRIPT_DIR")/opencv"
BUILD_DIR="$OPENCV_DIR/build"
BUILD_MARKER="$BUILD_DIR/.build-type"
JOBS=4
BUILD_TYPE=debug
REBUILD=
TEST_ARGS=

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] [-- <test args>]

Build the lowgui OpenCV module.

Options:
  -b, --build-type TYPE  Build configuration: release, debug, asan, ubsan, tsan
                         (default: debug)
  -j, --jobs N           Parallel build jobs (default: 4)
  -r, --rebuild          Force a fresh cmake configure, discarding the current
                         build directory contents
  -h, --help             Show this help

Any arguments after '--' are passed through to the test binaries.
EOF
  exit 0
}

while [ $# -gt 0 ] && [ "$1" != "--" ]; do
  case "$1" in
    -b|--build-type)
      [ $# -ge 2 ] || { echo "Missing value for $1" >&2; exit 1; }
      BUILD_TYPE="$2"; shift 2 ;;
    -j|--jobs)
      [ $# -ge 2 ] || { echo "Missing value for $1" >&2; exit 1; }
      JOBS="$2"; shift 2 ;;
    -r|--rebuild)
      REBUILD=1; shift ;;
    -h|--help)
      usage ;;
    *)
      echo "Unknown argument: $1" >&2
      usage ;;
  esac
done
[ $# -gt 0 ] && [ "$1" = "--" ] && shift
TEST_ARGS="$*"

CMAKE_BUILD_TYPE=Debug
C_FLAGS=
CXX_FLAGS="-DCL_TARGET_OPENCL_VERSION=120"
EXE_LINKER_FLAGS=
SHARED_LINKER_FLAGS=

case "$BUILD_TYPE" in
  release)
    CMAKE_BUILD_TYPE=Release ;;
  debug)
    CMAKE_BUILD_TYPE=Debug ;;
  relwithdeb)
    CMAKE_BUILD_TYPE=ReleaseWithDebInfo ;;
  asan)  SAN="-fsanitize=address" ;;
  ubsan) SAN="-fsanitize=undefined" ;;
  tsan)  SAN="-fsanitize=thread" ;;
  *)
    echo "Invalid build type '$BUILD_TYPE' (expected release, debug, asan, ubsan or tsan)" >&2
    exit 1 ;;
esac

if [ -n "${SAN:-}" ]; then
  C_FLAGS="$SAN -fno-omit-frame-pointer"
  CXX_FLAGS="$CXX_FLAGS $SAN -fno-omit-frame-pointer"
  EXE_LINKER_FLAGS="$SAN"
  SHARED_LINKER_FLAGS="$SAN"
fi

if [ ! -d "$OPENCV_DIR" ]; then
  echo "Error: OpenCV not found at $OPENCV_DIR" >&2
  exit 1
fi

if [ -f "$BUILD_MARKER" ] && [ "$(cat "$BUILD_MARKER")" != "$BUILD_TYPE" ]; then
  echo "Build dir was configured as '$(cat "$BUILD_MARKER")', reconfiguring for '$BUILD_TYPE'"
  REBUILD=1
fi

if [ "$REBUILD" = 1 ] || [ ! -d "$BUILD_DIR" ]; then
  rm -rf "$BUILD_DIR"
  mkdir -p "$BUILD_DIR"
fi

CMAKE_ARGS=(
  -DCMAKE_POLICY_VERSION_MINIMUM=3.24
  -DCMAKE_MODULE_PATH="${SCRIPT_DIR}/cmake"
  -DOPENCV_EXTRA_MODULES_PATH="${SCRIPT_DIR};/home/elchaschab/devel/Plan-V4D/modules"
  -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE"
  -DBUILD_HARFBUZZ=ON
  -DBUILD_SHARED_LIBS=ON
  -DBUILD_EXAMPLES=ON
  -DBUILD_TESTS=OFF
  -DBUILD_PERF_TESTS=OFF
  -DBUILD_opencv_highgui=OFF
  -DBUILD_opencv_v4d=ON
  -DBUILD_opencv_plan=ON
  -DWITH_QT=OFF
  -DWITH_GTK=OFF
  -DWITH_OPENGL=ON
  -DOPENCV_ENABLE_GLX=ON
  -DOPENCV_ENABLE_EGL=ON
  -DWITH_FFMPEG=ON
  -DWITH_OPENCL=ON
  -DCV_TRACE=OFF
  -DOPENCV_GENERATE_PKGCONFIG=ON
)

if [ -n "$C_FLAGS" ]; then
  CMAKE_ARGS+=(-DCMAKE_C_FLAGS="$C_FLAGS")
fi
if [ -n "$EXE_LINKER_FLAGS" ]; then
  CMAKE_ARGS+=(-DCMAKE_EXE_LINKER_FLAGS="$EXE_LINKER_FLAGS")
fi
if [ -n "$SHARED_LINKER_FLAGS" ]; then
  CMAKE_ARGS+=(-DCMAKE_SHARED_LINKER_FLAGS="$SHARED_LINKER_FLAGS")
fi
CMAKE_ARGS+=(-DCMAKE_CXX_FLAGS="$CXX_FLAGS")

echo "==> Building lowgui (build type: ${BUILD_TYPE}) in ${BUILD_DIR}"
cd "$BUILD_DIR"

if [ "$REBUILD" = 1 ]; then
  cmake --fresh "${CMAKE_ARGS[@]}" "$OPENCV_DIR"
else
  cmake "${CMAKE_ARGS[@]}" "$OPENCV_DIR"
fi

echo "$BUILD_TYPE" > "$BUILD_MARKER"

make -j"$JOBS" opencv_lowgui

if [ -x ./bin/example_lowgui_demo ]; then
  ./bin/example_lowgui_demo $TEST_ARGS
elif [ -x ./example_lowgui_demo ]; then
  ./example_lowgui_demo $TEST_ARGS
fi
