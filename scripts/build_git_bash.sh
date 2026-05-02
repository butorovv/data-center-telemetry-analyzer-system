#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

CXX="${CXX:-g++}"
BUILD_DIR="${BUILD_DIR:-build}"
TARGET="$BUILD_DIR/telemetry_analyzer.exe"

if ! command -v "$CXX" >/dev/null 2>&1; then
  echo "Error: compiler '$CXX' was not found in PATH." >&2
  echo "For TDM-GCC in Git Bash, try:" >&2
  echo "  export PATH=/c/TDM-GCC-64/bin:\$PATH" >&2
  exit 1
fi

mkdir -p "$BUILD_DIR"

sources=(
  src/CsvParser.cpp
  src/CsvReader.cpp
  src/DatabaseManager.cpp
  src/DataLoader.cpp
  src/GraphBuilder.cpp
  src/HybridDetector.cpp
  src/IForestDetector.cpp
  src/IsolationForestDetector.cpp
  src/KMeansDetector.cpp
  src/MetricsCalculator.cpp
  src/ParquetConverter.cpp
  src/Preprocessor.cpp
  src/RealFailureValidator.cpp
  src/SummitPrototypeAdapter.cpp
  src/Summit1970187Adapter.cpp
  src/Visualizer.cpp
  src/main.cpp
)

echo "Compiler: $CXX"
echo "Output:   $TARGET"

"$CXX" \
  -std=c++17 \
  -O2 \
  -Wall \
  -Wextra \
  -pedantic \
  -Iinclude \
  "${sources[@]}" \
  -o "$TARGET"

echo "Build finished: $TARGET"

if [[ "${1:-}" == "--smoke-test" ]]; then
  "$TARGET" --input data/sample_telemetry.csv --algorithm all --threads 2 --window 2
fi

