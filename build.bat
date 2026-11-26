@echo off
rem Build with MinGW-w64 (e.g. MSYS2: pacman -S mingw-w64-x86_64-gcc) - gcc and windres must be on PATH
rem   cs2_128tick.exe           manual patcher (Patch / Restore 64 tick) + command line
rem   cs2_128tick_launcher.exe  automatic launcher (patch, start CS2, restore when it closes)
cd /d "%~dp0src"
set LIBS=-lbcrypt -lshell32 -lole32 -lgdi32 -luser32 -lcomdlg32 -lcomctl32
windres app.rc -O coff -o app.res || goto :err
gcc -O2 -Wall -Wno-unused-function -mwindows -static -o ..\cs2_128tick.exe cs2_128tick.c app.res %LIBS% || goto :err
windres launcher.rc -O coff -o launcher.res || goto :err
gcc -O2 -Wall -Wno-unused-function -mwindows -static -o ..\cs2_128tick_launcher.exe launcher.c launcher.res %LIBS% || goto :err
del app.res launcher.res
echo built cs2_128tick.exe and cs2_128tick_launcher.exe
exit /b 0
:err
echo BUILD FAILED
exit /b 1
