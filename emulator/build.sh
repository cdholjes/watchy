#!/bin/bash
# Build the emulator for one watch face in ../faces: ./build.sh [face-folder]   (default: my-face)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
FACE="${1:-my-face}"
FACE_DIR="$ROOT/faces/$FACE"
LIBS="${ARDUINO_LIBS:-$HOME/Documents/Arduino/libraries}"
OUT="$HERE/build/watchy-emu-$FACE"

[ -d "$FACE_DIR" ] || { echo "no watch face folder: $FACE_DIR" >&2; exit 1; }
mkdir -p "$HERE/build"

# cJSON (inside Arduino_JSON) is C; build it once on its own
JSON="$LIBS/Arduino_JSON/src"
CJSON_O="$HERE/build/cJSON.o"
[ "$CJSON_O" -nt "$JSON/cjson/cJSON.c" ] || clang -c -O1 -w "$JSON/cjson/cJSON.c" -o "$CJSON_O"

clang++ -std=gnu++20 -O1 -g -w -Wno-c++11-narrowing \
  -DARDUINO=100 -DARDUINO_ESP32S3_DEV -DEMULATOR -D__time_t_defined \
  -I "$HERE/stubs" -I "$HERE/core" -I "$LIBS/Adafruit_GFX_Library" -I "$LIBS/Time" -I "$JSON" -I "$FACE_DIR" \
  "$HERE"/core/*.cpp \
  "$LIBS/Adafruit_GFX_Library/Adafruit_GFX.cpp" \
  "$LIBS/Time/Time.cpp" "$LIBS/Time/DateStrings.cpp" \
  "$JSON/JSON.cpp" "$JSON/JSONVar.cpp" "$CJSON_O" \
  "$FACE_DIR"/*.cpp \
  -x c++ "$FACE_DIR"/*.ino \
  -o "$OUT"

echo "$OUT"
