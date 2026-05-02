#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

QMAKE_BIN="${QMAKE:-}"
if [[ -z "$QMAKE_BIN" ]]; then
  if command -v qmake >/dev/null 2>&1; then
    QMAKE_BIN="$(command -v qmake)"
  elif command -v qmake6 >/dev/null 2>&1; then
    QMAKE_BIN="$(command -v qmake6)"
  else
    for candidate in \
      /c/Qt/5.15.2/mingw81_64/bin/qmake.exe \
      /c/Qt/5.15.2/mingw81_32/bin/qmake.exe \
      /c/Qt/6.*/mingw_64/bin/qmake.exe \
      /c/Qt/6.*/mingw_32/bin/qmake.exe; do
      matches=( $candidate )
      if [[ -e "${matches[0]:-}" ]]; then
        QMAKE_BIN="${matches[0]}"
        break
      fi
    done
  fi
fi

if [[ -z "$QMAKE_BIN" || ! -x "$QMAKE_BIN" ]]; then
  echo "Error: qmake was not found." >&2
  echo "Install Qt for MinGW, then run for example:" >&2
  echo "  export QMAKE=/c/Qt/5.15.2/mingw81_64/bin/qmake.exe" >&2
  echo "  export PATH=/c/Qt/5.15.2/mingw81_64/bin:/c/Qt/Tools/mingw810_64/bin:\$PATH" >&2
  echo "  bash scripts/build_qt_git_bash.sh" >&2
  exit 1
fi

MAKE_BIN="${MAKE:-}"
if [[ -z "$MAKE_BIN" ]]; then
  if command -v mingw32-make >/dev/null 2>&1; then
    MAKE_BIN="$(command -v mingw32-make)"
  elif command -v make >/dev/null 2>&1; then
    MAKE_BIN="$(command -v make)"
  else
    echo "Error: mingw32-make/make was not found in PATH." >&2
    exit 1
  fi
fi

BUILD_DIR="${BUILD_DIR:-build-gui}"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "qmake: $QMAKE_BIN"
echo "make:  $MAKE_BIN"

"$QMAKE_BIN" ../telemetry_gui.pro CONFIG+=release
"$MAKE_BIN" -j"${JOBS:-4}"

echo "GUI build finished:"
if [[ -f telemetry_gui.exe ]]; then
  echo "  $BUILD_DIR/telemetry_gui.exe"
elif [[ -f release/telemetry_gui.exe ]]; then
  echo "  $BUILD_DIR/release/telemetry_gui.exe"
fi

if [[ "${1:-}" == "--run" ]]; then
  if [[ -f telemetry_gui.exe ]]; then
    ./telemetry_gui.exe
  else
    ./release/telemetry_gui.exe
  fi
fi
