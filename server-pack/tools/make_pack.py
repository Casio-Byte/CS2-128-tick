#!/usr/bin/env python3
"""Builds CS2-Server-Pack.zip from this folder.

Usage:
    python server-pack/tools/make_pack.py VENDOR_DIR [OUT_DIR]

VENDOR_DIR holds the third-party downloads, unchanged:
    mmsource-2.0.0-git1491-windows.zip                   https://www.metamodsource.net/downloads.php?branch=dev
    counterstrikesharp-with-runtime-windows-1.0.380.zip  https://github.com/roflmuffin/CounterStrikeSharp/releases
    MatchZy-0.9.3.zip                                    https://github.com/shobhit-pathak/MatchZy/releases

Build these first (see server-pack/BUILD.md):
    cs2_128tick.exe                                  repo root (build.sh / build.bat)
    server-pack/CS2 Server Control.exe               mcs / csc
    server-pack/plugin-src/CS2Match/bin/Release/CS2Match.dll   dotnet build -c Release
"""
import json, os, shutil, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
PACK = os.path.dirname(HERE)            # server-pack/
REPO = os.path.dirname(PACK)

VENDOR = [  # zip, versions.json key, version, label
    ("mmsource-2.0.0-git1491-windows.zip", "metamod", "2.0.0-git1491", "Metamod:Source 2.0.0-git1491"),
    ("counterstrikesharp-with-runtime-windows-1.0.380.zip", "counterstrikesharp", "v1.0.380", "CounterStrikeSharp v1.0.380 (with .NET runtime)"),
    ("MatchZy-0.9.3.zip", "MatchZy", "0.9.3", "MatchZy 0.9.3"),
]


def target(rel, plugin_folders):
    """Where a file from a plugin zip goes under game/csgo (same rules as the control program)."""
    low = rel.lower()
    top = rel.split("/")[0] if "/" in rel else ""
    if top in plugin_folders:
        return "addons/counterstrikesharp/plugins/" + rel
    if low.startswith("addons/") or low.startswith("cfg/"):
        return rel
    if "/addons/" in low:
        return rel[low.index("/addons/") + 1:]
    for k in ("plugins/", "shared/", "configs/", "gamedata/"):
        if low.startswith(k):
            return "addons/counterstrikesharp/" + rel
    return None


def need(path):
    if not os.path.exists(path):
        sys.exit("missing: " + path)
    return path


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    vendor = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(REPO, "dist")
    stage = os.path.join(out, "CS2-Server-Pack")
    csgo = os.path.join(stage, "game", "csgo")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(csgo)

    # 1. Metamod, CounterStrikeSharp (+ .NET runtime), MatchZy - unchanged
    versions, labels = {}, []
    for zname, key, ver, label in VENDOR:
        z = zipfile.ZipFile(need(os.path.join(vendor, zname)))
        names = [n for n in z.namelist() if not n.endswith("/")]
        pf = {n.split("/")[0] for n in names
              if n.count("/") == 1 and n.split("/")[1].lower() == n.split("/")[0].lower() + ".dll"}
        count = 0
        for n in names:
            t = target(n, pf)
            if t is None or ".." in t:
                continue
            # MatchZy ships SQLite for every OS; a Windows server only needs win-x64.
            if "/runtimes/" in t.lower() and "/plugins/" in t.lower() and "/runtimes/win-x64/" not in t.lower():
                continue
            dest = os.path.join(csgo, t)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with z.open(n) as s, open(dest, "wb") as d:
                d.write(s.read())
            count += 1
        versions[key] = ver
        labels.append(label)
        print(f"{label}: {count} files")

    # 2. this pack: mode configs, the CS2 Match plugin, the control program and the tick patcher
    shutil.copytree(os.path.join(PACK, "game", "csgo"), csgo, dirs_exist_ok=True)
    plug = os.path.join(csgo, "addons", "counterstrikesharp", "plugins", "CS2Match")
    os.makedirs(plug, exist_ok=True)
    shutil.copy2(need(os.path.join(PACK, "plugin-src", "CS2Match", "bin", "Release", "CS2Match.dll")), plug)
    shutil.copy2(need(os.path.join(PACK, "CS2 Server Control.exe")), csgo)
    shutil.copy2(need(os.path.join(REPO, "cs2_128tick.exe")), csgo)

    os.makedirs(os.path.join(csgo, "cs2pack"), exist_ok=True)
    with open(os.path.join(csgo, "cs2pack", "versions.json"), "w") as f:
        json.dump(versions, f, indent=2)
    with open(os.path.join(csgo, "cs2pack", "bundled_versions.json"), "w") as f:
        json.dump({"bundled": labels}, f, indent=2)

    shutil.copy2(os.path.join(PACK, "README.md"), os.path.join(csgo, "README_server_pack.txt"))
    shutil.copy2(os.path.join(PACK, "README.md"), os.path.join(stage, "README.txt"))
    shutil.copy2(os.path.join(PACK, "LICENSE"), os.path.join(stage, "LICENSE.txt"))

    # 3. zip
    zpath = os.path.join(out, "CS2-Server-Pack.zip")
    if os.path.exists(zpath):
        os.remove(zpath)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for root, _, files in os.walk(stage):
            for fn in sorted(files):
                full = os.path.join(root, fn)
                z.write(full, os.path.relpath(full, out))
    print("wrote", zpath, os.path.getsize(zpath) // 1024, "KB")


if __name__ == "__main__":
    main()
