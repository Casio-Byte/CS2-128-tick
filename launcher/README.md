# CS2 128/256 Tick Launcher (game, v4)

Self-contained game launcher with the new v4 patch engine built in (`../engine`). No separate patcher is needed.

> [!WARNING]
> LAN / `-insecure` servers only. Never join official Valve or VAC-secured servers while patched.

Build: `../build_v4.sh` (MinGW). Supported build: CS2 1.42.0.1 (Nov 23 2026); any other build is refused and nothing is changed.

```text
LAN / -insecure servers only. Never join official Valve servers with it.
The server must be patched to the same tick rate.

No other program is needed: the patch engine is built in.
Supported build: CS2 1.42.0.1 (Nov 23 2026). Any other build is refused
and your files are left untouched.

Use
  1. Pick 128 or 256 tick, click Launch CS2.
  2. It waits for Steam (starts it if needed), patches engine2/server/client,
     runs steam.exe -applaunch 730 -insecure, watches the game's cs2.exe,
     warns if -insecure is missing, and restores 64 tick after CS2 exits
     (waits 10 s for self-relaunch, retries while DLLs are locked).
  3. Closed mid-game? The next start restores 64 tick.
  "Restore 64 tick now" restores by hand. Shortcut: cs2_tick_launcher.exe --launch 256

Info tab (tick-rate check)
  Shows what rate your CS2 DLLs are patched to (read from the files and their
  backups), whether a patched session is active, and - if CS2 is
  running - its real command line and whether -insecure is on. The server must
  run the same rate (CS2 Server Launcher shows it in the server name and on its
  own Info tab). CS2 itself has no console command that prints the tick rate.

Backups - nothing is left in the CS2 folder
  Before patching, each original DLL (bytes, SHA-256, created / modified /
  accessed times, attributes) is backed up to
    %LOCALAPPDATA%\CS2-128-tick\backup\<install>\
  The patched DLL replaces the original and keeps its timestamps.
  On restore the original bytes, timestamps and attributes are put back, the
  SHA-256 is checked against the original CS2 build, and only then is the
  backup deleted - the CS2 folder is exactly as it was, as if nothing happened.
  No .orig64 / .tickpatch files are kept next to the DLLs any more; ones from
  older versions are used for restore and then removed.
  If Steam updates CS2 while it is patched, the new files are left alone and
  the old backup is discarded.

What it patches (617 edits in total)
  engine2 107 sites, server 197, client 301, plus 4 constants per DLL written
  into int3 padding in .text (rate and 1/rate, float and double).
  - RIP-relative SSE reads of 1/64 and 64.0 used for tick math are pointed
    at the new constants.
  - mov dword [..], 0x3C800000 (interval stores) get the new interval.
  - Time->tick conversions done through helper functions (GameTick + fraction),
    lag compensation, the GOTV/HLTV tick rate and delay, and the dedicated
    server's frame cap (main loop) are included.
  - Every other 1/64 and 64.0 use was reviewed and is left alone because it is
    not about ticks (sizes in units, grids, physics, sprites, decals, image
    decoding ...). The full list with reasons: src/engine/coverage_1420.txt.

Logs: %LOCALAPPDATA%\CS2-128-tick\launcher.log

Rebuilding the tables for a new CS2 build (Linux, Python + capstone + pefile):
  scan.py / ctx.py <dll>, policy.py, gen_table.py -> engine/sites_1420.h,
  test_engine.c + verify.py to check.
Build: x86_64-w64-mingw32-gcc -O2 -municode -mwindows -static \
       src/launcher/cs2_tick_launcher.c -o cs2_tick_launcher.exe -lshlwapi -lcomctl32
```
