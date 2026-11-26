# CS2 Server Pack

A drag-and-drop pack for a **CS2 dedicated server on Windows**: MatchZy matches and practice, game modes you can switch in game, and a built-in **64 / 128 / 256 tick** option. Nothing else: no skins, no extra plugins.

## What's in it

| | |
|---|---|
| **CS2 Server Control.exe** | Installs and updates the CS2 server (SteamCMD), sets it up, starts / stops it (restarts it if it crashes), shows the console, maps and players, sets the tick rate. |
| **cs2_128tick.exe** | The CS2 128-Tick Patcher (v2.4.1, from this repository), the same one players use on their game. The control program runs it to patch the server. |
| **MatchZy** 0.9.3 | Competitive / wingman matches (ready-up, knife round, pauses, demos, backups) and **practice mode** (`.prac`, `.spawn`, `.bot`, `.god`, `.traj`, `.noflash` ...). |
| **CS2 Match** (this pack) | Game modes with `!gamemode`, map votes `!rtv` / `!rtg`, free-for-all deathmatch, 1v1 duels and arenas, `/login` admin, `!tickrate`. |
| Metamod:Source 2.0, CounterStrikeSharp v1.0.380 | The plugin loader and C# platform MatchZy and CS2 Match run on (bundled, with its .NET runtime). |

## Install

Download **`CS2-Server-Pack.zip`** from [Releases](https://github.com/Casio-Byte/CS2-128-tick/releases) and extract it.

1. Drag the **`game`** folder onto your CS2 server folder (the folder that has `game` in it, e.g. `C:\cs2-server`) and choose **Replace**. No server yet? Use an empty folder: the program installs CS2 into it.
2. Run **`game\csgo\CS2 Server Control.exe`**.
3. No CS2 yet: **Components → Install / update CS2** (about 60 GB).
4. **Setup**: server name, passwords, LAN or Internet, tick rate → **Save**.
5. **Info → Start**. In game, type `/login <admin password>` in chat.

## Tick rate (64 / 128 / 256)

CS2 has no `-tickrate`: 64 tick is compiled into its DLLs. The server's `engine2.dll` and `server.dll` (and `client.dll`, which SteamCMD installs too) are patched by **`cs2_128tick.exe`** (the CS2 128-Tick Patcher v2.4.1), exactly as it patches a player's game: same reviewed patch for each CS2 build, automatic matching for newer builds with its safety checks, `*.orig64` backups. Its messages appear in the program's log.

- Pick the rate in **Setup → Tick rate** and Start. The program runs `cs2_128tick.exe patch 128|256 <server folder>`; pick 64 and Start to run `undo`.
- **CS2 updates are handled for you.** Before every Start the program checks the DLLs: if an update replaced them, the new files are patched straight away. When the program updates CS2 itself (Components, or "Update CS2 on every Start"), it restores the originals first, lets SteamCMD update them, then patches the new ones before the server starts.
- At 128 / 256 tick the server always runs with `-insecure` (patched files must never meet VAC).
- **Players patch their own CS2 to the same rate** with the same patcher: Setup → **Patch my CS2 game...** opens it on this PC; friends just get `cs2_128tick.exe` and run it. They add `-insecure` to CS2's launch options and must click **Undo** before playing matchmaking or VAC servers. A CS2 update on their side also undoes it, so they patch again.
- 256 tick is experimental: ~4x the CPU and network of 64, and any tick maths the patch missed is 4x off. Best on LAN with a fast CPU.

If a CS2 update changes the game too much for the patcher to match safely, it changes nothing and Start says so; set 64 tick until there's a newer `cs2_128tick.exe` in [Releases](https://github.com/Casio-Byte/CS2-128-tick/releases) (drop it next to `CS2 Server Control.exe`).

## Game modes: `!gamemode` (admins)

Pick a mode, then a map; it switches 5 seconds later and stays across map changes and restarts.

| Mode | Runs on | |
|---|---|---|
| Competitive 5v5 / Wingman 2v2 | MatchZy | `.ready`, `.pause`, `.tech`, `.stop` ... |
| Practice | MatchZy | `.spawn`, `.bot`, `.clear`, `.god`, `.traj`, `.noflash` ... |
| Casual / Arms Race | the game | Arms Race on `ar_` maps |
| Deathmatch | CS2 Match | free-for-all, instant respawn, knife + AK + Deagle, +50 HP per kill |
| 1v1 duels | CS2 Match | `aim_` / `awp_` / `1v1_` maps: `!duel [name]`, `!accept`, `!forfeit` |
| Multi-1v1 arenas | CS2 Match | `am_` maps: many 1v1s at once, `!guns`, `!sitout` |

MatchZy is only loaded in competitive, wingman and practice. Maps only show for the modes they're made for (`de_` maps: competitive, wingman, casual, deathmatch, practice).

**Votes**: `!rtv` (next map) and `!rtg` (game mode, then map), when 60% of players ask. Not during a live match.

**Admin**: `/login <password>` (remembered per player), `/logout`. Three wrong passwords lock you out for a minute.

All chat commands: `!commands`.

## After a CS2 update

- The program **checks for updates** when it opens and every 6 hours (or **Components → Check for updates**): CS2, Metamod, CounterStrikeSharp and MatchZy. Anything new goes in the log.
- Stop the server, then **Components → Install / update everything**. The tick rate is re-applied automatically.
- CS2 updates often break CounterStrikeSharp until its developers release a fix (the server crashes on start). The update check warns about it. Meanwhile tick **Setup → Run without plugins** for a plain server (still at your tick rate); untick it once the fix is installed.

## Network

- **LAN / virtual LAN** (Tailscale, ZeroTier): nothing to forward.
- **Internet**: a login token for App ID 730 from <https://steamcommunity.com/dev/managegameservers>, forward **UDP 27015** to this PC, and press **Setup → Fix firewall** (it also removes rules blocking cs2.exe, which Windows adds if its "allow this app?" prompt is cancelled). Don't forward TCP 27015 (RCON).

## Files

| | |
|---|---|
| `game\csgo\cs2pack\launcher_settings.json` | your Setup (private: has the passwords and token) |
| `game\csgo\cfg\cs2pack\` | one cfg per mode, and `server_settings.cfg` written by Setup |
| `game\csgo\addons\counterstrikesharp\configs\plugins\CS2Match\` | `settings.json`, `maps.json`, `state.json` |
| `*.orig64`, `*.tickpatch` next to the DLLs | the 64-tick originals (made by the patcher) and the program's notes on what it patched |

## Source and licence

Source: <https://github.com/Casio-Byte/CS2-128-tick/tree/main/server-pack> (the control program, the CS2 Match plugin, the mode configs and the script that builds this zip). The server pack is GPLv3; the tick patcher itself is MIT.
Credits: Metamod:Source (AlliedModders), CounterStrikeSharp (roflmuffin and contributors, GPLv3), MatchZy (shobhit-pathak, GPLv3), all bundled unchanged.
