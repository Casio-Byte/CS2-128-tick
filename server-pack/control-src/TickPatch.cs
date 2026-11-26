using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace CS2ServerControl
{
    /// 128 / 256 tick for the server, done by the CS2 128-Tick Patcher (cs2_128tick.exe, v2.4) that ships next to
    /// this exe - the same program players use on their game, so the server gets exactly the same patch:
    /// its reviewed tables, automatic matching for newer CS2 builds, safety checks and *.orig64 backups.
    ///
    /// This class only calls it ("patch 128|256 <folder>" / "undo <folder>"), shows its output in the log,
    /// checks the result, and remembers what it patched (*.tickpatch) so a CS2 update is noticed and re-patched.
    internal static class TickPatch
    {
        public static readonly int[] Rates = { 64, 128, 256 };

        public static readonly string[] ServerFiles =
        {
            @"game\bin\win64\engine2.dll",
            @"game\csgo\bin\win64\server.dll",
            @"game\csgo\bin\win64\client.dll",     // patched too when the install has it, like the patcher does
        };

        public const string PatcherName = "cs2_128tick.exe";

        /// cs2_128tick.exe next to CS2 Server Control.exe (or in cs2pack\).
        public static string PatcherPath
        {
            get
            {
                var dir = AppDomain.CurrentDomain.BaseDirectory;
                foreach (var p in new[] { Path.Combine(dir, PatcherName), Path.Combine(dir, "cs2pack", PatcherName) })
                    if (File.Exists(p)) return p;
                return Path.Combine(dir, PatcherName);
            }
        }

        // ------------------------------------------------------------ public API

        /// Puts the DLLs under an install root at a tick rate (64 = the originals), using cs2_128tick.exe.
        public static void ApplyAll(string root, string[] files, int rate, Action<string> log)
        {
            if (!Rates.Contains(rate)) throw new ArgumentException("Tick rate must be 64, 128 or 256.");
            var paths = files.Select(f => Path.Combine(root, f)).Where(File.Exists).ToList();
            // Never call the patcher on a folder without CS2 in it: it would go looking for the player's own CS2 instead.
            if (paths.Count == 0 || !File.Exists(Path.Combine(root, ServerFiles[0]))) { log("Tick rate: no CS2 DLLs found in " + root + "."); return; }

            // Tidy up first: forget patches a CS2 update has replaced, and undo patches made by
            // older versions of this pack (its own built-in patcher) so cs2_128tick.exe starts from the originals.
            foreach (var p in paths) Prepare(p, log);

            if (rate == 64)
            {
                if (!paths.Any(p => File.Exists(p + ".orig64"))) return;    // already the originals
                Run(new[] { "undo", root }, log);
                foreach (var p in paths)
                {
                    if (File.Exists(p + ".orig64"))
                        throw new InvalidOperationException(Path.GetFileName(p) + " is still patched (the patcher couldn't undo it). Stop the server, then try again or verify the CS2 files.");
                    State.Delete(p);
                }
                return;
            }

            if (paths.All(p => { var st = State.Read(p); return st != null && st.Rate == rate && st.Tool == State.V24 && Sha(p) == st.Patched; }))
            {
                log("Tick rate: " + string.Join(", ", paths.Select(Path.GetFileName).ToArray()) + " already " + rate + " tick.");
                return;
            }

            int code = Run(new[] { "patch", rate.ToString(), root }, log);
            var bad = new List<string>();
            foreach (var p in paths)
            {
                var b = p + ".orig64";
                if (!File.Exists(b) || Sha(b) == Sha(p)) { bad.Add(Path.GetFileName(p)); State.Delete(p); continue; }
                new State { Tool = State.V24, Rate = rate, Orig = Sha(b), Patched = Sha(p) }.Write(p);
            }
            if (bad.Count > 0 || code != 0)
                throw new InvalidOperationException("the CS2 128-Tick Patcher didn't patch " +
                    (bad.Count > 0 ? string.Join(" and ", bad.ToArray()) : "the server") +
                    " (exit code " + code + ") - see its messages above.");
        }

        /// Current tick rate of one DLL: 64, 128, 256, or 0 when it is patched but the rate isn't known.
        public static int Status(string path)
        {
            if (!File.Exists(path)) return 0;
            var st = State.Read(path);
            if (st != null && Sha(path) == st.Patched) return st.Rate;
            if (st != null) return 64;                              // CS2 replaced the patched file
            return File.Exists(path + ".orig64") ? 0 : 64;
        }

        public static string Describe(string root, string[] files)
        {
            var parts = new List<string>();
            foreach (var rel in files)
            {
                var p = Path.Combine(root, rel);
                if (!File.Exists(p)) continue;
                var r = Status(p);
                parts.Add(Path.GetFileName(p) + " " + (r == 0 ? "patched (rate unknown)" : r + " tick"));
            }
            return parts.Count == 0 ? "no CS2 DLLs found" : string.Join(", ", parts.ToArray());
        }

        /// Opens the patcher's own window (for patching a player's CS2 game).
        public static void OpenPatcherWindow()
        {
            var exe = PatcherPath;
            if (!File.Exists(exe)) throw new FileNotFoundException(PatcherName + " is missing - it should be next to CS2 Server Control.exe. Re-extract the pack.");
            Process.Start(new ProcessStartInfo(exe) { UseShellExecute = true, WorkingDirectory = Path.GetDirectoryName(exe) });
        }

        // ------------------------------------------------------------ housekeeping

        private static void Prepare(string path, Action<string> log)
        {
            var name = Path.GetFileName(path);
            var backup = path + ".orig64";
            var st = State.Read(path);
            if (st == null) return;
            var sha = Sha(path);
            if (sha != st.Patched)
            {
                // A CS2 update replaced the patched file: it is Valve's new original now, and the old backup is stale.
                if (sha != st.Orig) log(name + ": CS2 updated since the last patch - the new file will be patched.");
                State.Delete(path);
                TryDelete(backup);
                return;
            }
            if (st.Tool != State.V24)
            {
                // Patched by an older version of this pack: put the original back so the patcher starts clean.
                if (!File.Exists(backup) || Sha(backup) != st.Orig)
                    throw new InvalidOperationException(name + ": patched by an older pack version and its backup is missing or changed. Verify the CS2 files (Components → Verify), then start again.");
                try { File.Copy(backup, path, true); }
                catch (Exception e) { throw new IOException(name + " is in use - stop the server first. (" + e.Message + ")"); }
                TryDelete(backup);
                State.Delete(path);
                log(name + ": removed the old pack's tick patch - the CS2 128-Tick Patcher takes over.");
            }
        }

        // ------------------------------------------------------------ running cs2_128tick.exe

        private static readonly object ConsoleLock = new object();

        /// Runs cs2_128tick.exe with arguments and logs what it prints. Returns its exit code.
        /// It writes to the console of whoever started it (CONOUT$), so this process keeps a hidden console,
        /// lets the patcher attach to it, and reads the text back off the screen buffer.
        private static int Run(string[] args, Action<string> log)
        {
            var exe = PatcherPath;
            if (!File.Exists(exe))
                throw new FileNotFoundException(PatcherName + " is missing - it should be next to CS2 Server Control.exe. Re-extract the pack.");
            var argLine = string.Join(" ", args.Select(Quote).ToArray());
            log("CS2 128-Tick Patcher: " + string.Join(" ", args.Take(args.Length - 1).ToArray()) + " ...");

            lock (ConsoleLock)
            {
                var con = HiddenConsole.Open();
                if (con != null) con.Clear();
                var psi = new ProcessStartInfo(exe, argLine)
                {
                    UseShellExecute = false,
                    WorkingDirectory = Path.GetDirectoryName(exe),
                };
                int code;
                using (var p = Process.Start(psi))
                {
                    if (!p.WaitForExit(180000))
                    {
                        try { p.Kill(); } catch { }
                        throw new TimeoutException(PatcherName + " didn't finish within 3 minutes.");
                    }
                    code = p.ExitCode;
                }
                if (con != null)
                    foreach (var line in con.ReadLines())
                        if (line.Trim().Length > 0) log("  " + line.TrimEnd());
                return code;
            }
        }

        private static string Quote(string a)
        {
            if (a.Length > 0 && a.IndexOfAny(new[] { ' ', '\t', '"' }) < 0) return a;
            return "\"" + a.TrimEnd('\\').Replace("\"", "\\\"") + "\"";
        }

        /// A console window that is never shown, so the patcher's output can be read.
        private sealed class HiddenConsole
        {
            private const short Width = 160, Height = 2000;
            private static HiddenConsole _one;
            private readonly IntPtr _out;

            private HiddenConsole(IntPtr h) { _out = h; }

            public static HiddenConsole Open()
            {
                if (_one != null) return _one;
                try
                {
                    if (GetConsoleWindow() == IntPtr.Zero)
                    {
                        if (!AllocConsole()) return null;
                        var w = GetConsoleWindow();
                        if (w != IntPtr.Zero) ShowWindow(w, 0);
                    }
                    var h = CreateFile("CONOUT$", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
                    if (h == IntPtr.Zero || h == new IntPtr(-1)) return null;
                    SetConsoleScreenBufferSize(h, new Coord { X = Width, Y = Height });
                    _one = new HiddenConsole(h);
                    return _one;
                }
                catch { return null; }
            }

            public void Clear()
            {
                int n;
                FillConsoleOutputCharacter(_out, ' ', Width * Height, new Coord(), out n);
                SetConsoleCursorPosition(_out, new Coord());
            }

            public IEnumerable<string> ReadLines()
            {
                var info = new BufferInfo();
                if (!GetConsoleScreenBufferInfo(_out, out info)) yield break;
                int w = info.Size.X, rows = Math.Min(info.Cursor.Y + 1, info.Size.Y);
                for (short y = 0; y < rows; y++)
                {
                    var sb = new StringBuilder(w + 1);
                    int read;
                    if (!ReadConsoleOutputCharacter(_out, sb, (uint)w, new Coord { X = 0, Y = y }, out read)) yield break;
                    yield return sb.ToString(0, Math.Min(read, sb.Length));
                }
            }

            [StructLayout(LayoutKind.Sequential)] private struct Coord { public short X, Y; }
            [StructLayout(LayoutKind.Sequential)] private struct SmallRect { public short L, T, R, B; }
            [StructLayout(LayoutKind.Sequential)]
            private struct BufferInfo { public Coord Size, Cursor; public short Attr; public SmallRect Window; public Coord Max; }

            [DllImport("kernel32.dll")] private static extern bool AllocConsole();
            [DllImport("kernel32.dll")] private static extern IntPtr GetConsoleWindow();
            [DllImport("user32.dll")] private static extern bool ShowWindow(IntPtr h, int cmd);
            [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
            private static extern IntPtr CreateFile(string name, uint access, uint share, IntPtr sec, uint disp, uint flags, IntPtr tmpl);
            [DllImport("kernel32.dll")] private static extern bool SetConsoleScreenBufferSize(IntPtr h, Coord size);
            [DllImport("kernel32.dll")] private static extern bool SetConsoleCursorPosition(IntPtr h, Coord pos);
            [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
            private static extern bool FillConsoleOutputCharacter(IntPtr h, char c, int len, Coord at, out int written);
            [DllImport("kernel32.dll")] private static extern bool GetConsoleScreenBufferInfo(IntPtr h, out BufferInfo info);
            [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
            private static extern bool ReadConsoleOutputCharacter(IntPtr h, StringBuilder sb, uint len, Coord at, out int read);
        }

        // ------------------------------------------------------------ state

        public static string Sha(string path) { using (var f = File.OpenRead(path)) return Sha(f); }
        private static string Sha(Stream s)
        {
            using (var h = SHA256.Create())
                return BitConverter.ToString(h.ComputeHash(s)).Replace("-", "").ToLowerInvariant();
        }

        private static void TryDelete(string p) { try { if (File.Exists(p)) File.Delete(p); } catch { } }

        /// <dll>.tickpatch: which tool patched it, the rate, and SHA-256 of the original and the patched file.
        /// Old pack versions wrote "rate|orig|patched"; now it is "v24|rate|orig|patched".
        private sealed class State
        {
            public const string V24 = "v24";
            public string Tool = "";
            public int Rate;
            public string Orig = "", Patched = "";

            public static State Read(string dll)
            {
                var f = dll + ".tickpatch";
                if (!File.Exists(f)) return null;
                try
                {
                    var p = File.ReadAllText(f).Trim().Split('|');
                    if (p.Length == 4) return new State { Tool = p[0], Rate = int.Parse(p[1]), Orig = p[2], Patched = p[3] };
                    return new State { Tool = "pack", Rate = int.Parse(p[0]), Orig = p[1], Patched = p[2] };
                }
                catch { return null; }
            }

            public void Write(string dll) { File.WriteAllText(dll + ".tickpatch", Tool + "|" + Rate + "|" + Orig + "|" + Patched); }
            public static void Delete(string dll) { TryDelete(dll + ".tickpatch"); }
        }
    }
}
