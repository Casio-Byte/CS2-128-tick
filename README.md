# CS2 128-Tick Patcher

An **experimental** Windows tool that makes Counter-Strike 2 run at **128 tick** (Standard) or **256 tick** (High, more experimental) on your own LAN / `-insecure` server, by patching the tick rate Valve compiled into the game's DLLs. It includes one-click **Undo**.

> [!WARNING]
> **Never connect to official Valve servers while patched.** That includes matchmaking, Premier and VAC-secured community servers. Modified game files can get your Steam account **VAC banned**, and Valve does not reverse VAC bans.
> Only use this on your own server, launch CS2 and the server with `-insecure`, and click **Undo** (or use Steam → *Verify integrity of game files*) before playing online.
>
> This project is not affiliated with or endorsed by Valve.

## Why a patch?

CS2 has no tick rate setting. `-tickrate 128` and `sv_tickrate` do nothing on current builds, and Valve's servers all run at 64 tick with sub-tick input. Disassembly shows why: the rate is a **compile-time constant**. `engine2.dll` even logs:

```
Server requested tick interval of %g, but we are compiled with fixed value of %g.
```

The 1/64 s interval and the 64 ticks/s rate are baked into `engine2.dll`, `server.dll` and `client.dll`. No plugin or config can change them.

## What it changes

Rather than overwriting the shared `64.0` / `0.015625` constants (other unrelated code reads those too), each **tick-related instruction** is pointed at the tool's own rate / 1/rate constants. These sit in unused padding between functions, so switching between 128 and 256 only rewrites those few values.

| Area | Examples |
|---|---|
| Engine tick helpers | `GetTickInterval()`, `TICKS_TO_TIME`, `TIME_TO_TICKS` |
| Networking | the `tick_interval` sent to clients in ServerInfo, and the client's "compiled with fixed value" check |
| Frame limiter | the dedicated server's 64 fps cap in `FilterTime` (without this fix, ticks arrive in bunches → jitter) |
| Movement / prediction / weapons | the per-command clock setup `tickcount = curtime × 64 + 0.5` on both server and client |
| Everything else | every remaining use of the 1/64 tick interval |

`64` used as a size or distance (player hulls, etc.) is left alone.

One patch covers all of the above: every 1/64 use, every 64.0 that converts time to ticks, and the remaining 64.0 values inside tick code. That's 608 edits on a game install.

The full list of edited instructions is in [`docs/patched_sites.txt`](docs/patched_sites.txt).

## Tick rates

| | Standard (128 tick) | High (256 tick) |
|---|---|---|
| Status | The safer choice | More experimental |
| Server CPU / network vs stock | ~2× | ~4× |
| Time budget per tick | 7.8 ms | 3.9 ms (tighter with 10 players, lots of utility or a slower CPU) |
| Client fps needed | above 128 | **above 256 at all times**, otherwise movement jitters |
| Missed 64-tick maths | 2× off | 4× off, so bugs are more likely |
| Lag compensation (likely tick-counted) | half the history time of stock | a quarter, so it's worse for high-ping players |
| Timing precision (float `curtime`) | fine for ~9 h on one map | change map / restart every ~4–5 h |
| Gain per step | −7.8 ms vs 64 | only −3.9 ms vs 128 |

The client and the server **must use the same tick rate**.

Rates above 256 aren't offered. At 512 the per-tick budget is only 1.95 ms, fps would need to stay above 512, and time precision runs out within about 2 hours.

## Check it's working

CS2 has a console command that shows the real timing of your connection:

```
cl_ticktiming print
```

1. Enable the console, if you haven't already: **Settings → Game → Enable Developer Console (~) → Yes**.
2. Join your patched server, open the console with **`~`** and run `cl_ticktiming print`.
3. Find the line **`FirstInput -> NetSend`**. It's roughly one tick minus half a frame:

| Tick rate | 1 tick | `FirstInput -> NetSend` |
|---|---|---|
| 64 (stock) | 15.6 ms | a bit under 15.6 ms (e.g. ~14–15 ms) |
| **128** | **7.8 ms** | **a bit under 7.8 ms** (e.g. ~6–7 ms) |
| 256 | 3.9 ms | a bit under 3.9 ms (e.g. ~3 ms) |

Example of the line at 128 tick:

```
[Client]     FirstInput -> NetSend . :   6.9ms (pessimistic measurement, typically = 1 tick - half frame time)
```

If it still shows about 15 ms, the game isn't at 128. Check that both the game and the server are patched (the app's status lines) and that the server was restarted after patching.

## Usage

1. Download `cs2_128tick.exe` from [Releases](../../releases). It doesn't need installing and has no dependencies.
2. Close CS2 and run it. It finds CS2 through Steam (if not, click **Change folder...** and pick `engine2.dll`; it remembers the folder and your tick rate).
3. Tick **128 tick** or **256 tick** (experimental) and click **Patch**. The first time for each rate you're shown the VAC / downsides warning.
4. Add `-insecure` to CS2's launch options in Steam (Properties → General) and start CS2.
5. Join your own `-insecure` server (patched to the same rate) and check it's working: run `cl_ticktiming print` in the console (see [Check it's working](#check-its-working)).
6. Click **Restore 64 tick** when you're done, before playing matchmaking, Premier or VAC servers again.

Servers are patched from the command line (below) or by the [CS2 Server Pack](server-pack), which does it before every start.

| Install | Patched files |
|---|---|
| Game install | `engine2.dll`, `client.dll`, `server.dll` (also covers listen/local servers) |
| SteamCMD dedicated server | `engine2.dll`, `server.dll` (no `client.dll` → skipped) |

If your server is a separate SteamCMD install, run the tool there too.

Originals are backed up next to each DLL as `*.orig64`. Undo checks the restored files byte-for-byte against the original SHA-256.

Command line:
```
cs2_128tick.exe status|patch|undo [128|256] ["<CS2 folder or any DLL path>"]
```
Exit code 0 = done. A folder that is given but isn't a CS2 install is an error (it never falls back to another install).

### Windows warnings

The exe is unsigned and edits game DLLs, so SmartScreen and some antivirus tools may flag it. You can build it yourself from source (below).

## CS2 updates

Every DLL is SHA-256-checked against the builds the tool knows before anything is written.

**Newer CS2 builds are matched automatically.** The app carries a fingerprint of every patched instruction from its newest known build. These are the bytes around each instruction, with addresses that move between builds masked out. On a build it hasn't seen, it:

1. looks only at places in the new DLLs that read a 1/64 or 64.0 value (or hold a 1/64 immediate),
2. requires the instruction ±8 bytes to match exactly and the wider ±48-byte window to match closely; a match that isn't perfect must clearly beat the runner-up,
3. treats identical copies of a helper as one group, all getting the same patch,
4. **accepts the result only if** every critical site is found (tick getters, the ServerInfo tick interval, the client's fixed-value check, the server frame-rate cap), ≥ 98.5 % of all sites are found, no two sites conflict, free padding exists for the constants, and there are no more than a few new 1/64 reads it can't account for.

If any check fails, the DLL shows *"unknown build – couldn't be matched safely"* and nothing is changed. Matched builds are labelled *"newer CS2 build, auto-matched"*, and the confirmation popup says the build hasn't been reviewed by a person.

How reliable is it? Tested in both directions between the two builds below:

- Fingerprints from 2026-11-24 applied to 2026-11-22 found 610 of 612 edits, every one byte-identical to the full analysis pipeline, with 0 wrong or extra edits.
- The reverse direction gave the same result.
- Wrong DLLs and already-patched DLLs are all refused.

`tools/port_check.c` runs the same engine on any folder of DLLs.

The tool also refuses to restore an `.orig64` backup from a different CS2 build than the rest of the install, so an old backup can never be mixed into a newer game.

Supported builds (SHA-256 of the original files):

| Build | `engine2.dll` | `server.dll` | `client.dll` |
|---|---|---|---|
| 2026-11-24 | `c525a3f56b3eb6be…` | `45fdde774c52dd04…` | `afe9cf1ffd977083…` |
| 2026-11-22 | `edb6ab58c84727ce…` | `7126c11b3c56fb32…` | `07103341bc9e0ae2…` |

Full hashes are in `src/patches.h`.

## Building

**Windows (MSYS2 / MinGW-w64):**
```
build.bat
```

**Linux (cross-compile):**
```
sudo apt install gcc-mingw-w64-x86-64
./build.sh
```

The output is a single static `cs2_128tick.exe`.

## Updating for a new CS2 build

The `tools/` folder is the analysis pipeline. It needs Python 3 with `pip install pefile capstone numpy`.

```bash
mkdir work && cp <CS2>/game/bin/win64/engine2.dll <CS2>/game/csgo/bin/win64/{server,client}.dll work/
for f in engine2 server client; do python tools/analyze.py work/$f.dll work/$f.json; done
python tools/classify3.py work/engine2.json work/server.json work/client.json
python tools/build_patches.py      # -> work/patches.json
python tools/verify.py             # re-disassembles every patched site at 128 and 256
python tools/gen_header.py work work_previous   # newest first; every build listed stays supported
gcc -O2 -o port_check tools/port_check.c && ./port_check work   # note the "uncovered" numbers
python tools/make_signatures.py work --uncovered E,S,C        # fingerprints from the newest build
```

`build_patches.py` finds the two special `engine2.dll` sites automatically: the "compiled with fixed value" check, found by its log string, and the `FilterTime` frame-rate target. It stops with an error if either can't be found unambiguously. The constant slots are picked from int3 padding; check they're still unreferenced.

| Script | What it does |
|---|---|
| `analyze.py` | Linear-sweeps `.text` and records every use of 1/64, 64, 1/128 and 128 (float/double, RIP-relative or immediate) with surrounding instructions |
| `classify3.py` | Tags each use as a getter, `ticks→time`, or `time→ticks` (follows the register to `cvttss2si`), or as unknown |
| `build_patches.py` | Picks the sites per tier and redirects their displacement to the tool's own constant slots in int3 padding; slot values = rate / 1/rate |
| `verify.py` | Applies the patches to copies at 128 and 256 and re-disassembles every edited instruction |
| `gen_header.py` | Generates the C patch table for the app |
| `make_signatures.py` | Generates the fingerprints the app uses to match newer CS2 builds by itself |
| `port_check.c` | Runs the app's auto-match engine on a folder of DLLs and reports what it would do |

## Known limitations

- **Experimental.** Some timing code may still assume 64 tick in ways static analysis can't see.
- **Opponent movement:** if other players look choppy, check `cl_interp_ratio` (1 is a sensible value at 128 tick).
- **Clients need the patch too.** The client and server must both be patched; an unpatched client stays at 64.

## License

MIT, see [LICENSE](LICENSE). No Valve binaries are included in this repository. The patch table only contains file offsets and the few bytes being changed.
