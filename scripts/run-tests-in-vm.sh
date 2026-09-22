#!/bin/bash
set -e

trap 'poweroff' EXIT

rm -rf /workspace
mkdir -p /workspace
mount -t 9p -o trans=virtio repo /workspace
mkdir -p /out
mount -t 9p -o trans=virtio out /out
cd /workspace/lowgui

export LIBGL_ALWAYS_SOFTWARE=1

./build.sh -t plan+v4d+lowgui -j$(nproc)

TEST_BIN="./opencv/build/bin/opencv_test_lowgui"
if [ ! -x "$TEST_BIN" ]; then
  TEST_BIN="./opencv/build/opencv_test_lowgui"
fi

"$TEST_BIN" --gtest_output=xml:test_results.xml "$@"

OFFSCREEN_BIN="./opencv/build/bin/opencv_test_lowgui_offscreen"
if [ ! -x "$OFFSCREEN_BIN" ]; then
  OFFSCREEN_BIN="./opencv/build/opencv_test_lowgui_offscreen"
fi

if [ -x "$OFFSCREEN_BIN" ]; then
  LOWGUI_HEADLESS_RENDER=1 "$OFFSCREEN_BIN" --gtest_output=xml:offscreen_results.xml
  cp -f offscreen_results.xml /out/
fi

cp -f test_results.xml /out/
