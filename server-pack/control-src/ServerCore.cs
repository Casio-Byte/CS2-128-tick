using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

namespace CS2ServerControl
{
    internal static class Modes
    {
        public static readonly string[] Keys = { "competitive", "wingman", "casual", "deathmatch", "armsrace", "practice", "duels", "arenas" };
        public static readonly string[] Titles = { "Competitive 5v5", "Wingman 2v2", "Casual", "Deathmatch (FFA)", "Arms Race", "Practice", "1v1 duels", "Multi-1v1 arenas" };

        public static void GameTypeMode(string key, out int type, out int mode)
        {
            switch (key)
            {
                case "competitive": case "practice": type = 0; mode = 1; break;
                case "wingman": type = 0; mode = 2; break;
                case "armsrace": type = 1; mode = 0; break;
                case "deathmatch": type = 1; mode = 2; break;
                default: type = 0; mode = 0; break;
            }
        }

        public static string[] ForMapName(string name)
        {
            var n = name.ToLowerInvariant();
            if (n.StartsWith("am_")) return new[] { "arenas" };
            if (n.StartsWith("aim_") || n.StartsWith("awp_") || n.StartsWith("1v1_")) return new[] { "duels" };
            if (n.StartsWith("ar_")) return new[] { "armsrace" };
            if (n.StartsWith("kz_") || n.StartsWith("bhop_") || n.StartsWith("surf_")) return new string[0];
            return new[] { "competitive", "wingman", "casual", "deathmatch", "practice" };
        }
    }

    internal sealed class LauncherSettings
    {
        public string Hostname = "CS2 Server Pack";
        public string ServerPassword = "";
        public string RconPassword = "";
        public string AdminPassword = "";
        public int MaxPlayers = 12;
        public int Port = 27015;
        public string Network = "lan";      // lan | internet
        public string Gslt = "";
        public string DefaultMode = "competitive";
        public string StartMap = "de_dust2";
        public bool Insecure = true;
        public int TickRate = 64;           // 64 (stock), 128 or 256: built-in DLL patch
        public bool AutoRestart = true;
        public bool UpdateCs2OnStart;
        public bool UpdatePluginsOnStart;
        public bool CheckUpdates = true;
        public bool PluginsOff;             // start without CounterStrikeSharp (plain server, e.g. after a CS2 update broke it)
        public bool ShowServerWindow;
        public int Bots;

        public Dictionary<string, object> ToDict()
        {
            return new Dictionary<string, object>
            {
                { "hostname", Hostname }, { "sv_password", ServerPassword }, { "rcon_password", RconPassword },
                { "admin_password", AdminPassword }, { "maxplayers", MaxPlayers }, { "port", Port },
                { "network", Network }, { "gslt", Gslt }, { "default_mode", DefaultMode }, { "start_map", StartMap },
                { "insecure", Insecure }, { "tick_rate", TickRate },
                { "auto_restart", AutoRestart },
                { "update_cs2_on_start", UpdateCs2OnStart }, { "update_plugins_on_start", UpdatePluginsOnStart }, { "check_updates", CheckUpdates }, { "plugins_off", PluginsOff },
                { "show_server_window", ShowServerWindow }, { "bots", Bots },
            };
        }

        public static LauncherSettings From(Dictionary<string, object> d)
        {
            var s = new LauncherSettings();
            if (d.Count == 0) return s;
            s.Hostname = Json.Str(d, "hostname", s.Hostname);
            s.ServerPassword = Json.Str(d, "sv_password");
            s.RconPassword = Json.Str(d, "rcon_password");
            s.AdminPassword = Json.Str(d, "admin_password");
            s.MaxPlayers = Json.Int(d, "maxplayers", s.MaxPlayers);
            s.Port = Json.Int(d, "port", s.Port);
            s.Network = Json.Str(d, "network", s.Network);
            s.Gslt = Json.Str(d, "gslt");
            s.DefaultMode = Json.Str(d, "default_mode", s.DefaultMode);
            if (!Modes.Keys.Contains(s.DefaultMode)) s.DefaultMode = "competitive";
            s.StartMap = Json.Str(d, "start_map", s.StartMap);
            s.Insecure = Json.Bool(d, "insecure", s.Insecure);
            s.TickRate = Json.Int(d, "tick_rate", Json.Bool(d, "tick128", false) ? 128 : 64);
            if (!TickPatch.Rates.Contains(s.TickRate)) s.TickRate = 64;
            s.AutoRestart = Json.Bool(d, "auto_restart", true);
            s.UpdateCs2OnStart = Json.Bool(d, "update_cs2_on_start", false);
            s.UpdatePluginsOnStart = Json.Bool(d, "update_plugins_on_start", false);
            s.CheckUpdates = Json.Bool(d, "check_updates", true);
            s.PluginsOff = Json.Bool(d, "plugins_off", false);
            s.ShowServerWindow = Json.Bool(d, "show_server_window", false);
            s.Bots = Json.Int(d, "bots", 0);
            return s;
        }
    }

    /// Paths, config writing and the server process.
    internal sealed class ServerCore
    {
        public readonly string Root, Game, Csgo, Cs2Exe, GameInfo, Css, Metamod, PackDir, SettingsFile;
        public LauncherSettings S;
        public event Action<string> Log = delegate { };
        public event Action StateChanged = delegate { };

        private Process _proc;
        private volatile bool _stopping;
        private readonly List<DateTime> _crashes = new List<DateTime>();

        public ServerCore()
        {
            var exeDir = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\', '/');
            // Normal place: <server>\game\csgo\CS2 Server Control.exe
            var parent = Directory.GetParent(exeDir);
            if (Path.GetFileName(exeDir).Equals("csgo", StringComparison.OrdinalIgnoreCase) && parent != null &&
                parent.Name.Equals("game", StringComparison.OrdinalIgnoreCase) && parent.Parent != null)
                Root = parent.Parent.FullName;
            else if (Directory.Exists(Path.Combine(exeDir, "game", "csgo")))
                Root = exeDir;
            else
                Root = exeDir;

            Game = Path.Combine(Root, "game");
            Csgo = Path.Combine(Game, "csgo");
            Cs2Exe = Path.Combine(Game, "bin", "win64", "cs2.exe");
            GameInfo = Path.Combine(Csgo, "gameinfo.gi");
            Css = Path.Combine(Csgo, "addons", "counterstrikesharp");
            Metamod = Path.Combine(Csgo, "addons", "metamod");
            PackDir = Path.Combine(Csgo, "cs2pack");
            SettingsFile = Path.Combine(PackDir, "launcher_settings.json");
            S = LauncherSettings.From(Json.LoadObject(SettingsFile));
        }

        public string MatchCfgDir { get { return Path.Combine(Css, "configs", "plugins", "CS2Match"); } }
        public string MapsFile { get { return Path.Combine(MatchCfgDir, "maps.json"); } }
        /// Where CS2 puts the con_logfile file differs between builds; the newest candidate wins.
        public string ConsoleLog
        {
            get
            {
                // With Metamod first in gameinfo.gi, CS2's write path is csgo/addons/metamod.
                var candidates = new[] { Path.Combine(Metamod, "console.log"), Path.Combine(Csgo, "console.log") };
                string best = candidates[0];
                DateTime newest = DateTime.MinValue;
                foreach (var c in candidates)
                {
                    try
                    {
                        if (!File.Exists(c)) continue;
                        var t = File.GetLastWriteTimeUtc(c);
                        if (t > newest) { newest = t; best = c; }
                    }
                    catch { }
                }
                return best;
            }
        }
        public bool Cs2Installed { get { return File.Exists(Cs2Exe); } }
        public bool MetamodInstalled { get { return Directory.Exists(Path.Combine(Metamod, "bin")); } }
        public bool CssInstalled { get { return File.Exists(Path.Combine(Css, "api", "CounterStrikeSharp.API.dll")); } }
        public bool Running { get { try { return _proc != null && !_proc.HasExited; } catch { return false; } } }

        public string Cs2Version()
        {
            try
            {
                var inf = Path.Combine(Csgo, "steam.inf");
                if (!File.Exists(inf)) return "";
                foreach (var l in File.ReadAllLines(inf))
                    if (l.StartsWith("PatchVersion=")) return l.Substring(13).Trim();
            }
            catch { }
            return "";
        }

        public void SaveSettings()
        {
            if (string.IsNullOrEmpty(S.RconPassword)) S.RconPassword = Rand.Password(16);
            Json.Save(SettingsFile, S.ToDict());
            WriteGameFiles();
        }

        // ------------------------------------------------------------ files the setup owns

        public void WriteGameFiles()
        {
            var cfgDir = Path.Combine(Csgo, "cfg", "cs2pack");
            Directory.CreateDirectory(cfgDir);
            bool lan = S.Network != "internet";
            var sb = new StringBuilder();
            sb.AppendLine("// Written by CS2 Server Control.exe (Setup tab). Changes here are overwritten.");
            sb.AppendLine("hostname \"" + S.Hostname.Replace("\"", "'") + "\"");
            sb.AppendLine("sv_password \"" + S.ServerPassword.Replace("\"", "") + "\"");
            sb.AppendLine("rcon_password \"" + S.RconPassword.Replace("\"", "") + "\"");
            sb.AppendLine("sv_lan " + (lan ? 1 : 0));
            sb.AppendLine("bot_quota " + S.Bots);
            sb.AppendLine("bot_quota_mode fill");
            File.WriteAllText(Path.Combine(cfgDir, "server_settings.cfg"), sb.ToString());

            FixServerCfg();

            // CS2 Match plugin settings: only the keys the setup owns are replaced.
            var mpath = Path.Combine(MatchCfgDir, "settings.json");
            var m = Json.LoadObject(mpath);
            m["admin_password"] = S.AdminPassword;
            m["default_mode"] = S.DefaultMode;
            Json.Save(mpath, m);

            // No skins in the pack: keep CounterStrikeSharp's "follow CS2 server guidelines" on.
            SetCssGuidelines(true);
        }

        /// server.cfg runs on every map: it must not fight the setup, and it loads our settings.
        private void FixServerCfg()
        {
            var path = Path.Combine(Csgo, "cfg", "server.cfg");
            var owned = new[] { "hostname", "sv_password", "rcon_password", "sv_lan" };
            var lines = File.Exists(path) ? File.ReadAllLines(path).ToList() : new List<string>();
            bool changed = false, hasExec = false;
            for (int i = 0; i < lines.Count; i++)
            {
                var t = lines[i].Trim();
                if (t.StartsWith("//")) continue;
                if (t.StartsWith("exec cs2pack/server_settings")) { hasExec = true; continue; }
                var first = t.Split(new[] { ' ', '\t' }, 2)[0].ToLowerInvariant();
                if (owned.Contains(first))
                {
                    lines[i] = "// (set in CS2 Server Control.exe) " + lines[i];
                    changed = true;
                }
            }
            if (!hasExec)
            {
                lines.Add("");
                lines.Add("// CS2 Server Pack: name, passwords and LAN/internet from CS2 Server Control.exe");
                lines.Add("exec cs2pack/server_settings.cfg");
                changed = true;
            }
            if (changed || !File.Exists(path)) File.WriteAllLines(path, lines.ToArray());
        }

        /// CounterStrikeSharp blocks some econ changes unless FollowCS2ServerGuidelines is off.
        public void SetCssGuidelines(bool follow)
        {
            var dir = Path.Combine(Css, "configs");
            var core = Path.Combine(dir, "core.json");
            if (!File.Exists(core))
            {
                var ex = Path.Combine(dir, "core.example.json");
                if (!File.Exists(ex)) return;
                File.Copy(ex, core);
            }
            var txt = File.ReadAllText(core);
            var re = new Regex("(\"FollowCS2ServerGuidelines\"\\s*:\\s*)(true|false)", RegexOptions.IgnoreCase);
            var want = follow ? "true" : "false";
            var updated = re.Replace(txt, "${1}" + want);
            if (updated != txt) File.WriteAllText(core, updated);
        }

        /// Metamod only loads when gameinfo.gi lists it. CS2 updates overwrite the file, so this runs before every start.
        public bool PatchGameInfo()
        {
            if (!File.Exists(GameInfo)) return false;
            var txt = File.ReadAllText(GameInfo);
            if (txt.IndexOf("csgo/addons/metamod", StringComparison.OrdinalIgnoreCase) >= 0) return true;
            var m = Regex.Match(txt, @"^([ \t]*)Game_LowViolence[ \t]+csgo_lv[^\r\n]*\r?\n", RegexOptions.Multiline);
            if (!m.Success)
            {
                Log("gameinfo.gi: couldn't find the Game_LowViolence line, Metamod was NOT added. Check the file by hand.");
                return false;
            }
            var nl = m.Value.EndsWith("\r\n") ? "\r\n" : "\n";
            var insert = m.Groups[1].Value + "Game\tcsgo/addons/metamod" + nl;
            txt = txt.Insert(m.Index + m.Length, insert);
            File.Copy(GameInfo, GameInfo + ".before_metamod", true);
            File.WriteAllText(GameInfo, txt);
            Log("gameinfo.gi: added Metamod.");
            return true;
        }

        // ------------------------------------------------------------ maps / modes

        public List<Dictionary<string, object>> LoadMaps()
        {
            var d = Json.LoadObject(MapsFile);
            var list = new List<Dictionary<string, object>>();
            object v;
            if (d.TryGetValue("maps", out v) && v is object[])
                foreach (var o in (object[])v)
                    if (o is Dictionary<string, object>) list.Add((Dictionary<string, object>)o);
            if (list.Count == 0)
                foreach (var n in new[] { "de_dust2", "de_mirage", "de_inferno", "de_nuke", "de_ancient", "de_anubis", "de_overpass", "de_train", "de_vertigo", "cs_office", "cs_italy", "ar_baggage", "ar_shoots", "ar_pool_day" })
                    list.Add(new Dictionary<string, object> { { "name", n }, { "workshop_id", "" }, { "modes", new object[0] } });
            return list;
        }

        public void SaveMaps(List<Dictionary<string, object>> maps)
        {
            Json.Save(MapsFile, new Dictionary<string, object> { { "maps", maps } });
        }

        public string CurrentMode()
        {
            var st = Json.LoadObject(Path.Combine(MatchCfgDir, "state.json"));
            var m = Json.Str(st, "mode");
            return Modes.Keys.Contains(m) ? m : (Modes.Keys.Contains(S.DefaultMode) ? S.DefaultMode : "competitive");
        }

        public void SetCurrentMode(string mode)
        {
            var path = Path.Combine(MatchCfgDir, "state.json");
            var st = Json.LoadObject(path);
            st["mode"] = mode;
            Json.Save(path, st);
        }

        // ------------------------------------------------------------ process

        public string BuildArgs()
        {
            var mode = CurrentMode();
            int gt, gm;
            Modes.GameTypeMode(mode, out gt, out gm);
            // +ip 0.0.0.0: CS2 binds RCON (TCP) to the server's address only otherwise, and 127.0.0.1 is refused.
            // -condebug writes console.log into the first game search path, csgo/addons/metamod (see ConsoleLog).
            var a = new StringBuilder("-dedicated -console -usercon -condebug +ip 0.0.0.0");
            a.Append(" -port ").Append(S.Port);
            a.Append(" -maxplayers ").Append(Math.Max(2, Math.Min(64, S.MaxPlayers)));
            if (S.Network == "internet")
            {
                if (!string.IsNullOrEmpty(S.Gslt)) a.Append(" +sv_setsteamaccount ").Append(S.Gslt);
                if (S.Insecure || S.TickRate != 64) a.Append(" -insecure");
            }
            else
            {
                a.Append(" +sv_lan 1");
                if (S.Insecure || S.TickRate != 64) a.Append(" -insecure");
            }
            a.Append(" +game_type ").Append(gt).Append(" +game_mode ").Append(gm);
            a.Append(" +exec cs2pack/server_settings.cfg");

            var map = (S.StartMap ?? "").Trim();
            var entry = LoadMaps().FirstOrDefault(x => Json.Str(x, "name").Equals(map, StringComparison.OrdinalIgnoreCase));
            var wid = entry != null ? Json.Str(entry, "workshop_id") : "";
            if (!string.IsNullOrEmpty(wid)) a.Append(" +host_workshop_map ").Append(wid);
            else a.Append(" +map ").Append(string.IsNullOrEmpty(map) ? "de_dust2" : map);
            return a.ToString();
        }

        public void Start()
        {
            if (Running) return;
            if (!Cs2Installed) throw new InvalidOperationException("cs2.exe not found at " + Cs2Exe + ". Install the CS2 server first (Components tab).");
            _stopping = false;
            SaveSettings();
            PatchGameInfo();
            RemoveOldExtras();
            ApplyTickRate();
            ApplyPluginsSwitch();
            if (S.TickRate != 64 && !S.Insecure) Log("Tick rate " + S.TickRate + ": starting with -insecure (patched DLLs must never meet VAC).");

            foreach (var old in new[] { Path.Combine(Metamod, "console.log"), Path.Combine(Csgo, "console.log") })
                try { if (File.Exists(old)) File.Delete(old); } catch { }
            var psi = new ProcessStartInfo(Cs2Exe, BuildArgs())
            {
                WorkingDirectory = Path.GetDirectoryName(Cs2Exe),
                UseShellExecute = false,
                CreateNoWindow = !S.ShowServerWindow,
            };
            Log("Starting: cs2.exe " + MaskArgs(psi.Arguments));
            _proc = Process.Start(psi);
            _proc.EnableRaisingEvents = true;
            _proc.Exited += OnExited;
            StateChanged();
        }

        private string MaskArgs(string a)
        {
            return string.IsNullOrEmpty(S.Gslt) ? a : a.Replace(S.Gslt, "<token>");
        }

        private void OnExited(object sender, EventArgs e)
        {
            int code = -1;
            try { code = _proc.ExitCode; } catch { }
            StateChanged();
            if (_stopping) { Log("Server stopped."); return; }
            Log("Server exited (code " + code + ").");
            if (!S.AutoRestart) return;
            _crashes.Add(DateTime.UtcNow);
            _crashes.RemoveAll(t => t < DateTime.UtcNow.AddMinutes(-5));
            if (_crashes.Count > 5) { Log("Crashed more than 5 times in 5 minutes: not restarting. Check the console log. If CS2 updated recently, Metamod or CounterStrikeSharp may need an update first: Components > Check for updates." +
                    (S.PluginsOff ? "" : " To play now without the plugins, tick 'Run without plugins' in Setup and Start again.")); return; }
            Log("Restarting in 5 seconds...");
            ThreadPool.QueueUserWorkItem(_ =>
            {
                Thread.Sleep(5000);
                if (_stopping) return;
                try { Start(); } catch (Exception ex) { Log("Restart failed: " + ex.Message); }
            });
        }

        public void Stop()
        {
            _stopping = true;
            if (!Running) return;
            try { Rcon.Run("127.0.0.1", S.Port, S.RconPassword, "quit", 2000); } catch { }
            try
            {
                if (!_proc.WaitForExit(8000)) _proc.Kill();
            }
            catch { }
            StateChanged();
        }

        private string _rconHost;

        /// Tries 127.0.0.1 first, then this PC's LAN addresses (in case CS2 bound RCON to one of them).
        public string RconExec(string cmd)
        {
            var hosts = new List<string>();
            if (_rconHost != null) hosts.Add(_rconHost);
            hosts.Add("127.0.0.1");
            try
            {
                foreach (var ip in System.Net.Dns.GetHostEntry(System.Net.Dns.GetHostName()).AddressList)
                    if (ip.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork) hosts.Add(ip.ToString());
            }
            catch { }
            Exception last = null;
            foreach (var h in hosts.Distinct())
            {
                try
                {
                    var r = Rcon.Run(h, S.Port, S.RconPassword, cmd);
                    _rconHost = h;
                    return r;
                }
                catch (UnauthorizedAccessException) { throw; }
                catch (Exception e) { last = e; }
            }
            throw last ?? new IOException("RCON: no address answered");
        }

        // ------------------------------------------------------------ tick rate

        /// Puts the server DLLs at the tick rate chosen in Setup (64 restores the originals).
        /// Runs before every start, so it also re-patches after a CS2 update replaced the files.
        public void ApplyTickRate()
        {
            try
            {
                TickPatch.ApplyAll(Root, TickPatch.ServerFiles, S.TickRate, m => Log(m));
            }
            catch (Exception e)
            {
                throw new InvalidOperationException(S.TickRate + " tick patch failed: " + e.Message +
                    (S.TickRate != 64 ? " Set the tick rate to 64 in Setup to start without it." : ""));
            }
        }

        /// Older versions of the pack also had Retakes, InstaDefuse and a skins plugin. When the pack is dropped over
        /// such an install they are moved to plugins\disabled (CounterStrikeSharp doesn't load from there).
        public void RemoveOldExtras()
        {
            var plugins = Path.Combine(Css, "plugins");
            var disabled = Path.Combine(plugins, "disabled");
            foreach (var name in new[] { "RetakesPlugin", "InstadefusePlugin", "CS2Loadout", "RetakesAllocator" })
            {
                var dir = Path.Combine(plugins, name);
                if (!Directory.Exists(dir)) continue;
                try
                {
                    Directory.CreateDirectory(disabled);
                    var to = Path.Combine(disabled, name);
                    if (Directory.Exists(to)) Directory.Delete(to, true);
                    Directory.Move(dir, to);
                    Log("Removed " + name + " (not part of the pack any more; moved to plugins\\disabled).");
                }
                catch (Exception e) { Log("Couldn't move " + name + " out: " + e.Message); }
            }
        }

        /// "Run without plugins": Metamod stays, CounterStrikeSharp (and so every C# plugin) is not loaded.
        /// Done by renaming its Metamod entry, so turning it back on restores it exactly.
        public void ApplyPluginsSwitch()
        {
            var on = Path.Combine(Metamod, "counterstrikesharp.vdf");
            var off = on + ".disabled";
            try
            {
                if (S.PluginsOff)
                {
                    if (File.Exists(on)) { if (File.Exists(off)) File.Delete(off); File.Move(on, off); }
                    Log("Plugins are OFF (Setup): starting without CounterStrikeSharp - no MatchZy, mode switching or chat commands.");
                }
                else if (File.Exists(off))
                {
                    if (File.Exists(on)) File.Delete(off); else File.Move(off, on);
                    Log("Plugins are back on (CounterStrikeSharp loads again).");
                }
            }
            catch (Exception e) { Log("Couldn't switch CounterStrikeSharp " + (S.PluginsOff ? "off" : "on") + ": " + e.Message); }
        }

        public string TickStatus() { return TickPatch.Describe(Root, TickPatch.ServerFiles); }

        // ------------------------------------------------------------ firewall

        /// Allows the server through Windows Firewall. Also removes inbound Block rules for this cs2.exe:
        /// Windows creates those when its "allow this app?" prompt is cancelled, and a Block rule beats every Allow rule.
        public void AddFirewallRules()
        {
            var exe = Cs2Exe.Replace("'", "''");
            var ps =
                "$exe='" + exe + "';" +
                "Get-NetFirewallApplicationFilter | Where-Object { $_.Program -ieq $exe } | Get-NetFirewallRule | " +
                "Where-Object { $_.Action -eq 'Block' -and $_.Direction -eq 'Inbound' } | Remove-NetFirewallRule;" +
                "Get-NetFirewallRule -DisplayName 'CS2 Server program','CS2 Server (UDP " + S.Port + ")' -ErrorAction SilentlyContinue | Remove-NetFirewallRule;" +
                "New-NetFirewallRule -DisplayName 'CS2 Server program' -Direction Inbound -Action Allow -Program $exe -Profile Any | Out-Null;" +
                "New-NetFirewallRule -DisplayName 'CS2 Server (UDP " + S.Port + ")' -Direction Inbound -Action Allow -Protocol UDP -LocalPort " + S.Port + " -Profile Any | Out-Null";
            var enc = Convert.ToBase64String(Encoding.Unicode.GetBytes(ps));
            var p = Process.Start(new ProcessStartInfo("powershell.exe", "-NoProfile -ExecutionPolicy Bypass -EncodedCommand " + enc)
            { Verb = "runas", UseShellExecute = true, WindowStyle = ProcessWindowStyle.Hidden });
            if (p != null) p.WaitForExit();
        }
    }
}
