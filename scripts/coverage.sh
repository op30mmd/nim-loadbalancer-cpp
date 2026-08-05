#!/usr/bin/env bash
# ============================================================================
# Local coverage verification script (Linux/macOS, GCC or Clang).
#
# Configures a coverage-instrumented build, runs the full test suite and
# prints a per-file line coverage report. Fails if total line coverage is
# below the required threshold (default 90%, matching the CI gate).
#
# Requires: cmake, a GNU/Clang toolchain and gcovr
#   Debian/Ubuntu:  sudo apt-get install gcovr
#   macOS:          brew install gcovr
# ============================================================================
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build-coverage}"
THRESHOLD="${THRESHOLD:-90}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug -DNIM_ENABLE_COVERAGE=ON "$@"
cmake --build "$BUILD_DIR" --parallel "$JOBS"

echo ""
echo "=== Running tests ==="
ctest --test-dir "$BUILD_DIR" --output-on-failure

echo ""
echo "=== Coverage report (threshold: ${THRESHOLD}%) ==="
gcovr -r . "$BUILD_DIR" --fail-under-line "$THRESHOLD" \
    --filter '\./(utils|anthropic_handler|proxy_handlers|proxy_config|key_manager|logger|provider_manager|stats_collector)\.'
