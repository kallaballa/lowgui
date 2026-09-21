#!/bin/bash
set -e

trap 'poweroff' EXIT

rm -rf /workspace
mkdir -p /workspace/lowgui
#mount -t 9p -o trans=virtio repo /workspace
#mount -t 9p -o trans=virtio out /out
cd /workspace/lowgui

export LIBGL_ALWAYS_SOFTWARE=1

./build.sh -t plan+v4d -j$(nproc)

TEST_BIN="../opencv/build/bin/opencv_test_lowgui"
if [ ! -x "$TEST_BIN" ]; then
  TEST_BIN="../opencv/build/opencv_test_lowgui"
fi

"$TEST_BIN" --gtest_output=xml:test_results.xml "$@"

cp -f test_results.xml /out/ 2>/dev/null || true
cp -f ../opencv/build/testing.xml /out/ 2>/dev/null || true
cp -f ../opencv/build/opencv_test_lowgui.xml /out/ 2>/dev/null || true
