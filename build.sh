#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIR="$SCRIPT_DIR/Plan-V4D/"
OPENCV_DIR="$SCRIPT_DIR/opencv"
BUILD_DIR="$OPENCV_DIR/build"
BUILD_MARKER="$BUILD_DIR/.build-type"
JOBS=4
TARGET=plan+v4d+lowgui
BUILD_TYPE=debug
REBUILD=
TEST_ARGS=

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] [-- <test args>]

Build the OpenCV plan / plan+v4d+lowgui stack.

Options:
  -t, --target TARGET    What to build: 'plan' or 'plan+v4d+lowgui'.
                         'plan+v4d' is accepted as a shorthand for
                         'plan+v4d+lowgui'. (default: plan+v4d+lowgui)
  -b, --build-type TYPE  Build configuration: release, debug, asan, ubsan, tsan
                         (default: debug)
  -j, --jobs N           Parallel build jobs (default: 4)
  -r, --rebuild          Force a fresh cmake configure, discarding the current
                         build directory contents
  -h, --help             Show this help

Any arguments after '--' are passed through to the test binaries (only relevant
when target is 'plan', which builds and runs the plan tests).

Examples:
  $(basename "$0")
  $(basename "$0") -t plan+v4d+lowgui
  $(basename "$0") -t plan -b asan -j 8 -- --gtest_filter=Plan.*
EOF
  exit 0
}

while [ $# -gt 0 ] && [ "$1" != "--" ]; do
  case "$1" in
    -t|--target)
      [ $# -ge 2 ] || { echo "Missing value for $1" >&2; exit 1; }
      TARGET="$2"; shift 2 ;;
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

case "$TARGET" in
  plan) ;;
  plan+v4d+lowgui) ;;
  plan+v4d)
    # Shorthand for the full stack (documented in README and used by CI/VM).
    TARGET=plan+v4d+lowgui ;;
  *) echo "Invalid target '$TARGET' (expected 'plan' or 'plan+v4d+lowgui')" >&2; exit 1 ;;
esac

# Which modules' test/perf binaries should cmake generate. This must match the
# active target: 'plan' builds the plan module's tests, everything else builds
# lowgui's (setting it unconditionally to lowgui made `make opencv_test_plan`
# fail with "No rule to make target").
TEST_MODULES=lowgui
PERF_TEST_MODULES=lowgui
if [ "$TARGET" = plan ]; then
  TEST_MODULES=plan
  PERF_TEST_MODULES=plan
fi

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
  # Prefer SSH (fast for repo owners), fall back to HTTPS so developers and CI
  # without GitHub SSH keys can bootstrap the build.
  if ! git clone git@github.com:kallaballa/opencv.git "$OPENCV_DIR" 2>/dev/null; then
    rm -rf "$OPENCV_DIR"
    git clone https://github.com/kallaballa/opencv.git "$OPENCV_DIR"
  fi
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
  -DOPENCV_BUILD_TEST_MODULES_LIST="$TEST_MODULES"
  -DOPENCV_BUILD_PERF_TEST_MODULES_LIST="$PERF_TEST_MODULES"
  -DCMAKE_POLICY_VERSION_MINIMUM=3.24
  -DWITH_WAYLAND=ON
  -DOPENCV_V4D_ENABLE_ES3=OFF
  -DOPENCV_V4D_ENABLE_BGFX=OFF
  -DOPENCV_ALGO_HINT_DEFAULT=ALGO_HINT_APPROX
  -DCMAKE_MODULE_LINKER_FLAGS="/usr/local/lib64/"
  -DINSTALL_BIN_EXAMPLES=OFF
  -DBUILD_EXAMPLES=ON
  -DBUILD_PACKAGE=OFF
  -DBUILD_DOCS=OFF
  -DCV_TRACE=OFF
  -DBUILD_SHARED_LIBS=ON
  -DWITH_OPENGL=ON
  -DOPENCV_ENABLE_EGL_INTEROP=ON
  -DOPENCV_ENABLE_GLX_INTEROP=ON
  -DOPENCV_FFMPEG_ENABLE_LIBAVDEVICE=ON
  -DBUILD_HARFBUZZ=ON
  -DWITH_FFMPEG=ON
  -DOPENCV_FFMPEG_SKIP_BUILD_CHECK=ON
  -DWITH_VA=ON
  -DWITH_VA_INTEL=ON
  -DWITH_1394=OFF
  -DWITH_ADE=OFF
  -DWITH_VTK=OFF
  -DWITH_EIGEN=OFF
  -DWITH_QT=OFF
  -DWITH_GTK=OFF
  -DWITH_GTK_2_X=OFF
  -DWITH_IPP=OFF
  -DWITH_JASPER=OFF
  -DWITH_WEBP=OFF
  -DWITH_OPENEXR=OFF
  -DWITH_OPENVX=OFF
  -DWITH_OPENNI=OFF
  -DWITH_OPENNI2=OFF
  -DWITH_TBB=OFF
  -DWITH_TIFF=OFF
  -DWITH_OPENCL=ON
  -DWITH_OPENCL_SVM=OFF
  -DWITH_OPENCLAMDFFT=OFF
  -DWITH_OPENCLAMDBLAS=OFF
  -DWITH_GPHOTO2=OFF
  -DWITH_LAPACK=OFF
  -DWITH_ITT=OFF
  -DWITH_QUIRC=ON
  -DBUILD_ZLIB=OFF
  -DBUILD_opencv_apps=OFF
  -DBUILD_opencv_calib3d=ON
  -DBUILD_opencv_ccalib=ON
  -DBUILD_opencv_dnn=ON
  -DBUILD_opencv_features2d=ON
  -DBUILD_opencv_flann=ON
  -DBUILD_opencv_gapi=OFF
  -DBUILD_opencv_ml=OFF
  -DBUILD_opencv_photo=ON
  -DBUILD_opencv_shape=OFF
  -DBUILD_opencv_imgcodecs=ON
  -DBUILD_opencv_superres=OFF
  -DBUILD_opencv_videoio=ON
  -DBUILD_opencv_videostab=OFF
  -DBUILD_opencv_stitching=ON
  -DBUILD_opencv_java=OFF
  -DBUILD_opencv_js=OFF
  -DBUILD_opencv_python2=OFF
  -DBUILD_opencv_python3=OFF
  -DBUILD_opencv_alphamat=OFF
  -DBUILD_opencv_aruco=OFF
  -DBUILD_opencv_barcode=OFF
  -DBUILD_opencv_bgsegm=OFF
  -DBUILD_opencv_bioinspired=OFF
  -DBUILD_opencv_cnn_3dobj=OFF
  -DBUILD_opencv_cudaarithm=OFF
  -DBUILD_opencv_cudabgsegm=OFF
  -DBUILD_opencv_cudacodec=OFF
  -DBUILD_opencv_cudafeatures2d=OFF
  -DBUILD_opencv_cudafilters=OFF
  -DBUILD_opencv_cudaimgproc=OFF
  -DBUILD_opencv_cudalegacy=OFF
  -DBUILD_opencv_cudaobjdetect=OFF
  -DBUILD_opencv_cudaoptflow=OFF
  -DBUILD_opencv_cudastereo=OFF
  -DBUILD_opencv_cudawarping=OFF
  -DBUILD_opencv_cudev=OFF
  -DBUILD_opencv_cvv=OFF
  -DBUILD_opencv_datasets=OFF
  -DBUILD_opencv_dnn_objdetect=OFF
  -DBUILD_opencv_dnns_easily_fooled=OFF
  -DBUILD_opencv_dnn_superres=OFF
  -DBUILD_opencv_dpm=OFF
  -DBUILD_opencv_face=ON
  -DBUILD_opencv_freetype=OFF
  -DBUILD_opencv_fuzzy=OFF
  -DBUILD_opencv_hdf=OFF
  -DBUILD_opencv_hfs=OFF
  -DBUILD_opencv_img_hash=OFF
  -DBUILD_opencv_intensity_transform=OFF
  -DBUILD_opencv_julia=OFF
  -DBUILD_opencv_line_descriptor=OFF
  -DBUILD_opencv_matlab=OFF
  -DBUILD_opencv_mcc=OFF
  -DBUILD_opencv_optflow=ON
  -DBUILD_opencv_ovis=OFF
  -DBUILD_opencv_phase_unwrapping=OFF
  -DBUILD_opencv_plot=ON
  -DBUILD_opencv_quality=OFF
  -DBUILD_opencv_rapid=OFF
  -DBUILD_opencv_reg=OFF
  -DBUILD_opencv_rgbd=OFF
  -DBUILD_opencv_saliency=OFF
  -DBUILD_opencv_sfm=OFF
  -DBUILD_opencv_structured_light=OFF
  -DBUILD_opencv_surface_matching=OFF
  -DBUILD_opencv_text=OFF
  -DBUILD_opencv_tracking=ON
  -DBUILD_opencv_viz=OFF
  -DBUILD_opencv_wechat_qrcode=OFF
  -DBUILD_opencv_xfeatures2d=OFF
  -DBUILD_opencv_ximgproc=ON
  -DBUILD_opencv_xphoto=OFF
  -DBUILD_opencv_world=OFF
  -DBUILD_opencv_lowgui=ON
  -DWITH_PTHREADS_PF=ON
  -DCV_ENABLE_INTRINSICS=ON
  -DBUILD_opencv_video=ON
  -DBUILD_opencv_plan=ON
  -DBGFX_CONFIG_MULTITHREADED=ON
  -DBGFX_CONFIG_PASSIVE=ON
  -DOPENCV_EXTRA_MODULES_PATH="$DIR/modules;$SCRIPT_DIR"
  -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE"
)

if [ "$TARGET" = plan+v4d+lowgui ]; then
  CMAKE_ARGS+=(
    -DWITH_GTK=ON
    -DBUILD_TESTS=ON
    -DBUILD_PERF_TESTS=OFF
    # opencv_ts links against highgui's test UI, so the full target builds the
    # real highgui (GTK backend) as a dependency; code should use lowgui.
    -DBUILD_opencv_highgui=ON
    -DBUILD_opencv_geometry=ON
    -DBUILD_opencv_stereo=ON
    -DBUILD_opencv_xobjdetect=ON
    -DBUILD_opencv_v4d=ON
    -DBUILD_opencv_ts=ON
  )
fi

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

echo "==> Building target '${TARGET}' (build type: ${BUILD_TYPE}) in ${BUILD_DIR}"
cd "$BUILD_DIR"

if [ "$REBUILD" = 1 ]; then
  cmake --fresh "${CMAKE_ARGS[@]}" "$OPENCV_DIR"
else
  if [ "$TARGET" = plan+v4d+lowgui ] && [ -f CMakeCache.txt ]; then
    # Purging stale internal module flags makes re-enabling modules that an
    # earlier configure disabled (e.g. opencv_ts from a BUILD_TESTS=OFF run)
    # actually take effect; INTERNAL cache vars are immune to -D overrides.
    cmake -U'HAVE_opencv_ts' -U'BUILD_opencv_ts' \
          -U'HAVE_opencv_v4d' -U'BUILD_opencv_v4d' \
          -U'HAVE_opencv_plan' -U'BUILD_opencv_plan' \
          -U'HAVE_opencv_lowgui' -U'BUILD_opencv_lowgui' \
          -U'HAVE_opencv_highgui' -U'BUILD_opencv_highgui' \
          -U'BUILD_TESTS' -U'BUILD_PERF_TESTS' -U'WITH_GTK' "$BUILD_DIR" || true
  fi
  cmake "${CMAKE_ARGS[@]}" "$OPENCV_DIR"
fi

echo "$BUILD_TYPE" > "$BUILD_MARKER"

if [ "$TARGET" = plan+v4d+lowgui ]; then
  # The default build target does not include the gtest binaries (they live
  # under opencv_tests), so build them explicitly along with their deps.
  make -j"$JOBS" opencv_test_lowgui opencv_test_lowgui_offscreen
elif [ "$TARGET" = plan ]; then
  make -j"$JOBS" opencv_test_plan opencv_perf_plan
  if [ -x ./bin/opencv_test_plan ]; then
    ./bin/opencv_test_plan $TEST_ARGS
  else
    ./opencv_test_plan $TEST_ARGS
  fi
  if [ -x ./bin/opencv_perf_plan ]; then
    ./bin/opencv_perf_plan $TEST_ARGS
  else
    ./opencv_perf_plan $TEST_ARGS
  fi
fi
