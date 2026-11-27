#!/usr/bin/env bash
# Cross-compile the v4 game launcher and server launcher on Linux (needs gcc-mingw-w64-x86-64).
# Self-contained: the patch engine (engine/) is compiled into both exes.
set -euo pipefail
cd "$(dirname "$0")"
x86_64-w64-mingw32-gcc -O2 -Wall -Wno-unused-function -municode -mwindows -static \
  -o cs2_tick_launcher.exe launcher/cs2_tick_launcher.c \
  -lshlwapi -lcomctl32 -lshell32 -lole32
echo "built cs2_tick_launcher.exe"
( cd server-launcher && x86_64-w64-mingw32-windres server.rc -O coff -o server_res.o )
x86_64-w64-mingw32-gcc -O2 -Wall -Wno-unused-function -municode -mwindows -static -D_WIN32_WINNT=0x0A00 \
  -o cs2_server_launcher.exe server-launcher/cs2_server_launcher.c server-launcher/server_res.o \
  -lshlwapi -lole32 -lshell32 -lcomctl32 -lurlmon -lws2_32 -liphlpapi -lwininet
rm -f server-launcher/server_res.o
echo "built cs2_server_launcher.exe"
# engine self-test on Linux against a folder with the original 1.42.0.1 DLLs (not in git):
#   gcc -O2 -o test_engine engine/test_engine.c && ./test_engine /path/to/dlls
