using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace CS2ServerControl
{
    /// Installed versions of the downloaded parts, kept in cs2pack/versions.json.
    internal static class Versions
    {
        private static string File_(ServerCore c) { return Path.Combine(c.PackDir, "versions.json"); }

        public static string Get(ServerCore c, string key)
        {
            try { return Json.Str(Json.LoadObject(File_(c)), key); } catch { return ""; }
        }

        public static void Set(ServerCore c, string key, string value)
        {
            try
            {
                var d = File.Exists(File_(c)) ? Json.LoadObject(File_(c)) : new Dictionary<string, object>();
                d[key] = value;
                Json.Save(File_(c), d);
            }
            catch { }
        }
    }

    /// Checks CS2, Metamod, CounterStrikeSharp and the GitHub plugins for newer versions.
    internal static class Updates
    {
        public sealed class Item
        {
            public string Name, Installed, Latest, Note;
            public bool Available;

            public override string ToString()
            {
                var s = Name + ": " + (Available ? "UPDATE  " + Show(Installed) + " -> " + Latest : "up to date (" + Show(Latest.Length > 0 ? Latest : Installed) + ")");
                return Note == null ? s : s + "  - " + Note;
            }

            private static string Show(string v) { return string.IsNullOrEmpty(v) ? "?" : v; }
        }

        public static List<Item> Check(ServerCore c)
        {
            var list = new List<Item>();
            DateTime cs2Date = DateTime.MinValue;

            // CS2: Steam says whether this build is the current one.
            if (c.Cs2Installed)
            {
                var it = new Item { Name = "CS2 server", Installed = c.Cs2Version(), Latest = "" };
                try
                {
                    var inf = ReadInf(Path.Combine(c.Csgo, "steam.inf"));
                    string cv; inf.TryGetValue("ClientVersion", out cv);
                    string vd; inf.TryGetValue("VersionDate", out vd);
                    DateTime.TryParseExact((vd ?? "").Replace("  ", " "), new[] { "MMM d yyyy", "MMM dd yyyy" }, CultureInfo.InvariantCulture, DateTimeStyles.None, out cs2Date);
                    var r = Json.ParseObject(Http.Get("https://api.steampowered.com/ISteamApps/UpToDateCheck/v1/?appid=730&version=" + cv));
                    var resp = r.ContainsKey("response") ? r["response"] as Dictionary<string, object> : null;
                    bool upToDate = resp != null && Json.Bool(resp, "up_to_date", true);
                    var req = resp != null ? Json.Str(resp, "required_version") : "";
                    it.Available = !upToDate;
                    it.Latest = upToDate ? it.Installed : "build " + req;
                    if (it.Available) it.Note = "Components > Install / update CS2";
                }
                catch (Exception e) { it.Note = "check failed: " + e.Message; }
                list.Add(it);
            }

            // Metamod: the drop folder names the newest build.
            {
                var it = new Item { Name = "Metamod 2.0", Installed = Versions.Get(c, "metamod"), Latest = "" };
                try
                {
                    var name = Http.Get("https://mms.alliedmods.net/mmsdrop/2.0/mmsource-latest-windows").Trim();
                    var m = Regex.Match(name, @"mmsource-([\d.]+-git\d+)");
                    it.Latest = m.Success ? m.Groups[1].Value : name;
                    it.Available = Newer(it.Latest, it.Installed);
                    if (it.Available) it.Note = "Components > Metamod";
                }
                catch (Exception e) { it.Note = "check failed: " + e.Message; }
                list.Add(it);
            }

            // CounterStrikeSharp: newest release, and whether it is older than the CS2 build.
            {
                var it = new Item { Name = "CounterStrikeSharp", Installed = Versions.Get(c, "counterstrikesharp"), Latest = "" };
                try
                {
                    var rel = Json.ParseObject(Http.Get("https://api.github.com/repos/roflmuffin/CounterStrikeSharp/releases/latest"));
                    it.Latest = Json.Str(rel, "tag_name");
                    it.Available = Newer(it.Latest, it.Installed);
                    if (it.Available) it.Note = "Components > CounterStrikeSharp";
                    DateTime pub;
                    if (cs2Date > DateTime.MinValue && DateTime.TryParse(Json.Str(rel, "published_at"), CultureInfo.InvariantCulture, DateTimeStyles.AdjustToUniversal, out pub) && pub.Date < cs2Date.Date)
                        it.Note = (it.Note == null ? "" : it.Note + ". ") +
                                  "CS2 updated on " + cs2Date.ToString("d MMM", CultureInfo.InvariantCulture) + " but the newest CounterStrikeSharp is from " + pub.ToString("d MMM", CultureInfo.InvariantCulture) +
                                  ": if the server crashes on start, wait for a CounterStrikeSharp update (checked again automatically)";
                }
                catch (Exception e) { it.Note = "check failed: " + e.Message; }
                list.Add(it);
            }

            foreach (var p in Components.Plugins)
            {
                var it = new Item { Name = p.Name, Installed = Versions.Get(c, p.Name), Latest = "" };
                try
                {
                    var rel = Json.ParseObject(Http.Get("https://api.github.com/repos/" + p.Repo + "/releases/latest"));
                    it.Latest = Json.Str(rel, "tag_name");
                    it.Available = Newer(it.Latest, it.Installed);
                    if (it.Available) it.Note = "Components > " + p.Name;
                }
                catch (Exception e) { it.Note = "check failed: " + e.Message; }
                list.Add(it);
            }
            return list;
        }

        /// True when `latest` has a higher version number than `installed` (or nothing is recorded as installed).
        public static bool Newer(string latest, string installed)
        {
            if (string.IsNullOrEmpty(latest)) return false;
            if (string.IsNullOrEmpty(installed)) return true;
            var a = Nums(latest); var b = Nums(installed);
            for (int i = 0; i < Math.Max(a.Count, b.Count); i++)
            {
                long x = i < a.Count ? a[i] : 0, y = i < b.Count ? b[i] : 0;
                if (x != y) return x > y;
            }
            return false;
        }

        private static List<long> Nums(string v)
        {
            return Regex.Matches(v ?? "", @"\d+").Cast<Match>().Select(m => { long n; long.TryParse(m.Value, out n); return n; }).ToList();
        }

        private static Dictionary<string, string> ReadInf(string path)
        {
            var d = new Dictionary<string, string>();
            if (!File.Exists(path)) return d;
            foreach (var l in File.ReadAllLines(path))
            {
                int i = l.IndexOf('=');
                if (i > 0) d[l.Substring(0, i).Trim()] = l.Substring(i + 1).Trim();
            }
            return d;
        }
    }
}
