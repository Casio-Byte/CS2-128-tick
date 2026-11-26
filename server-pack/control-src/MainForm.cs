using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Net;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;

namespace CS2ServerControl
{
    internal static class Theme
    {
        public static readonly Color Bg = Color.FromArgb(22, 22, 24);
        public static readonly Color Panel = Color.FromArgb(32, 32, 36);
        public static readonly Color Field = Color.FromArgb(44, 44, 50);
        public static readonly Color Text = Color.FromArgb(230, 230, 230);
        public static readonly Color Dim = Color.FromArgb(150, 150, 158);
        public static readonly Color Accent = Color.FromArgb(255, 85, 0);
        public static readonly Color Good = Color.FromArgb(70, 200, 110);
        public static readonly Color Bad = Color.FromArgb(230, 70, 70);
        public static readonly Font Ui = new Font("Segoe UI", 9.5f);
        public static readonly Font Bold = new Font("Segoe UI Semibold", 10f);
        public static readonly Font Big = new Font("Segoe UI Semibold", 15f);
        public static readonly Font Mono = new Font("Consolas", 9f);

        public static Button Btn(string text, EventHandler click, bool accent = false)
        {
            var b = new Button
            {
                Text = text, AutoSize = true, FlatStyle = FlatStyle.Flat, ForeColor = Color.White,
                BackColor = accent ? Accent : Field, Font = Ui, Padding = new Padding(8, 3, 8, 3), Margin = new Padding(3),
                Cursor = Cursors.Hand,
            };
            b.FlatAppearance.BorderSize = 0;
            b.Click += click;
            return b;
        }

        public static void Style(Control root)
        {
            foreach (Control c in root.Controls)
            {
                if (c is TextBox || c is ComboBox || c is NumericUpDown || c is ListBox)
                {
                    c.BackColor = Field; c.ForeColor = Text;
                    if (c is TextBox) ((TextBox)c).BorderStyle = BorderStyle.FixedSingle;
                }
                else if (c is Label || c is CheckBox || c is RadioButton) c.ForeColor = c.ForeColor == SystemColors.ControlText ? Text : c.ForeColor;
                if (c.Font == Control.DefaultFont) c.Font = Ui;
                Style(c);
            }
        }
    }

    internal sealed class MainForm : Form
    {
        private readonly ServerCore _core = new ServerCore();
        private Components _comp;
        private readonly TabControl _tabs = new TabControl();
        private TextBox _log;
        private Label _status, _info;
        private Button _start, _stop, _restart;
        private ComboBox _modeQuick;
        private volatile bool _busy;

        public MainForm()
        {
            Text = "CS2 Server Control";
            Size = new Size(980, 720);
            MinimumSize = new Size(860, 600);
            BackColor = Theme.Bg;
            ForeColor = Theme.Text;
            Font = Theme.Ui;
            StartPosition = FormStartPosition.CenterScreen;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

            _comp = new Components(_core, Log);
            _core.Log += Log;
            _core.StateChanged += () => BeginInvokeSafe(RefreshInfo);

            _tabs.Dock = DockStyle.Fill;
            _tabs.Appearance = TabAppearance.Normal;
            _tabs.Font = Theme.Bold;
            _tabs.TabPages.Add(BuildInfo());
            _tabs.TabPages.Add(BuildSetup());
            _tabs.TabPages.Add(BuildComponents());
            _tabs.TabPages.Add(BuildMaps());
            _tabs.TabPages.Add(BuildPlayers());
            _tabs.TabPages.Add(BuildConsole());
            foreach (TabPage p in _tabs.TabPages) { p.BackColor = Theme.Bg; p.ForeColor = Theme.Text; p.Padding = new Padding(10); }
            Controls.Add(_tabs);
            Theme.Style(this);

            Load += (s, e) =>
            {
                RefreshInfo();
                if (_core.S.CheckUpdates) CheckForUpdates(true);
                _updTimer = new System.Windows.Forms.Timer { Interval = 6 * 3600 * 1000 };
                _updTimer.Tick += (s2, e2) => { if (_core.S.CheckUpdates) CheckForUpdates(true); };
                _updTimer.Start();
                if (!_core.Cs2Installed)
                {
                    Log("No CS2 server found in " + _core.Root + ". Go to the Components tab and press 'Install / update CS2' (SteamCMD, about 60 GB).");
                    _tabs.SelectedIndex = 2;
                }
                else if (!_core.MetamodInstalled || !_core.CssInstalled)
                {
                    Log("Metamod or CounterStrikeSharp is missing. Components tab > 'Install / update everything'.");
                    _tabs.SelectedIndex = 2;
                }
                else if (string.IsNullOrEmpty(_core.S.AdminPassword))
                {
                    Log("Fill in the Setup tab (server name, passwords) and press Save.");
                    _tabs.SelectedIndex = 1;
                }
            };
            FormClosing += OnClosingForm;
        }

        private void OnClosingForm(object sender, FormClosingEventArgs e)
        {
            if (!_core.Running) return;
            var r = MessageBox.Show(this, "The server is still running. Stop it?\n\nYes = stop the server and close\nNo = close and leave the server running (no crash restart)",
                "CS2 Server Control", MessageBoxButtons.YesNoCancel, MessageBoxIcon.Question);
            if (r == DialogResult.Cancel) { e.Cancel = true; return; }
            if (r == DialogResult.Yes) _core.Stop();
        }

        // ============================================================ helpers

        private void Log(string msg)
        {
            BeginInvokeSafe(() =>
            {
                if (_log == null) return;
                _log.AppendText("[" + DateTime.Now.ToString("HH:mm:ss") + "] " + msg + Environment.NewLine);
            });
        }

        private void BeginInvokeSafe(Action a)
        {
            if (IsDisposed) return;
            if (!IsHandleCreated) { try { a(); } catch { } return; }
            try { BeginInvoke(a); } catch { }
        }

        private void Background(string what, Action work, Action after = null)
        {
            if (_busy) { Log("Busy, wait for the current job to finish."); return; }
            _busy = true;
            UseWaitCursor = true;
            ThreadPool.QueueUserWorkItem(_ =>
            {
                try { work(); }
                catch (Exception e) { Log(what + " failed: " + e.Message); }
                finally
                {
                    _busy = false;
                    BeginInvokeSafe(() =>
                    {
                        UseWaitCursor = false;
                        RefreshInfo();
                        if (after != null) after();
                    });
                }
            });
        }

        private static Label L(string text, bool dim = false)
        {
            return new Label { Text = text, AutoSize = true, ForeColor = dim ? Theme.Dim : Theme.Text, Margin = new Padding(3, 7, 3, 3) };
        }

        /// A toolbar with a fixed height that wraps its buttons inside it. (AutoSize + wrap + Dock.Top
        /// measures one line on Windows, so the second line ended up under the next docked control.)
        private static Panel Bar(int height, params Control[] cs)
        {
            var p = new Panel { Dock = DockStyle.Top, Height = height };
            var f = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = true, FlowDirection = FlowDirection.LeftToRight };
            f.Controls.AddRange(cs);
            p.Controls.Add(f);
            return p;
        }

        private static Label Note(string text, int height)
        {
            return new Label { Text = text, Dock = DockStyle.Top, Height = height, AutoSize = false, ForeColor = Theme.Dim, Padding = new Padding(3, 4, 3, 0) };
        }

        private static FlowLayoutPanel Row(params Control[] cs)
        {
            var f = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Top, WrapContents = true, FlowDirection = FlowDirection.LeftToRight };
            f.Controls.AddRange(cs);
            return f;
        }

        // ============================================================ Info

        private TabPage BuildInfo()
        {
            var page = new TabPage("Info");
            _status = new Label { Text = "Stopped", Font = Theme.Big, AutoSize = true, ForeColor = Theme.Bad, Margin = new Padding(3, 3, 20, 3) };
            _start = Theme.Btn("Start", (s, e) => DoStart(), true);
            _stop = Theme.Btn("Stop", (s, e) => Background("Stop", () => _core.Stop()));
            _restart = Theme.Btn("Restart", (s, e) => Background("Restart", () => { _core.Stop(); Thread.Sleep(1500); _core.Start(); }));
            var folder = Theme.Btn("Open server folder", (s, e) => Process.Start("explorer.exe", "\"" + _core.Csgo + "\""));

            _modeQuick = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 170 };
            _modeQuick.Items.AddRange(Modes.Titles);
            var applyMode = Theme.Btn("Set mode", (s, e) => SetModeQuick());

            _info = new Label { AutoSize = true, Font = Theme.Mono, ForeColor = Theme.Text, Margin = new Padding(3, 8, 3, 8) };

            _log = new TextBox
            {
                Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Dock = DockStyle.Fill,
                Font = Theme.Mono, BackColor = Theme.Panel, ForeColor = Theme.Text, BorderStyle = BorderStyle.None,
            };

            var top = new Panel { Dock = DockStyle.Top, Height = 250 };
            var col = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false };
            col.Controls.Add(Row(_status, _start, _stop, _restart, folder));
            col.Controls.Add(Row(L("Game mode:"), _modeQuick, applyMode, L("(switches live when running, otherwise used on the next Start)", true)));
            col.Controls.Add(_info);
            top.Controls.Add(col);

            page.Controls.Add(_log);
            page.Controls.Add(new Label { Text = "Log", Dock = DockStyle.Top, ForeColor = Theme.Dim, Height = 20 });
            page.Controls.Add(top);
            return page;
        }

        private void DoStart()
        {
            if (_core.Running) return;
            if (!_core.Cs2Installed) { Log("CS2 server isn't installed yet (Components tab)."); _tabs.SelectedIndex = 2; return; }
            if (string.IsNullOrEmpty(_core.S.AdminPassword)) { Log("Set an admin password in Setup first."); _tabs.SelectedIndex = 1; return; }
            Background("Start", () =>
            {
                if (_core.S.UpdateCs2OnStart) _comp.InstallOrUpdateCs2(false);
                if (_core.S.UpdatePluginsOnStart)
                {
                    try { _comp.InstallMetamod(); _comp.InstallCss(); foreach (var p in Components.Plugins) _comp.InstallPlugin(p); }
                    catch (Exception e) { Log("Plugin update failed, starting anyway: " + e.Message); }
                }
                _core.Start();
            });
        }

        private void SetModeQuick()
        {
            if (_modeQuick.SelectedIndex < 0) return;
            var key = Modes.Keys[_modeQuick.SelectedIndex];
            if (_core.Running)
            {
                Background("Mode switch", () =>
                {
                    var r = _core.RconExec("cs2pack_setmode " + key);
                    Log("Mode: " + (string.IsNullOrWhiteSpace(r) ? "switching to " + Modes.Titles[_modeQuick.SelectedIndex] : r.Trim()));
                });
            }
            else
            {
                _core.SetCurrentMode(key);
                var maps = _core.LoadMaps().Where(m => MapFits(m, key)).Select(m => Json.Str(m, "name")).ToList();
                if (maps.Count > 0 && !maps.Contains(_core.S.StartMap, StringComparer.OrdinalIgnoreCase))
                {
                    _core.S.StartMap = maps[0];
                    _core.SaveSettings();
                    Log("Start map changed to " + maps[0] + " (fits " + Modes.Titles[_modeQuick.SelectedIndex] + ").");
                }
                Log("Next Start: " + Modes.Titles[_modeQuick.SelectedIndex] + ".");
                RefreshInfo();
            }
        }

        private static bool MapFits(Dictionary<string, object> m, string mode)
        {
            object v;
            if (m.TryGetValue("modes", out v) && v is object[] && ((object[])v).Length > 0)
                return ((object[])v).Any(x => Convert.ToString(x) == mode);
            return Modes.ForMapName(Json.Str(m, "name")).Contains(mode);
        }

        private string _publicIp;

        private void RefreshInfo()
        {
            if (_status == null) return;
            bool run = _core.Running;
            _status.Text = run ? "Running" : "Stopped";
            _status.ForeColor = run ? Theme.Good : Theme.Bad;
            _start.Enabled = !run; _stop.Enabled = run; _restart.Enabled = run;
            var mode = _core.CurrentMode();
            var idx = Array.IndexOf(Modes.Keys, mode);
            if (idx >= 0 && !_modeQuick.DroppedDown) _modeQuick.SelectedIndex = idx;

            var lan = _core.S.Network != "internet";
            var ip = lan ? LocalIp() : (_publicIp ?? "(public IP: Setup tab)");
            var sb = new StringBuilder();
            sb.AppendLine("Server folder : " + _core.Root);
            sb.AppendLine("CS2 build     : " + (_core.Cs2Installed ? _core.Cs2Version() : "not installed"));
            sb.AppendLine("Plugins       : Metamod " + (_core.MetamodInstalled ? "yes" : "NO") + ", CounterStrikeSharp " + (_core.CssInstalled ? (_core.S.PluginsOff ? "OFF (Setup)" : "yes") : "NO"));
            sb.AppendLine("Mode / map    : " + Modes.Titles[Math.Max(0, idx)] + " / " + _core.S.StartMap + " (start map)");
            sb.AppendLine("Network       : " + (lan ? "LAN / virtual LAN" : "Internet") + (_core.S.Insecure || _core.S.TickRate != 64 ? ", -insecure" : "") + ", " + _core.S.TickRate + " tick");
            sb.AppendLine("Connect       : connect " + ip + ":" + _core.S.Port + (string.IsNullOrEmpty(_core.S.ServerPassword) ? "" : "; password " + _core.S.ServerPassword));
            sb.Append("Admin         : type /login <admin password> in chat");
            _info.Text = sb.ToString();
        }

        private static string LocalIp()
        {
            try
            {
                foreach (var a in Dns.GetHostEntry(Dns.GetHostName()).AddressList)
                    if (a.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork && !IPAddress.IsLoopback(a)) return a.ToString();
            }
            catch { }
            return "127.0.0.1";
        }

        // ============================================================ Setup

        private TextBox _host, _svpw, _rconpw, _adminpw, _gslt, _startMap;
        private NumericUpDown _maxpl, _port, _bots;
        private RadioButton _lan, _inet;
        private ComboBox _defMode, _tickRate;
        private CheckBox _insecure, _pluginsOff, _autoRestart, _updCs2, _updPlugins, _checkUpd, _showWin;
        private Label _warn;

        private TabPage BuildSetup()
        {
            var page = new TabPage("Setup");
            var t = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3, AutoScroll = true };
            t.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 190));
            t.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 330));
            t.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));

            Func<int, TextBox> tb = w => new TextBox { Width = w };
            _host = tb(320); _svpw = tb(200); _rconpw = tb(200); _adminpw = tb(200); _gslt = tb(320);
            _startMap = tb(200);
            _maxpl = new NumericUpDown { Minimum = 2, Maximum = 64, Width = 70 };
            _port = new NumericUpDown { Minimum = 1024, Maximum = 65535, Width = 90 };
            _bots = new NumericUpDown { Minimum = 0, Maximum = 20, Width = 70 };
            _lan = new RadioButton { Text = "LAN / virtual LAN (Tailscale, ZeroTier)", AutoSize = true };
            _inet = new RadioButton { Text = "Internet (needs a login token)", AutoSize = true };
            _defMode = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 200 };
            _defMode.Items.AddRange(Modes.Titles);
            _insecure = new CheckBox { Text = "-insecure (no VAC; always on at 128 / 256 tick)", AutoSize = true };
            _tickRate = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 160 };
            _tickRate.Items.AddRange(new object[] { "64 tick (stock)", "128 tick", "256 tick (experimental)" });
            _pluginsOff = new CheckBox { Text = "Run without plugins (CounterStrikeSharp off) - plain server, for when a CS2 update breaks CounterStrikeSharp", AutoSize = true };
            _autoRestart = new CheckBox { Text = "Restart the server if it crashes", AutoSize = true };
            _updCs2 = new CheckBox { Text = "Update CS2 (SteamCMD) on every Start", AutoSize = true };
            _updPlugins = new CheckBox { Text = "Update Metamod / CounterStrikeSharp / plugins on every Start", AutoSize = true };
            _checkUpd = new CheckBox { Text = "Check for updates (CS2, Metamod, CounterStrikeSharp, plugins) when this opens and every 6 hours", AutoSize = true };
            _showWin = new CheckBox { Text = "Show the cs2.exe console window", AutoSize = true };
            _warn = new Label { AutoSize = true, ForeColor = Theme.Accent, MaximumSize = new Size(700, 0), Margin = new Padding(3, 8, 3, 3) };

            var gameTick = Theme.Btn("Patch my CS2 game...", (s, e) =>
            {
                try { TickPatch.OpenPatcherWindow(); }
                catch (Exception ex) { MessageBox.Show(this, ex.Message, "Patch my CS2 game", MessageBoxButtons.OK, MessageBoxIcon.Error); }
            });
            var genRcon = Theme.Btn("New", (s, e) => _rconpw.Text = Rand.Password(16));
            var tokenLink = new LinkLabel { Text = "get a token (App ID 730)", AutoSize = true, LinkColor = Theme.Accent, Margin = new Padding(3, 7, 3, 3) };
            tokenLink.LinkClicked += (s, e) => Process.Start("https://steamcommunity.com/dev/managegameservers");
            var ipBtn = Theme.Btn("Check public IP", (s, e) => Background("Public IP", () =>
            {
                _publicIp = Http.Get("https://api.ipify.org").Trim();
                Log("Public IP: " + _publicIp + " (forward UDP " + _core.S.Port + " on your router to this PC).");
            }));
            var fw = Theme.Btn("Fix firewall", (s, e) => Background("Firewall", () => { _core.AddFirewallRules(); Log("Firewall: cs2.exe and UDP " + _core.S.Port + " allowed; any rules blocking the server were removed."); }));

            int r = 0;
            Action<string, Control, Control> add = (label, c, extra) =>
            {
                t.Controls.Add(L(label), 0, r);
                t.Controls.Add(c, 1, r);
                if (extra != null) t.Controls.Add(extra, 2, r);
                else t.SetColumnSpan(c, 2);
                r++;
            };
            add("Server name", _host, null);
            add("Join password", _svpw, L("empty = no password", true));
            add("RCON password", Row(_rconpw, genRcon), L("used by this program; random if empty", true));
            add("Admin password", _adminpw, L("players type /login <password> in chat", true));
            add("Max players", _maxpl, null);
            add("Bots", _bots, L("fill empty slots with bots", true));
            add("Port", _port, L("UDP; also RCON (TCP) - never forward TCP", true));
            add("Default game mode", _defMode, null);
            add("Start map", _startMap, L("e.g. de_dust2, or a map name from the Maps tab", true));
            add("Network", Row(_lan, _inet), null);
            add("Login token (GSLT)", _gslt, tokenLink);
            add("", _insecure, null);
            add("", Row(ipBtn, fw), null);
            add("Tick rate", Row(_tickRate, gameTick), L("server DLLs are patched before each start; players patch their game to the same rate", true));
            add("", _pluginsOff, null);
            add("", _autoRestart, null);
            add("", _updCs2, null);
            add("", _updPlugins, null);
            add("", _checkUpd, null);
            add("", _showWin, null);
            t.Controls.Add(_warn, 0, r); t.SetColumnSpan(_warn, 3); r++;
            var save = Theme.Btn("Save", (s, e) => SaveSetup(), true);
            t.Controls.Add(save, 1, r);

            EventHandler warn = (s, e) => UpdateWarnings();
            _inet.CheckedChanged += warn; _pluginsOff.CheckedChanged += warn; _tickRate.SelectedIndexChanged += warn; _insecure.CheckedChanged += warn;

            LoadSetup();
            page.Controls.Add(t);
            return page;
        }

        private void LoadSetup()
        {
            var s = _core.S;
            _host.Text = s.Hostname; _svpw.Text = s.ServerPassword; _rconpw.Text = s.RconPassword; _adminpw.Text = s.AdminPassword;
            _gslt.Text = s.Gslt; _startMap.Text = s.StartMap;
            _maxpl.Value = Math.Max(2, Math.Min(64, s.MaxPlayers)); _port.Value = Math.Max(1024, Math.Min(65535, s.Port)); _bots.Value = Math.Max(0, Math.Min(20, s.Bots));
            _lan.Checked = s.Network != "internet"; _inet.Checked = s.Network == "internet";
            _defMode.SelectedIndex = Math.Max(0, Array.IndexOf(Modes.Keys, s.DefaultMode));
            _insecure.Checked = s.Insecure; _tickRate.SelectedIndex = Math.Max(0, Array.IndexOf(TickPatch.Rates, s.TickRate)); _pluginsOff.Checked = s.PluginsOff; _autoRestart.Checked = s.AutoRestart;
            _updCs2.Checked = s.UpdateCs2OnStart; _updPlugins.Checked = s.UpdatePluginsOnStart; _checkUpd.Checked = s.CheckUpdates; _showWin.Checked = s.ShowServerWindow;
            UpdateWarnings();
        }

        private void UpdateWarnings()
        {
            var w = new List<string>();
            bool insecure = _insecure.Checked || _tickRate.SelectedIndex > 0;
            if (_tickRate.SelectedIndex > 0)
                w.Add("128 / 256 tick patches the CS2 DLLs, so the server always starts with -insecure. Players patch their game to the same rate (\"Patch my CS2 game...\") and must not join VAC servers until they undo it.");
            if (_pluginsOff.Checked)
                w.Add("Plugins off: the server starts without CounterStrikeSharp, so there is no MatchZy, game-mode switching or chat commands (!tickrate etc.). Untick it once Components > Check for updates finds a CounterStrikeSharp update.");
            if (_tickRate.SelectedIndex == 2)
                w.Add("256 tick: about 4x the CPU and network of 64 tick, every tick has 3.9 ms, and any tick maths the patch missed is 4x off. Best on LAN with a fast CPU.");
            if (_inet.Checked && string.IsNullOrWhiteSpace(_gslt.Text) && !insecure)
                w.Add("Internet mode needs a login token (GSLT) unless the server runs -insecure.");
            _warn.Text = string.Join(Environment.NewLine, w.ToArray());
        }

        private void SaveSetup()
        {
            var s = _core.S;
            var oldMode = s.DefaultMode;
            var oldRate = s.TickRate;
            var oldPlugins = s.PluginsOff;
            s.Hostname = _host.Text.Trim(); s.ServerPassword = _svpw.Text.Trim(); s.RconPassword = _rconpw.Text.Trim();
            s.AdminPassword = _adminpw.Text.Trim(); s.Gslt = _gslt.Text.Trim();
            s.StartMap = _startMap.Text.Trim();
            s.MaxPlayers = (int)_maxpl.Value; s.Port = (int)_port.Value; s.Bots = (int)_bots.Value;
            s.Network = _inet.Checked ? "internet" : "lan";
            s.DefaultMode = Modes.Keys[Math.Max(0, _defMode.SelectedIndex)];
            s.Insecure = _insecure.Checked; s.TickRate = TickPatch.Rates[Math.Max(0, _tickRate.SelectedIndex)]; s.PluginsOff = _pluginsOff.Checked; s.AutoRestart = _autoRestart.Checked;
            s.UpdateCs2OnStart = _updCs2.Checked; s.UpdatePluginsOnStart = _updPlugins.Checked; s.CheckUpdates = _checkUpd.Checked; s.ShowServerWindow = _showWin.Checked;
            if (string.IsNullOrEmpty(s.AdminPassword)) { MessageBox.Show(this, "Set an admin password: it's how you get admin in game (/login).", "Setup"); return; }
            try
            {
                _core.SaveSettings();
                if (oldMode != s.DefaultMode) _core.SetCurrentMode(s.DefaultMode);
                _rconpw.Text = s.RconPassword;
                Log("Setup saved." + (_core.Running ? " Restart the server for port / player / network changes." : ""));
                if (oldPlugins != s.PluginsOff)
                    Log("Plugins " + (s.PluginsOff ? "off" : "on") + ": applies at the next " + (_core.Running ? "restart" : "Start") + ".");
                if (oldRate != s.TickRate)
                    Log("Tick rate " + s.TickRate + ": the server DLLs are " + (s.TickRate == 64 ? "restored" : "patched") + " at the next " + (_core.Running ? "restart" : "Start") + ".");
                if (_core.Running) try { _core.RconExec("exec cs2pack/server_settings.cfg; cs2pack_reload"); } catch { }
            }
            catch (Exception e) { Log("Saving failed: " + e.Message); }
            RefreshInfo();
        }

        // ============================================================ Components

        private ListBox _compList;

        private TabPage BuildComponents()
        {
            var page = new TabPage("Components");
            _compList = new ListBox { Dock = DockStyle.Fill, Font = Theme.Mono, BackColor = Theme.Panel, ForeColor = Theme.Text, BorderStyle = BorderStyle.None, IntegralHeight = false };
            Func<Action, Action> guard = a => () =>
            {
                if (_core.Running) { Log("Stop the server first: its files are in use."); return; }
                a();
            };
            var all = Theme.Btn("Install / update everything", (s, e) => guard(() => Background("Install", () => _comp.InstallAll(false), RefreshComponents))(), true);
            var cs2 = Theme.Btn("Install / update CS2", (s, e) => guard(() => Background("CS2 update", () => _comp.InstallOrUpdateCs2(false), RefreshComponents))());
            var val = Theme.Btn("Verify CS2 files", (s, e) => guard(() => Background("CS2 verify", () => _comp.InstallOrUpdateCs2(true), RefreshComponents))());
            var mm = Theme.Btn("Metamod", (s, e) => guard(() => Background("Metamod", () => _comp.InstallMetamod(), RefreshComponents))());
            var css = Theme.Btn("CounterStrikeSharp", (s, e) => guard(() => Background("CounterStrikeSharp", () => _comp.InstallCss(), RefreshComponents))());
            var cssrt = Theme.Btn("CSS + .NET runtime", (s, e) => guard(() => Background("CounterStrikeSharp runtime", () => _comp.ReinstallCssRuntime(), RefreshComponents))());
            var buttons = new List<Control> { cs2, all, val, mm, css, cssrt };
            foreach (var p in Components.Plugins)
            {
                var plugin = p;
                buttons.Add(Theme.Btn(p.Name, (s, e) => guard(() => Background(plugin.Name, () => _comp.InstallPlugin(plugin), RefreshComponents))()));
            }
            buttons.Add(Theme.Btn("Stop SteamCMD", (s, e) =>
            {
                if (!Components.SteamCmdRunning()) { Log("SteamCMD isn't running."); return; }
                if (MessageBox.Show(this, "Stop the CS2 download/update? It carries on where it stopped next time.", "SteamCMD", MessageBoxButtons.YesNo) != DialogResult.Yes) return;
                Components.KillSteamCmd();
                Log("SteamCMD stopped.");
            }));
            buttons.Insert(0, Theme.Btn("Check for updates", (s, e) => CheckForUpdates(false), true));
            buttons.Add(Theme.Btn("Refresh", (s, e) => RefreshComponents()));
            var note = Note("Metamod, CounterStrikeSharp and MatchZy come with the pack: only CS2 itself needs installing. After a CS2 update, " +
                            "'Install / update everything' fetches the newest plugin builds (GitHub, alliedmods.net) and CS2 (Steam).", 48);
            page.Controls.Add(_compList);
            page.Controls.Add(note);
            page.Controls.Add(Bar(84, buttons.ToArray()));
            RefreshComponents();
            return page;
        }

        private void RefreshComponents()
        {
            if (_compList == null) return;
            _compList.Items.Clear();
            try { foreach (var l in _comp.Status()) _compList.Items.Add(l); } catch (Exception e) { _compList.Items.Add(e.Message); }
            if (_updates != null)
            {
                _compList.Items.Add("");
                _compList.Items.Add("Updates (checked " + _updatesAt.ToString("HH:mm") + "):");
                foreach (var u in _updates) _compList.Items.Add("  " + u);
            }
        }

        // ------------------------------------------------------------ update check

        private List<Updates.Item> _updates;
        private DateTime _updatesAt;
        private string _lastUpdateKey = "";
        private System.Windows.Forms.Timer _updTimer;

        /// Asks Steam, alliedmods.net and GitHub for the newest versions. Quiet mode (startup / timer)
        /// only logs when something new turns up.
        private void CheckForUpdates(bool quiet)
        {
            List<Updates.Item> res = null;
            Action work = () => res = Updates.Check(_core);
            Action after = () =>
            {
                if (res == null) return;
                _updates = res; _updatesAt = DateTime.Now;
                RefreshComponents();
                var avail = res.Where(u => u.Available).ToList();
                var warn = res.Where(u => u.Note != null && u.Note.Contains("wait for")).ToList();
                var key = string.Join("|", avail.Select(u => u.Name + u.Latest).Concat(warn.Select(u => u.Note)).ToArray());
                if (quiet && key == _lastUpdateKey) return;
                _lastUpdateKey = key;
                if (avail.Count == 0 && warn.Count == 0) { if (!quiet) Log("Update check: everything is up to date."); return; }
                foreach (var u in avail) Log("Update available - " + u);
                foreach (var u in warn.Where(w => !w.Available)) Log("Note - " + u);
                if (avail.Count > 0) Log("Stop the server, then Components > 'Install / update everything' (or the single buttons).");
            };
            if (quiet)
            {
                new Thread(() => { try { work(); } catch { } BeginInvokeSafe(after); }) { IsBackground = true }.Start();
            }
            else Background("Update check", work, after);
        }

        // ============================================================ Maps

        private DataGridView _maps;
        private TextBox _wsInput;

        private TabPage BuildMaps()
        {
            var page = new TabPage("Maps");
            _maps = new DataGridView
            {
                Dock = DockStyle.Fill, BackgroundColor = Theme.Panel, BorderStyle = BorderStyle.None, AllowUserToAddRows = true,
                AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.Fill, RowHeadersVisible = false, EnableHeadersVisualStyles = false,
                GridColor = Theme.Field, SelectionMode = DataGridViewSelectionMode.FullRowSelect,
            };
            _maps.DefaultCellStyle.BackColor = Theme.Panel; _maps.DefaultCellStyle.ForeColor = Theme.Text;
            _maps.DefaultCellStyle.SelectionBackColor = Theme.Accent; _maps.ColumnHeadersDefaultCellStyle.BackColor = Theme.Field;
            _maps.ColumnHeadersDefaultCellStyle.ForeColor = Theme.Text;
            _maps.Columns.Add("name", "Map name (prefix decides the mode: am_ arenas, aim_/awp_/1v1_ duels, ar_ arms race)");
            _maps.Columns.Add("id", "Workshop ID (empty = official map)");
            _maps.Columns.Add("modes", "Modes (optional, comma separated)");
            _maps.Columns[0].FillWeight = 45; _maps.Columns[1].FillWeight = 25; _maps.Columns[2].FillWeight = 30;

            _wsInput = new TextBox { Width = 330 };
            var add = Theme.Btn("Add Workshop map", (s, e) => AddWorkshopMap(), true);
            var del = Theme.Btn("Remove selected", (s, e) =>
            {
                foreach (DataGridViewRow r in _maps.SelectedRows) if (!r.IsNewRow) _maps.Rows.Remove(r);
            });
            var save = Theme.Btn("Save maps", (s, e) => SaveMaps(), true);
            var startMap = Theme.Btn("Use as start map", (s, e) =>
            {
                if (_maps.CurrentRow == null || _maps.CurrentRow.IsNewRow) return;
                _core.S.StartMap = Convert.ToString(_maps.CurrentRow.Cells[0].Value);
                _core.SaveSettings();
                _startMap.Text = _core.S.StartMap;
                Log("Start map: " + _core.S.StartMap);
                RefreshInfo();
            });
            var suggest = Theme.Btn("Suggested maps...", (s, e) => SuggestedMaps());
            var hint = Note("Workshop maps download on the server by themselves the first time they load. Paste a link like steamcommunity.com/sharedfiles/filedetails/?id=123456789", 28);
            page.Controls.Add(_maps);
            page.Controls.Add(hint);
            page.Controls.Add(Bar(84, L("Workshop link or ID:"), _wsInput, add, del, startMap, suggest, save));
            LoadMapsGrid();
            return page;
        }

        private void LoadMapsGrid()
        {
            _maps.Rows.Clear();
            foreach (var m in _core.LoadMaps())
            {
                object v;
                var modes = m.TryGetValue("modes", out v) && v is object[] ? string.Join(", ", ((object[])v).Select(x => Convert.ToString(x)).ToArray()) : "";
                _maps.Rows.Add(Json.Str(m, "name"), Json.Str(m, "workshop_id"), modes);
            }
        }

        private void SaveMaps()
        {
            _maps.EndEdit();
            var list = new List<Dictionary<string, object>>();
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (DataGridViewRow r in _maps.Rows)
            {
                if (r.IsNewRow) continue;
                var name = Convert.ToString(r.Cells[0].Value).Trim();
                if (name.Length == 0 || !seen.Add(name)) continue;
                var id = Regex.Replace(Convert.ToString(r.Cells[1].Value) ?? "", "[^0-9]", "");
                var modes = (Convert.ToString(r.Cells[2].Value) ?? "").Split(new[] { ',', ' ' }, StringSplitOptions.RemoveEmptyEntries)
                    .Select(x => x.Trim().ToLowerInvariant()).Where(x => Modes.Keys.Contains(x)).Cast<object>().ToArray();
                list.Add(new Dictionary<string, object> { { "name", name }, { "workshop_id", id }, { "modes", modes } });
            }
            _core.SaveMaps(list);
            Log("Saved " + list.Count + " maps." + (_core.Running ? " Reloaded on the server." : ""));
            if (_core.Running) try { _core.RconExec("cs2pack_reload"); } catch { }
        }

        private static string WorkshopId(string text)
        {
            var m = Regex.Match(text ?? "", @"id=(\d+)");
            if (m.Success) return m.Groups[1].Value;
            m = Regex.Match(text ?? "", @"^\s*(\d{6,})\s*$");
            return m.Success ? m.Groups[1].Value : null;
        }

        private void AddWorkshopMap()
        {
            var id = WorkshopId(_wsInput.Text);
            if (id == null) { Log("That isn't a Workshop link or ID."); return; }
            Background("Workshop lookup", () =>
            {
                string title = "";
                try
                {
                    var r = Json.ParseObject(Http.Post("https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/", "itemcount=1&publishedfileids%5B0%5D=" + id));
                    var resp = r["response"] as Dictionary<string, object>;
                    var details = resp != null ? resp["publishedfiledetails"] as object[] : null;
                    if (details != null && details.Length > 0) title = Json.Str((Dictionary<string, object>)details[0], "title");
                }
                catch (Exception e) { Log("Couldn't read the Workshop title (" + e.Message + "), type the map name yourself."); }
                BeginInvokeSafe(() =>
                {
                    var guess = GuessMapName(title);
                    var name = Prompt("Map name for Workshop item " + id + (title.Length > 0 ? " (\"" + title + "\")" : "") +
                                      ":\nUse the real map name with its prefix (am_, aim_, awp_, 1v1_, ar_, de_ ...) so it lands in the right mode.", guess);
                    if (string.IsNullOrWhiteSpace(name)) return;
                    _maps.Rows.Add(name.Trim(), id, "");
                    SaveMaps();
                    _wsInput.Text = "";
                });
            });
        }

        private static string GuessMapName(string title)
        {
            var m = Regex.Match(title ?? "", @"\b((?:am|aim|awp|1v1|ar|de|cs|fy|gd)_[A-Za-z0-9_]+)");
            if (m.Success) return m.Groups[1].Value.ToLowerInvariant();
            return Regex.Replace((title ?? "").ToLowerInvariant(), "[^a-z0-9]+", "_").Trim('_');
        }

        private void SuggestedMaps()
        {
            MessageBox.Show(this,
                "Good places to find CS2 1v1 and arena maps:\n\n" +
                "- Steam Workshop, CS2 maps, search \"aim\", \"1v1\", \"awp\" (duels) or \"arena\", \"multi 1v1\" (arenas: map names start with am_)\n" +
                "- Arms Race: ar_baggage, ar_shoots, ar_pool_day are official\n\n" +
                "Open a map's Workshop page, copy the link, paste it here and press 'Add Workshop map'.",
                "Suggested maps", MessageBoxButtons.OK, MessageBoxIcon.Information);
            Process.Start("https://steamcommunity.com/workshop/browse/?appid=730&searchtext=aim_&browsesort=trend&section=readytouseitems");
        }

        private string Prompt(string text, string value)
        {
            using (var f = new Form { Text = "CS2 Server Control", Size = new Size(520, 200), StartPosition = FormStartPosition.CenterParent, BackColor = Theme.Bg, ForeColor = Theme.Text, FormBorderStyle = FormBorderStyle.FixedDialog, MaximizeBox = false, MinimizeBox = false })
            {
                var l = new Label { Text = text, Dock = DockStyle.Top, Height = 70, Padding = new Padding(8) };
                var t = new TextBox { Text = value, Dock = DockStyle.Top, BackColor = Theme.Field, ForeColor = Theme.Text };
                var ok = Theme.Btn("OK", (s, e) => f.DialogResult = DialogResult.OK, true);
                var bar = Row(ok);
                bar.Dock = DockStyle.Bottom;
                f.Controls.Add(t); f.Controls.Add(l); f.Controls.Add(bar);
                f.AcceptButton = ok;
                return f.ShowDialog(this) == DialogResult.OK ? t.Text : null;
            }
        }

        // ============================================================ Players

        private DataGridView _players;
        private TextBox _say;

        private TabPage BuildPlayers()
        {
            var page = new TabPage("Players");
            _players = new DataGridView
            {
                Dock = DockStyle.Fill, BackgroundColor = Theme.Panel, BorderStyle = BorderStyle.None, AllowUserToAddRows = false, ReadOnly = true,
                AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.Fill, RowHeadersVisible = false, EnableHeadersVisualStyles = false,
                SelectionMode = DataGridViewSelectionMode.FullRowSelect, GridColor = Theme.Field,
            };
            _players.DefaultCellStyle.BackColor = Theme.Panel; _players.DefaultCellStyle.ForeColor = Theme.Text;
            _players.DefaultCellStyle.SelectionBackColor = Theme.Accent;
            _players.ColumnHeadersDefaultCellStyle.BackColor = Theme.Field; _players.ColumnHeadersDefaultCellStyle.ForeColor = Theme.Text;
            _players.Columns.Add("id", "ID"); _players.Columns.Add("name", "Name"); _players.Columns.Add("time", "Time");
            _players.Columns.Add("ping", "Ping"); _players.Columns.Add("addr", "Address");
            _players.Columns[0].FillWeight = 10; _players.Columns[1].FillWeight = 40;

            _say = new TextBox { Width = 380 };
            var refresh = Theme.Btn("Refresh", (s, e) => RefreshPlayers(), true);
            var kick = Theme.Btn("Kick selected", (s, e) =>
            {
                if (_players.CurrentRow == null) return;
                var id = Convert.ToString(_players.CurrentRow.Cells[0].Value);
                var name = Convert.ToString(_players.CurrentRow.Cells[1].Value);
                if (MessageBox.Show(this, "Kick " + name + "?", "Kick", MessageBoxButtons.YesNo) != DialogResult.Yes) return;
                Background("Kick", () => { Log(_core.RconExec("kickid " + id + " \"Kicked by admin\"").Trim()); }, RefreshPlayers);
            });
            var say = Theme.Btn("Say", (s, e) =>
            {
                var msg = _say.Text.Trim().Replace("\"", "'").Replace(";", ",");
                if (msg.Length == 0) return;
                Background("Say", () => _core.RconExec("say \"" + msg + "\""));
                _say.Text = "";
            });
            page.Controls.Add(_players);
            page.Controls.Add(Bar(44, refresh, kick, L("  Chat:"), _say, say));
            return page;
        }

        private void RefreshPlayers()
        {
            if (!_core.Running) { Log("Server isn't running."); return; }
            Background("Player list", () =>
            {
                var status = _core.RconExec("status");
                var rows = new List<string[]>();
                // CS2: "  2 00:21   30    0     active 786432 1.2.3.4:27005 'Name'"
                var re = new Regex(@"^\s*(\d+)\s+(BOT|[\d:]+)\s+(\d+)\s+\d+\s+\S+\s+\d+\s+(\S+)?\s*'(.*)'\s*$");
                foreach (var line in status.Split('\n'))
                {
                    var m = re.Match(line.TrimEnd('\r'));
                    if (m.Success) rows.Add(new[] { m.Groups[1].Value, m.Groups[5].Value, m.Groups[2].Value, m.Groups[3].Value, m.Groups[4].Value });
                }
                BeginInvokeSafe(() =>
                {
                    _players.Rows.Clear();
                    foreach (var r in rows) _players.Rows.Add(r);
                    if (rows.Count == 0) Log("No players (or the status output wasn't recognised).");
                });
            });
        }

        // ============================================================ Console

        private TextBox _con, _cmd;
        private long _conPos;
        private System.Windows.Forms.Timer _conTimer;

        private TabPage BuildConsole()
        {
            var page = new TabPage("Console");
            _con = new TextBox
            {
                Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Both, WordWrap = false, Dock = DockStyle.Fill,
                Font = Theme.Mono, BackColor = Color.Black, ForeColor = Color.FromArgb(200, 220, 200), BorderStyle = BorderStyle.None,
            };
            _cmd = new TextBox { Dock = DockStyle.Fill, Font = Theme.Mono };
            _cmd.KeyDown += (s, e) => { if (e.KeyCode == Keys.Enter) { e.SuppressKeyPress = true; SendCmd(); } };
            var send = Theme.Btn("Send (RCON)", (s, e) => SendCmd(), true);
            send.Dock = DockStyle.Right;
            var bottom = new Panel { Dock = DockStyle.Bottom, Height = 34, Padding = new Padding(0, 4, 0, 0) };
            bottom.Controls.Add(_cmd);
            bottom.Controls.Add(send);
            page.Controls.Add(_con);
            page.Controls.Add(bottom);

            _conTimer = new System.Windows.Forms.Timer { Interval = 1000 };
            _conTimer.Tick += (s, e) => TailConsole();
            _conTimer.Start();
            return page;
        }

        private void TailConsole()
        {
            try
            {
                var f = _core.ConsoleLog;
                if (!File.Exists(f)) { _conPos = 0; return; }
                using (var fs = new FileStream(f, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                {
                    if (fs.Length < _conPos) { _conPos = 0; _con.Clear(); }
                    if (fs.Length == _conPos) return;
                    if (_conPos == 0 && fs.Length > 200000) _conPos = fs.Length - 200000;
                    fs.Seek(_conPos, SeekOrigin.Begin);
                    using (var sr = new StreamReader(fs, Encoding.UTF8))
                    {
                        var text = sr.ReadToEnd();
                        _conPos = fs.Length;
                        _con.AppendText(text.Replace("\r\n", "\n").Replace("\n", Environment.NewLine));
                    }
                }
                if (_con.TextLength > 400000) _con.Text = _con.Text.Substring(_con.TextLength - 200000);
            }
            catch { }
        }

        private void SendCmd()
        {
            var c = _cmd.Text.Trim();
            if (c.Length == 0) return;
            _cmd.Text = "";
            _con.AppendText("> " + c + Environment.NewLine);
            if (!_core.Running) { _con.AppendText("(server isn't running)" + Environment.NewLine); return; }
            ThreadPool.QueueUserWorkItem(_ =>
            {
                string r;
                try { r = _core.RconExec(c); } catch (Exception e) { r = "RCON error: " + e.Message; }
                // CS2 answers with \n line ends; a WinForms TextBox needs \r\n.
                BeginInvokeSafe(() => { if (!string.IsNullOrWhiteSpace(r)) _con.AppendText(r.TrimEnd().Replace("\r\n", "\n").Replace("\n", Environment.NewLine) + Environment.NewLine); });
            });
        }
    }

    internal static class Program
    {
        [STAThread]
        private static void Main(string[] args)
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.ThreadException += (s, e) => MessageBox.Show(e.Exception.Message, "CS2 Server Control", MessageBoxButtons.OK, MessageBoxIcon.Error);
            // Players: patch your own CS2 to the server's tick rate without the server parts.
            if (args.Any(x => x.Equals("--game-tick", StringComparison.OrdinalIgnoreCase)))
            {
                try { TickPatch.OpenPatcherWindow(); }
                catch (Exception ex) { MessageBox.Show(ex.Message, "CS2 Server Control", MessageBoxButtons.OK, MessageBoxIcon.Error); }
                return;
            }
            using (var m = new System.Threading.Mutex(true, "CS2ServerControl_" + AppDomain.CurrentDomain.BaseDirectory.GetHashCode()))
            {
                if (!m.WaitOne(0)) { MessageBox.Show("CS2 Server Control is already open for this server.", "CS2 Server Control"); return; }
                Application.Run(new MainForm());
            }
        }
    }
}
