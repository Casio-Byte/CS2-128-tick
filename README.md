# CS2 128-Tick Patcher

An **experimental** Windows tool that makes Counter-Strike 2 run at **128 tick** (or, untested, **256 tick**) on your own LAN / `-insecure` server, by patching the tick rate Valve compiled into the game's DLLs. It includes one-click **Undo**.

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

Two modes:

- **Safe (recommended):** every 1/64 use, plus every 64.0 that provably converts time to ticks. 591 edits on a game install.
- **Aggressive:** Safe, plus a few 64.0 values inside tick code that couldn't be proven to be tick maths. 608 edits. Only use this if Safe still feels off.

The full list of edited instructions is in [`docs/patched_sites.txt`](docs/patched_sites.txt).

## Tick rates

| | 128 tick | 256 tick |
|---|---|---|
| Status | **Recommended**, confirmed with `cl_ticktiming` | **Untested** |
| Server CPU / network vs stock | ~2× | ~4× |
| Time budget per tick | 7.8 ms | 3.9 ms (measured simulation cost at 128 on LAN: ~1.1 ms) |
| Client fps needed | above 128 | **above 256 at all times**, otherwise movement jitters |
| Missed 64-tick maths | 2× off | 4× off, so bugs are more likely |
| Lag compensation (likely tick-counted) | half the history time of stock | a quarter, so it's worse for high-ping players |
| Timing precision (float `curtime`) | fine for ~9 h on one map | change map / restart every ~4–5 h |
| Gain per step | −7.8 ms vs 64 | only −3.9 ms vs 128 |

The client and the server **must use the same tick rate**.

Rates above 256 aren't offered. At 512 the per-tick budget (1.95 ms) is close to the measured simulation cost, fps would need to stay above 512, and time precision runs out within about 2 hours.

### Checking it worked

In the client console, run `cl_ticktiming print` and look at `FirstInput -> NetSend` (roughly 1 tick minus half a frame):

| Tick rate | Expected |
|---|---|
| 64 tick | ~14–15 ms |
| 128 tick | ~6–7 ms |

## Usage

1. Download `cs2_128tick.exe` from [Releases](../../releases). It doesn't need installing and has no dependencies.
2. Close CS2 and your server.
3. Run it. It finds CS2 through Steam. If it doesn't, use **Find file…** and pick `engine2.dll`, use **Browse folder…**, or paste the path, e.g.
   `C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive`
4. Choose **128 tick** (or read the downsides and choose **256 tick**), click **Patch – Safe** and confirm the warning.
5. Launch CS2 **and** your server with `-insecure`.
6. Click **Undo – restore 64 tick** when you're done.

| Install | Patched files |
|---|---|
| Game install | `engine2.dll`, `client.dll`, `server.dll` (also covers listen/local servers) |
| SteamCMD dedicated server | `engine2.dll`, `server.dll` (no `client.dll` → skipped) |

If your server is a separate SteamCMD install, run the tool there too.

Originals are backed up next to each DLL as `*.orig64`. Undo checks the restored files byte-for-byte against the original SHA-256.

Command line:
```
cs2_128tick.exe status|patch|aggressive|undo [128|256] ["<CS2 folder or any DLL path>"]
```

### Windows warnings

The exe is unsigned and edits game DLLs, so SmartScreen and some antivirus tools may flag it. You can build it yourself from source (below).

## ⚠️ Build-locked

Offsets only match **one specific CS2 build**. Every file is SHA-256-checked before anything is written. After a CS2 update the tool shows *"unknown build"* and changes nothing, so the patch table has to be regenerated for the new build (see below).

Supported build (SHA-256):

| File | SHA-256 |
|---|---|
| `engine2.dll` | `edb6ab58c84727ce6f1ec0edbffd8155c16bebeb4ec641449a7eab5d83dfea0e` |
| `server.dll` | `7126c11b3c56fb3237c95238b76fec4cc6e6b433c06f5818b01f4e3dc56bc7ac` |
| `client.dll` | `07103341bc9e0ae2c596b2929075c5094cb397d7e370ea00986c720ed72414e1` |

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
python tools/gen_header.py         # -> src/patches.h + docs/patched_sites.txt
```

`build_patches.py` contains a couple of **build-specific addresses** in `engine2.dll`. Re-check these by hand for a new build:

- `EXTRA_SAFE`: the fixed-value check and the `FilterTime` frame-rate target.
- The constant slots, which are picked automatically from int3 padding. Check they're still unreferenced.

| Script | What it does |
|---|---|
| `analyze.py` | Linear-sweeps `.text` and records every use of 1/64, 64, 1/128 and 128 (float/double, RIP-relative or immediate) with surrounding instructions |
| `classify3.py` | Tags each use as a getter, `ticks→time`, or `time→ticks` (follows the register to `cvttss2si`), or as unknown |
| `build_patches.py` | Picks the sites per tier and redirects their displacement to the tool's own constant slots in int3 padding; slot values = rate / 1/rate |
| `verify.py` | Applies the patches to copies at 128 and 256 and re-disassembles every edited instruction |
| `gen_header.py` | Generates the C patch table for the app |

## Known limitations

- **Experimental.** Some timing code may still assume 64 tick in ways static analysis can't see.
- **Opponent movement:** if other players look choppy, check `cl_interp_ratio` (1 is a sensible value at 128 tick).
- **Clients need the patch too.** The client and server must both be patched; an unpatched client stays at 64.

## License

MIT, see [LICENSE](LICENSE). No Valve binaries are included in this repository. The patch table only contains file offsets and the few bytes being changed.
