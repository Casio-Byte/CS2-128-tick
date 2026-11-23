#!/usr/bin/env bash
# Cross-compile cs2_128tick.exe on Linux (needs gcc-mingw-w64-x86-64)
set -euo pipefail
cd "$(dirname "$0")/src"
x86_64-w64-mingw32-windres app.rc -O coff -o app.res
x86_64-w64-mingw32-gcc -O2 -Wall -Wno-unused-function -mwindows -static \
  -o ../cs2_128tick.exe cs2_128tick.c app.res \
  -lbcrypt -lshell32 -lole32 -lgdi32 -luser32 -lcomdlg32 -lcomctl32
rm -f app.res
echo "built cs2_128tick.exe"
