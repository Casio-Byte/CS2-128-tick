# Building the CS2 Server Pack

Everything below is run from the repository root.

## 1. The tick patcher

`cs2_128tick.exe` is the patcher from this repository: `build.bat` (Windows, MSYS2/MinGW-w64) or `./build.sh` (Linux cross-compile).

## 2. CS2 Server Control.exe

.NET Framework 4.5+ WinForms, no dependencies. With Windows' own `csc` (`C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe`) or Mono's `mcs`:

    mcs -target:winexe -sdk:4.5 -optimize+ -win32icon:server-pack/control-src/app.ico ^
        -out:"server-pack/CS2 Server Control.exe" ^
        -r:System.Drawing.dll -r:System.Windows.Forms.dll -r:System.Web.Extensions.dll ^
        -r:System.IO.Compression.dll -r:System.IO.Compression.FileSystem.dll -r:System.Net.Http.dll ^
        server-pack/control-src/*.cs

## 3. CS2 Match plugin

.NET 10 SDK:

    dotnet build server-pack/plugin-src/CS2Match -c Release

It builds against CounterStrikeSharp v1.0.380 from NuGet, or from `server-pack/lib/` if you put
`CounterStrikeSharp.API.dll` (and the `Microsoft.Extensions.*` abstractions it exposes) there.

## 4. The zip

Download these into one folder, unchanged:

| File | From |
|---|---|
| `mmsource-2.0.0-git1491-windows.zip` | <https://www.metamodsource.net/downloads.php?branch=dev> |
| `counterstrikesharp-with-runtime-windows-1.0.380.zip` | <https://github.com/roflmuffin/CounterStrikeSharp/releases> |
| `MatchZy-0.9.3.zip` | <https://github.com/shobhit-pathak/MatchZy/releases> |

Then:

    python server-pack/tools/make_pack.py <that folder>

It writes `dist/CS2-Server-Pack.zip` (and the unpacked `dist/CS2-Server-Pack/`).

## How the server gets its tick rate

`CS2 Server Control.exe` runs `cs2_128tick.exe` from next to itself:

    cs2_128tick.exe patch 128|256 "<server folder>"     before every Start (and after a SteamCMD update)
    cs2_128tick.exe undo "<server folder>"              at 64 tick, and before SteamCMD updates the server

The patcher prints to the console of whoever started it (`CONOUT$`), so the control program keeps a hidden
console, lets the patcher attach to it, and reads the text back into its log (`control-src/TickPatch.cs`).
It only ever calls the patcher on a folder that has `game\bin\win64\engine2.dll`; with no CS2 in the folder
the patcher would go looking for the player's own CS2 instead.
After each run the control program checks the result itself (`*.orig64` present and different from the DLL)
and records it in `<dll>.tickpatch`, so it notices when a CS2 update replaces the patched files.
