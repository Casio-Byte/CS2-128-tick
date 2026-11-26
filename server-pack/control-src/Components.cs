using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text.RegularExpressions;

namespace CS2ServerControl
{
    /// Downloads and installs the server and its plugins. Nothing here is bundled in the pack:
    /// CS2 updates break plugins often, so the latest builds are fetched on the user's PC.
    internal sealed class Components
    {
        private readonly ServerCore _c;
        private readonly Action<string> _log;
        private readonly string _dl;

        public Components(ServerCore core, Action<string> log)
        {
            _c = core;
            _log = log;
            _dl = Path.Combine(core.PackDir, "downloads");
        }

        public sealed class GitHubPlugin
        {
            public string Name, Repo, FolderMatch, Note;
            public Func<string, bool> PickAsset;
        }

        public static readonly GitHubPlugin[] Plugins =
        {
            new GitHubPlugin
            {
                Name = "MatchZy", Repo = "shobhit-pathak/MatchZy", FolderMatch = "MatchZy",
                Note = "competitive / wingman / practice",
                PickAsset = n => n.EndsWith(".zip") && n.ToLowerInvariant().Contains("matchzy") && !n.ToLowerInvariant().Contains("cssharp"),
            },
        };

        // ------------------------------------------------------------ CS2 server (SteamCMD)

        public static string SteamCmdDir
        {
            get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CS2ServerControl", "steamcmd"); }
        }

        public static bool SteamCmdRunning()
        {
            try { return Process.GetProcessesByName("steamcmd").Length > 0; } catch { return false; }
        }

        public static void KillSteamCmd()
        {
            foreach (var p in Process.GetProcessesByName("steamcmd"))
            {
                try { p.Kill(); p.WaitForExit(5000); } catch { }
            }
        }

        public void InstallOrUpdateCs2(bool validate)
        {
            // SteamCMD must NOT live inside the server folder: it then refuses force_install_dir
            // ("Please set the game install path to something other than the Steam install folder")
            // and quietly installs CS2 into its own steamapps folder instead.
            var steamcmdDir = SteamCmdDir;
            var exe = Path.Combine(steamcmdDir, "steamcmd.exe");
            if (!File.Exists(exe))
            {
                _log("Downloading SteamCMD...");
                var zip = Path.Combine(_dl, "steamcmd.zip");
                Http.Download("https://steamcdn-a.akamaihd.net/client/installer/steamcmd.zip", zip, null);
                Directory.CreateDirectory(steamcmdDir);
                ZipFile.ExtractToDirectory(zip, steamcmdDir);
            }
            // Give SteamCMD Valve's own files to update (the 64-tick originals); the tick rate is put back afterwards.
            if (_c.Cs2Installed && _c.S.TickRate != 64)
            {
                _log("Tick rate " + _c.S.TickRate + ": restoring the original DLLs before the update...");
                try { TickPatch.ApplyAll(_c.Root, TickPatch.ServerFiles, 64, _log); }
                catch (Exception e) { _log("  " + e.Message + " (SteamCMD replaces them anyway)"); }
            }
            _log("SteamCMD: installing/updating the CS2 dedicated server (app 730). The first install is about 60 GB.");
            var args = "+force_install_dir \"" + _c.Root + "\" +login anonymous +app_update 730" + (validate ? " validate" : "") + " +quit";
            // SteamCMD updates itself on the first run and exits with code 7; just run it again.
            for (int attempt = 0; attempt < 3; attempt++)
            {
                int code = RunAndLog(exe, args, steamcmdDir);
                if (code == 0 || (code != 7 && _c.Cs2Installed)) break;
                _log("SteamCMD exited with " + code + ", running again...");
            }
            if (!_c.Cs2Installed)
            {
                var log = Path.Combine(steamcmdDir, "logs", "console_log.txt");
                throw new Exception("SteamCMD finished but cs2.exe is still missing in " + _c.Root + ". SteamCMD's own log: " + log);
            }
            _log("CS2 server build " + _c.Cs2Version() + " installed. Re-checking Metamod in gameinfo.gi...");
            _c.PatchGameInfo();
            // The update may have brought new engine2.dll / server.dll: patch them straight away to the chosen tick rate.
            if (_c.S.TickRate != 64)
            {
                try { _c.ApplyTickRate(); }
                catch (Exception e) { _log(e.Message); }
            }
        }

        private int RunAndLog(string exe, string args, string dir)
        {
            var psi = new ProcessStartInfo(exe, args)
            {
                WorkingDirectory = dir, UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
            };
            using (var p = new Process { StartInfo = psi })
            {
                // Progress lines arrive in big buffered bursts; the log file poll below reports progress instead.
                p.OutputDataReceived += (s, e) => { if (!string.IsNullOrWhiteSpace(e.Data) && !e.Data.Contains("Update state (")) _log("  " + e.Data.Trim()); };
                p.ErrorDataReceived += (s, e) => { if (!string.IsNullOrWhiteSpace(e.Data)) _log("  " + e.Data.Trim()); };
                p.Start();
                p.BeginOutputReadLine();
                p.BeginErrorReadLine();
                // SteamCMD buffers its redirected output, so progress is read from its own log instead.
                var consoleLog = Path.Combine(dir, "logs", "console_log.txt");
                string lastShown = null;
                while (!p.WaitForExit(30000))
                {
                    var line = LastProgressLine(consoleLog);
                    if (line != null && line != lastShown) { lastShown = line; _log("  " + line); }
                }
                p.WaitForExit();
                return p.ExitCode;
            }
        }

        /// "Update state (0x61) downloading, progress: 3.81 (2734284972 / 71799502864)" -> "CS2 download: 3.8% (2.7 of 71.8 GB)"
        private static string LastProgressLine(string logFile)
        {
            try
            {
                if (!File.Exists(logFile)) return null;
                using (var fs = new FileStream(logFile, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                {
                    var len = fs.Length;
                    fs.Seek(Math.Max(0, len - 4096), SeekOrigin.Begin);
                    var tail = new StreamReader(fs).ReadToEnd().Split('\n');
                    for (int i = tail.Length - 1; i >= 0; i--)
                    {
                        var m = Regex.Match(tail[i], @"Update state \(0x[0-9a-f]+\) (\w+), progress: ([\d.]+) \((\d+) / (\d+)\)");
                        if (!m.Success) continue;
                        double done = double.Parse(m.Groups[3].Value) / 1e9, total = double.Parse(m.Groups[4].Value) / 1e9;
                        return string.Format(System.Globalization.CultureInfo.InvariantCulture,
                            "CS2 {0}: {1:0.0}% ({2:0.0} of {3:0.0} GB)", m.Groups[1].Value, double.Parse(m.Groups[2].Value, System.Globalization.CultureInfo.InvariantCulture), done, total);
                    }
                }
            }
            catch { }
            return null;
        }

        // ------------------------------------------------------------ Metamod 2.0

        public void InstallMetamod()
        {
            _log("Metamod:Source 2.0: finding the latest Windows build...");
            var name = Http.Get("https://mms.alliedmods.net/mmsdrop/2.0/mmsource-latest-windows").Trim();
            if (!name.EndsWith(".zip")) throw new Exception("Unexpected answer from mms.alliedmods.net: " + name);
            var zip = Path.Combine(_dl, name);
            if (!File.Exists(zip)) Http.Download("https://mms.alliedmods.net/mmsdrop/2.0/" + name, zip, Progress("Metamod"));
            var n = ExtractSmart(zip, "metamod");
            _log("Metamod " + name + ": " + n + " files.");
            var mv = Regex.Match(name, @"mmsource-([\d.]+-git\d+)");
            Versions.Set(_c, "metamod", mv.Success ? mv.Groups[1].Value : name);
            _c.PatchGameInfo();
        }

        // ------------------------------------------------------------ CounterStrikeSharp

        public void InstallCss()
        {
            _log("CounterStrikeSharp: checking the latest release...");
            var rel = Json.ParseObject(Http.Get("https://api.github.com/repos/roflmuffin/CounterStrikeSharp/releases/latest"));
            var tag = Json.Str(rel, "tag_name");
            bool hasRuntime = Directory.Exists(Path.Combine(_c.Css, "dotnet"));
            var assets = Assets(rel);
            // The runtime build is big (~70 MB) but needed once, or when the .NET version moves on.
            var asset = assets.FirstOrDefault(a => a.Key.Contains("windows") && a.Key.Contains("with-runtime") == !hasRuntime);
            if (asset.Key == null) asset = assets.FirstOrDefault(a => a.Key.Contains("windows") && a.Key.Contains("with-runtime"));
            if (asset.Key == null) throw new Exception("No Windows build in CounterStrikeSharp " + tag);
            var zip = Path.Combine(_dl, asset.Key);
            if (!File.Exists(zip)) Http.Download(asset.Value, zip, Progress("CounterStrikeSharp"));
            var n = ExtractSmart(zip, "counterstrikesharp");
            _log("CounterStrikeSharp " + tag + (hasRuntime ? "" : " (with .NET runtime)") + ": " + n + " files.");
            Versions.Set(_c, "counterstrikesharp", tag);
        }

        /// The runtime build again, e.g. when CounterStrikeSharp moves to a new .NET version.
        public void ReinstallCssRuntime()
        {
            var dotnet = Path.Combine(_c.Css, "dotnet");
            if (Directory.Exists(dotnet)) Directory.Delete(dotnet, true);
            InstallCss();
        }

        // ------------------------------------------------------------ GitHub plugins

        public void InstallPlugin(GitHubPlugin p)
        {
            _log(p.Name + ": checking " + p.Repo + "...");
            var rel = Json.ParseObject(Http.Get("https://api.github.com/repos/" + p.Repo + "/releases/latest"));
            var tag = Json.Str(rel, "tag_name");
            var asset = Assets(rel).FirstOrDefault(a => p.PickAsset(a.Key));
            if (asset.Key == null) throw new Exception("No matching download in " + p.Repo + " " + tag);
            var zip = Path.Combine(_dl, asset.Key);
            if (!File.Exists(zip)) Http.Download(asset.Value, zip, Progress(p.Name));
            var n = ExtractSmart(zip, p.Name);
            _log(p.Name + " " + tag + ": " + n + " files.");
            Versions.Set(_c, p.Name, tag);
        }

        private static List<KeyValuePair<string, string>> Assets(Dictionary<string, object> rel)
        {
            var list = new List<KeyValuePair<string, string>>();
            object v;
            if (rel.TryGetValue("assets", out v) && v is object[])
                foreach (var o in (object[])v)
                {
                    var a = o as Dictionary<string, object>;
                    if (a == null) continue;
                    list.Add(new KeyValuePair<string, string>(Json.Str(a, "name"), Json.Str(a, "browser_download_url")));
                }
            return list;
        }

        // ------------------------------------------------------------ all

        public void InstallAll(bool includeCs2)
        {
            if (includeCs2 || !_c.Cs2Installed) InstallOrUpdateCs2(false);
            InstallMetamod();
            InstallCss();
            foreach (var p in Plugins)
            {
                try { InstallPlugin(p); }
                catch (Exception e) { _log(p.Name + " FAILED: " + e.Message); }
            }
            _c.PatchGameInfo();
            _log("All done.");
        }

        public List<string> Status()
        {
            var l = new List<string>();
            l.Add("CS2 server: " + (_c.Cs2Installed ? "installed, build " + _c.Cs2Version() : "not installed"));
            l.Add("Metamod 2.0: " + (_c.MetamodInstalled ? "installed" : "missing") +
                  (File.Exists(_c.GameInfo) ? (File.ReadAllText(_c.GameInfo).Contains("csgo/addons/metamod") ? ", in gameinfo.gi" : ", NOT in gameinfo.gi (fixed on Start)") : ""));
            l.Add("CounterStrikeSharp: " + (_c.CssInstalled ? "installed" + (Directory.Exists(Path.Combine(_c.Css, "dotnet")) ? " (with runtime)" : " (runtime MISSING)") : "missing"));
            var plugins = Path.Combine(_c.Css, "plugins");
            foreach (var p in Plugins)
                l.Add(p.Name + ": " + (FindPlugin(plugins, p.FolderMatch) ?? "missing") + "  (" + p.Note + ")");
            l.Add("CS2 Match (pack, game modes): " + (File.Exists(Path.Combine(plugins, "CS2Match", "CS2Match.dll")) ? "installed" : "missing"));
            return l;
        }

        private static string FindPlugin(string dir, string match)
        {
            if (!Directory.Exists(dir)) return null;
            foreach (var d in Directory.GetDirectories(dir))
            {
                var n = Path.GetFileName(d);
                if (n.IndexOf(match, StringComparison.OrdinalIgnoreCase) >= 0 && File.Exists(Path.Combine(d, n + ".dll"))) return "installed (" + n + ")";
            }
            return null;
        }

        private Action<int> Progress(string what)
        {
            int last = -1;
            return pct =>
            {
                if (pct / 20 != last) { last = pct / 20; _log("  " + what + ": " + pct + "%"); }
            };
        }

        // ------------------------------------------------------------ zip layout

        /// Plugin zips come in every layout (addons/..., counterstrikesharp/..., plugins/..., a bare plugin folder).
        /// Everything is mapped onto game/csgo. Existing config files are never overwritten.
        public int ExtractSmart(string zipPath, string what)
        {
            int count = 0;
            using (var z = ZipFile.OpenRead(zipPath))
            {
                var files = z.Entries.Where(e => !string.IsNullOrEmpty(e.Name)).ToList();
                var paths = files.Select(e => e.FullName.Replace('\\', '/')).ToList();
                // Strip one wrapper folder if everything sits in it and it isn't a known folder.
                string wrapper = null;
                var firsts = paths.Select(p => p.Contains('/') ? p.Substring(0, p.IndexOf('/')) : "").Distinct().ToList();
                var known = new[] { "addons", "cfg", "counterstrikesharp", "plugins", "shared", "configs", "gamedata", "metamod" };
                if (firsts.Count == 1 && firsts[0].Length > 0 && !known.Contains(firsts[0].ToLowerInvariant()))
                    wrapper = firsts[0] + "/";

                // Top-level folders that are plugin folders on their own (X/X.dll).
                var stripped = paths.Select(p => wrapper != null ? p.Substring(wrapper.Length) : p).ToList();
                var pluginFolders = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach (var p in stripped)
                {
                    var parts = p.Split('/');
                    if (parts.Length == 2 && parts[1].Equals(parts[0] + ".dll", StringComparison.OrdinalIgnoreCase)) pluginFolders.Add(parts[0]);
                }

                for (int i = 0; i < files.Count; i++)
                {
                    var rel = paths[i];
                    if (wrapper != null) rel = rel.Substring(wrapper.Length);
                    // A wrapper folder with no addons/plugins inside is the plugin folder itself (its name must match the dll).
                    var target = MapEntry(rel, wrapper != null && !HasKnownRoot(paths, wrapper) ? wrapper.TrimEnd('/') : null, pluginFolders);
                    if (target == null) continue;
                    var dest = Path.GetFullPath(Path.Combine(_c.Csgo, target.Replace('/', Path.DirectorySeparatorChar)));
                    if (!dest.StartsWith(Path.GetFullPath(_c.Csgo), StringComparison.OrdinalIgnoreCase)) continue; // zip slip
                    if (File.Exists(dest) && IsUserConfig(target)) continue;
                    Directory.CreateDirectory(Path.GetDirectoryName(dest));
                    try
                    {
                        files[i].ExtractToFile(dest, true);
                        count++;
                    }
                    catch (IOException e)
                    {
                        throw new IOException(target + " is in use. Stop the server before updating. (" + e.Message + ")");
                    }
                }
            }
            return count;
        }

        private static bool HasKnownRoot(List<string> paths, string wrapper)
        {
            foreach (var p in paths)
            {
                var rel = p.Substring(wrapper.Length).ToLowerInvariant();
                if (rel.StartsWith("addons/") || rel.StartsWith("cfg/") || rel.StartsWith("counterstrikesharp/") ||
                    rel.StartsWith("plugins/") || rel.StartsWith("shared/") || rel.StartsWith("configs/") || rel.StartsWith("gamedata/"))
                    return true;
            }
            return false;
        }

        private static string MapEntry(string rel, string bareFolder, HashSet<string> pluginFolders)
        {
            var low = rel.ToLowerInvariant();
            if (bareFolder != null) return "addons/counterstrikesharp/plugins/" + bareFolder + "/" + rel;
            var top = rel.Contains("/") ? rel.Substring(0, rel.IndexOf('/')) : "";
            if (top.Length > 0 && pluginFolders.Contains(top)) return "addons/counterstrikesharp/plugins/" + rel;
            if (low.StartsWith("addons/")) return rel;
            if (low.Contains("/addons/")) return rel.Substring(low.IndexOf("/addons/") + 1);
            if (low.StartsWith("counterstrikesharp/")) return "addons/" + rel;
            if (low.Contains("/counterstrikesharp/")) return "addons/" + rel.Substring(low.IndexOf("/counterstrikesharp/") + 1);
            if (low.StartsWith("cfg/")) return rel;
            foreach (var k in new[] { "plugins/", "shared/", "configs/", "gamedata/" })
                if (low.StartsWith(k)) return "addons/counterstrikesharp/" + rel;
            return null;
        }

        private static bool IsUserConfig(string target)
        {
            var low = target.ToLowerInvariant();
            if (low.EndsWith(".example.json")) return false;
            if (low.StartsWith("cfg/") && low.EndsWith(".cfg")) return true;
            if (low.Contains("/configs/") && (low.EndsWith(".json") || low.EndsWith(".cfg") || low.EndsWith(".toml"))) return true;
            if (low.EndsWith("admins.json") || low.EndsWith("database.json")) return true;
            return false;
        }
    }
}
