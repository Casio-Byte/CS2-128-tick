#!/usr/bin/env bash
# Cross-compile on Linux (needs gcc-mingw-w64-x86-64):
#   cs2_128tick.exe           manual patcher (Patch / Restore 64 tick) + command line
#   cs2_128tick_launcher.exe  automatic launcher (patch, start CS2, restore when it closes)
set -euo pipefail
cd "$(dirname "$0")/src"
LIBS="-lbcrypt -lshell32 -lole32 -lgdi32 -luser32 -lcomdlg32 -lcomctl32"
for t in cs2_128tick:app launcher:launcher; do
  src=${t%%:*}; rc=${t##*:}; out=$([ "$src" = launcher ] && echo cs2_128tick_launcher || echo cs2_128tick)
  x86_64-w64-mingw32-windres $rc.rc -O coff -o $rc.res
  x86_64-w64-mingw32-gcc -O2 -Wall -Wno-unused-function -mwindows -static -o ../$out.exe $src.c $rc.res $LIBS
  rm -f $rc.res
  echo "built $out.exe"
done
