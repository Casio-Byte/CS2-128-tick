# CS2 Server Launcher (v4)

Windows launcher for a CS2 dedicated server at 64 / 128 / 256 tick, with the same v4 patch engine (`../engine`), SteamCMD install/update, MatchZy (competitive / wingman / practice) and Retakes + InstaDefuse modes, players / match / admins / connect / info tabs.

> [!WARNING]
> LAN / `-insecure` servers only. Players must use the game launcher at the same tick rate.

Plugins are **downloaded on the user's PC** from their official release pages (Metamod:Source, CounterStrikeSharp, MatchZy 0.9.3, RetakesPlugin 3.1.2, cs2-instadefuse 2.0.0); nothing from them is stored in this repository. Settings, passwords and the GSLT stay in `%LOCALAPPDATA%\CS2-128-tick\server_launcher.ini` on the user's PC and are never committed.

Build: `../build_v4.sh` (MinGW). `cs2server.ps1` is embedded in the exe as a resource and extracted at start.

```text
LAN / -insecure servers only. Players must run the game launcher at the
SAME tick rate. Uses the same built-in patch engine (CS2 1.42.0.1 only;
other builds are refused, 64 tick still works).

First run
  1. Pick a server folder (default C:\CS2Server) and a SteamCMD folder
     (default C:\CS2Server\steamcmd). If steamcmd.exe is already in the
     SteamCMD folder it is used; otherwise it is downloaded there.
     Click Install / Update: installs/updates CS2 (app 730, anonymous login).
     Already have the server files? Pick the folder that contains "game"
     and skip Install / Update.
  2. Click Install plugins. Downloads from the official release pages:
       Metamod:Source 2.0 (latest dev build), CounterStrikeSharp (latest, with runtime),
       MatchZy 0.9.3, RetakesPlugin 3.1.2 (with map spawns), cs2-instadefuse 2.0.0
     and adds Metamod to game\csgo\gameinfo.gi.
  3. Click Fix firewall (asks for admin): removes Block rules for cs2.exe,
     allows UDP/TCP 27015.
  4. Pick mode + tick, click Start.

Modes
  Competitive / Wingman / Practice -> MatchZy only (practice = matchzy_autostart_mode 2;
                                     players can also type .prac)
  Retakes + InstaDefuse            -> RetakesPlugin + InstaDefuse only. The bomb is
                                     auto-planted by Retakes (IsAutoPlantEnabled).
  MatchZy and Retakes are never loaded together: before every Start the
  unused ones are moved to addons\counterstrikesharp\plugins\disabled.
  Mode settings: game\csgo\cfg\cs2launcher_<mode>.cfg (rewritten on Start).

Tick
  Patch coverage (CS2 1.42.0.1): 617 edits - engine2 107 sites, server 197,
  client 301 + 4 constants each, incl. the dedicated-server frame cap, GOTV
  tick rate/delay and lag compensation. Every 1/64 / 64.0 use left alone is
  listed with its reason in src/engine/coverage_1420.txt.
  Start patches the server's engine2/server/client.dll to the chosen rate.
  Originals (bytes, SHA-256, timestamps, attributes) are backed up to
  %LOCALAPPDATA%\CS2-128-tick\backup - nothing is stored in the server folder;
  64 tick / Restore puts them back exactly and deletes the backup and adds -insecure, +sv_fps_max <rate>
  and "[128 tick]" to the server name so players can see the rate.
  64 restores the original DLLs. Install / Update restores 64 tick first.

Tabs
  Server   settings (Map is a dropdown of the maps in game\csgo\maps), install / plugins / firewall / start / stop, launcher log
  Console  Change map dropdown (changelevel over RCON);
           the cs2.exe server window is embedded here ("Embed server window";
           untick to pop it out). Without embedding: the server's console.log
           text. Command box: commands go over RCON (localhost) and the reply is
           shown. If no RCON password is set, the launcher uses an automatic one.
  Players  connected players (refreshed every 15 s via status_json, falling back
           to status), Kick, Ban, Bans list, Steam profile (or double-click),
           Add to Team 1 / Team 2 for the Match tab.
           Bans: %LOCALAPPDATA%\CS2-128-tick\bans.txt (steamid64|name|ip, delete a
           line to unban); banned players are kicked whenever they show up.
  Match    MatchZy match: team names, players as Steam profiles (one per line:
           profile link, SteamID64, STEAM_0:x:y or custom URL name),
           players per team, Bo1/Bo2/Bo3/Bo5, sides (knife / team 1 CT / team 2 CT),
           maps: "Pick the maps in order" (Map 1-5 dropdowns, as many as the
           series) or "Veto from the ticked map pool" (+ extra pool maps).
           Resolve Steam profiles turns each line into "SteamID64 Name".
           Load match on server writes game\csgo\cs2launcher_match.json and runs
           matchzy_loadmatch. As many maps as the series = fixed order; more maps
           than the series = in-game veto (.ban / .pick). End match = css_endmatch.
           Needs Competitive or Wingman mode with plugins on.
  Admins   add / edit / remove admins by Steam profile (link, SteamID64,
           STEAM_0:x:y or custom URL name) with CounterStrikeSharp permission
           flags (@css/root = everything, @css/kick, @css/ban, @css/changemap ...)
           and optionally MatchZy admin. Writes
             game\csgo\addons\counterstrikesharp\configs\admins.json
             game\csgo\cfg\MatchZy\admins.json
           and sends css_reloadadmins if the server is running (MatchZy reads its
           list on map change / restart). Players tab: "Make admin" for a
           connected player. Note: groups / immunity entries in an existing
           admins.json are not kept when the launcher saves it.
  Info     plugin check: plugin files on disk (Metamod, CounterStrikeSharp,
           MatchZy / Retakes / InstaDefuse enabled or disabled, which ones the
           mode needs) and, while running, what really loaded (meta list,
           css_plugins list over RCON) with LOADED / NOT LOADED per plugin and
           any plugin errors from console.log.
           tick-rate check: patched rate of the server DLLs (files + backups), launch settings (-insecure, sv_fps_max) and live replies
           from the server over RCON (sv_fps_max, hostname with the [tick] tag).
           CS2 has no console command that prints the tick rate.
  Connect  local IP(s), external IP, port, server / RCON password (Show to reveal),
           ready-to-copy "connect ip:port; password ..." commands and
           steam://connect links for LAN and internet players, plus the server's
           tick rate, mode and LAN/Internet setting. Also logged on every Start.

Steam server library
  Before every Start (and after Install / Update) the launcher really tests
  game\bin\win64\steamclient64.dll in a separate process: it loads it and
  asks it for SteamClient023, as CS2 does. If that fails it logs why (load
  error + missing DLLs it imports, or interface not provided), updates SteamCMD,
  tests SteamCMD's copy and copies steamclient64.dll + tier0_s64.dll +
  vstdlib_s64.dll as one matching set; then tries the Steam client's copy.
  Note: the Steamworks SDK loads the steamclient64.dll registered by Steam
  (HKCU\Software\Valve\Steam\ActiveProcess\SteamClientDll64), even when Steam is
  closed. If that copy is too old (e.g. Steam not updating), the launcher points
  that entry at the working server copy for the first 30 s of a start, then puts
  the original back (also on the next launcher start if it was closed early).
  The real fix is updating Steam.
  Without a working one the server stops with
  "FATAL ERROR: Failed to initialize Steamworks SDK for gameserver. No SteamClient023".

Logs and diagnostics
  Every Start writes its own log: %LOCALAPPDATA%\CS2-128-tick\logs\server_<date>_<time>.log
  (button: Open logs). It contains:
   - pre-start checks: CS2 build, missing files, map file, Metamod line vs
     Metamod/CSS files, Visual C++ runtime, port in use, cs2.exe already
     running, write access, free space, patch state
   - the server's own console output (cs2> lines)
   - on an unexpected exit: exit code + what it means, CS2's console.log,
     new crash dumps (.mdmp), Windows Application error events for cs2.exe
  Debug window: tick it and Start - the server runs in a console that stays
    open after CS2 exits, so its last message stays on screen.

Other
  Server console: the server runs inside the launcher (Windows 10 1809+);
    its output is in the log as cs2> lines, commands go in the Server command box.
  Progress: bar + status line under the buttons (phase, GB done / total,
    speed, time left) parsed from SteamCMD; progress is logged every 10%.
  Pause / Resume: suspends SteamCMD (or the plugin installer) in place.
  Cancel: stops it. Running Install / Update again continues from what
    SteamCMD already downloaded.
  Run without plugins: removes Metamod from gameinfo.gi (also done when
    CounterStrikeSharp is not installed) (for when a CS2
    update breaks CounterStrikeSharp - CSS v1.0.380 crashes CS2 1.42.0.1).
  Check updates: CS2 build vs latest CounterStrikeSharp release, and the
    pinned plugin versions vs their latest releases.
  Crash restart: restarts after a crash; gives up after 3 crashes in 3 min.
  Settings (incl. passwords/GSLT): %LOCALAPPDATA%\CS2-128-tick\server_launcher.ini
  Log: %LOCALAPPDATA%\CS2-128-tick\server_launcher.log

Build (MinGW)
  windres src/server/server.rc -O coff -o server_res.o
  x86_64-w64-mingw32-gcc -O2 -municode -mwindows -static src/server/cs2_server_launcher.c server_res.o \
     -o cs2_server_launcher.exe -lshlwapi -lole32 -lshell32 -lcomctl32 -lurlmon -lws2_32 -liphlpapi -lwininet
```
