@echo off
rem Build with MinGW-w64 (e.g. MSYS2: pacman -S mingw-w64-x86_64-gcc) - gcc and windres must be on PATH
cd /d "%~dp0src"
windres app.rc -O coff -o app.res || goto :err
gcc -O2 -Wall -Wno-unused-function -mwindows -static -o ..\cs2_128tick.exe cs2_128tick.c app.res -lbcrypt -lshell32 -lole32 -lgdi32 -luser32 -lcomdlg32 -lcomctl32 || goto :err
del app.res
echo built cs2_128tick.exe
exit /b 0
:err
echo BUILD FAILED
exit /b 1
