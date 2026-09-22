#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_TYPE="${BUILD_TYPE:-debug}"
QEMU_IMG="${QEMU_IMG:-lowgui-test.img}"
QEMU_KERNEL="${QEMU_KERNEL:-vmlinuz}"
QEMU_INITRD="${QEMU_INITRD:-initrd.img}"
QEMU_SSH_KEY="${QEMU_SSH_KEY:-vm-key}"
QEMU_OUT_DIR="${QEMU_OUT_DIR:-/tmp/qemu-out}"
REPO_PATH="${REPO_PATH:-$(dirname "$REPO_ROOT")}"
RUN_QEMU="${RUN_QEMU:-0}"
GTEST_BIN="${REPO_ROOT}/opencv/build/bin/opencv_test_lowgui"
RESULT_DIR="${REPO_ROOT}/test-results"
TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
SUMMARY="${RESULT_DIR}/summary-${TIMESTAMP}.txt"
PASS=0
FAIL=0
declare -a RESULTS=()

mkdir -p "$RESULT_DIR"

log() {
  echo "[test-all] $*"
}

run_test() {
  local name="$1"
  local command="$2"
  local output_file="${RESULT_DIR}/${name}-${TIMESTAMP}.txt"

  log "RUN: $name"
  log "CMD: $command"

  set +e
  eval "$command" |& tee "$output_file"
  local rc=${PIPESTATUS[0]}
  set -e

  if [ "$rc" -eq 0 ]; then
    log "PASS: $name"
    RESULTS+=("PASS: $name")
    PASS=$((PASS + 1))
  else
    log "FAIL: $name (exit $rc)"
    RESULTS+=("FAIL: $name (exit $rc)")
    FAIL=$((FAIL + 1))
  fi
}

log "=== lowgui test suite ==="
log "Repo root: $REPO_ROOT"
log "Build type: $BUILD_TYPE"
log "QEMU enabled: $RUN_QEMU"

# 1. Build
cd "$REPO_ROOT"
run_test "build" "./build.sh -t plan+v4d+lowgui -b ${BUILD_TYPE} -j 12 -r"

# 2. Locate test binary
if [ ! -x "$GTEST_BIN" ]; then
  GTEST_BIN="$(find "$REPO_ROOT" -maxdepth 3 -type f -name 'opencv_test_lowgui*' -perm -111 | head -n1 || true)"
fi

if [ -z "$GTEST_BIN" ] || [ ! -x "$GTEST_BIN" ]; then
  log "ERROR: opencv_test_lowgui binary not found"
  RESULTS+=("FAIL: test-binary-not-found")
  FAIL=$((FAIL + 1))
else
  log "Using test binary: $GTEST_BIN"

  # 3. Main test binary (no rendering tests inside; run the full suite)
  run_test "unit-tests" "$GTEST_BIN"

  # 4. Offscreen rendering tests (Xvfb / llvmpipe)
  OFFSCREEN_BIN="${REPO_ROOT}/opencv/build/bin/opencv_test_lowgui_offscreen"
  if [ ! -x "$OFFSCREEN_BIN" ]; then
    OFFSCREEN_BIN="$(find "$REPO_ROOT" -maxdepth 3 -type f -name 'opencv_test_lowgui_offscreen*' -perm -111 | head -n1 || true)"
  fi
  if [ -z "$OFFSCREEN_BIN" ] || [ ! -x "$OFFSCREEN_BIN" ]; then
    log "ERROR: opencv_test_lowgui_offscreen binary not found"
    RESULTS+=("FAIL: offscreen-rendering-binary-not-found")
    FAIL=$((FAIL + 1))
  else
    log "Using offscreen test binary: $OFFSCREEN_BIN"
    run_test "offscreen-rendering" "xvfb-run -a $OFFSCREEN_BIN"
  fi
fi

# 5. QEMU system tests
if [ "$RUN_QEMU" = "1" ]; then
  if [ ! -f "$REPO_ROOT/$QEMU_IMG" ] || [ ! -f "$REPO_ROOT/$QEMU_KERNEL" ] || [ ! -f "$REPO_ROOT/$QEMU_INITRD" ]; then
    log "QEMU artifacts missing. Run scripts/build-vm-image.sh first."
    RESULTS+=("SKIP: qemu-tests (missing image)")
  else
    cd "$REPO_ROOT"
    run_test "qemu-tests" "./scripts/vm-boot-and-test.sh \
      --image $QEMU_IMG \
      --kernel $QEMU_KERNEL \
      --initrd $QEMU_INITRD \
      --repo-path $REPO_PATH \
      --ssh-key $QEMU_SSH_KEY \
      --out-dir $QEMU_OUT_DIR"
  fi
else
  log "QEMU tests skipped (set RUN_QEMU=1 to enable)"
  RESULTS+=("SKIP: qemu-tests")
fi

# 6. Summary
{
  echo "=== lowgui test summary ($TIMESTAMP) ==="
  echo "Build type: $BUILD_TYPE"
  echo "Results:"
  for r in "${RESULTS[@]}"; do
    echo "  $r"
  done
  echo "Passed: $PASS"
  echo "Failed: $FAIL"
  echo "Skipped: $(( ${#RESULTS[@]} - PASS - FAIL ))"
} | tee "$SUMMARY"

if [ "$FAIL" -gt 0 ]; then
  log "FAILED: $FAIL test(s) failed"
  exit 1
else
  log "ALL PASSED"
  exit 0
fi
