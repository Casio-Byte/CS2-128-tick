using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Attributes;
using CounterStrikeSharp.API.Modules.Admin;
using CounterStrikeSharp.API.Modules.Commands;
using CounterStrikeSharp.API.Modules.Cvars;
using CounterStrikeSharp.API.Modules.Menu;
using CounterStrikeSharp.API.Modules.Timers;
using CounterStrikeSharp.API.Modules.Utils;

namespace CS2Match;

[MinimumApiVersion(80)]
public partial class CS2Match : BasePlugin
{
    public override string ModuleName => "CS2 Match";
    public override string ModuleVersion => "1.0.0";
    public override string ModuleAuthor => "Casio";
    public override string ModuleDescription => "Game modes, map votes, 1v1 duels, arenas and /login admin for the CS2 Server Pack";

    public static string Prefix => $" {ChatColors.Green}[CS2]{ChatColors.Default} ";

    private string _cfgDir = "";
    private string _pluginsDir = "";
    internal Settings Settings = new();
    internal MapsFile MapsFile = new();
    internal State State = new();
    internal Mode CurrentMode = Mode.Competitive;

    private float _mapStartTime;
    private bool _modeSwitchPending;
    private string _reloadedForMode = "";

    // ---------------------------------------------------------------- lifecycle

    public override void Load(bool hotReload)
    {
        _cfgDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "configs", "plugins", "CS2Match"));
        _pluginsDir = Directory.GetParent(ModuleDirectory)!.FullName;
        LoadFiles();

        RegisterListener<Listeners.OnMapStart>(OnMapStart);
        RegisterListener<Listeners.OnClientDisconnect>(OnClientDisconnect);
        RegisterEventHandler<EventPlayerConnectFull>(OnPlayerConnectFull);
        RegisterEventHandler<EventPlayerSpawn>(OnPlayerSpawn);
        RegisterEventHandler<EventPlayerDeath>(OnPlayerDeath);
        RegisterEventHandler<EventRoundStart>(OnRoundStart);
        RegisterEventHandler<EventRoundEnd>(OnRoundEnd);
        RegisterEventHandler<EventItemPickup>(OnItemPickup);

        AddCommand("css_login", "Log in as admin: /login <password>", CmdLogin);
        AddCommand("css_logout", "Drop admin rights", CmdLogout);
        AddCommand("css_gamemode", "Admin: switch game mode", CmdGameMode);
        AddCommand("css_mode", "Admin: switch game mode", CmdGameMode);
        AddCommand("css_maps", "Admin: change map (fitting the current mode)", CmdMaps);
        AddCommand("css_changemap", "Admin: change map", CmdMaps);
        AddCommand("css_status", "Shows the current mode and map", CmdStatus);
        AddCommand("css_tickrate", "Shows the server tick rate (set and measured)", CmdTickrate);
        AddCommand("css_tick", "Shows the server tick rate (set and measured)", CmdTickrate);
        AddCommand("css_commands", "Lists the commands for the current mode", CmdCommands);
        AddCommand("css_cmds", "Lists the commands for the current mode", CmdCommands);
        AddCommand("css_1v1", "1v1 menu", Cmd1v1);
        AddCommand("cs2pack_reload", "Reloads the CS2 Match settings and map list", (p, i) =>
        {
            if (!IsAdmin(p)) { Reply(p, "Admins only."); return; }
            LoadFiles();
            Reply(p, "Settings and map list reloaded.");
        });
        AddCommand("cs2pack_setmode", "Server: cs2pack_setmode <mode> [map]", (p, i) =>
        {
            if (!IsAdmin(p)) return;
            if (i.ArgCount < 2 || !Modes.TryParse(i.GetArg(1), out var m)) { Reply(p, "Usage: cs2pack_setmode <mode> [map]"); return; }
            var map = i.ArgCount > 2 ? FindMap(i.GetArg(2)) : null;
            SwitchMode(m, map ?? PickMapFor(m), p);
        });

        RegisterVoteCommands();
        RegisterDuelCommands();
        RegisterArenaCommands();

        if (hotReload) OnMapStart(Server.MapName);
    }

    private void LoadFiles()
    {
        Settings = Json.Load(Path.Combine(_cfgDir, "settings.json"), () => new Settings());
        MapsFile = Json.Load(Path.Combine(_cfgDir, "maps.json"), MapsFile.Default);
        State = Json.Load(Path.Combine(_cfgDir, "state.json"), () => new State());
        var modeKey = string.IsNullOrEmpty(State.Mode) ? Settings.DefaultMode : State.Mode;
        CurrentMode = Modes.TryParse(modeKey, out var m) ? m : Mode.Competitive;
    }

    internal void SaveState() => Json.Save(Path.Combine(_cfgDir, "state.json"), State);

    private void OnMapStart(string mapName)
    {
        _mapStartTime = Server.CurrentTime;
        _modeSwitchPending = false;
        ResetVotes();
        ResetDuelsOnMapChange();
        ResetArenasOnMapChange();
        // server.cfg / gamemode cfgs run as the map loads; ours goes on top of them.
        AddTimer(2.0f, ApplyModeRuntime, TimerFlags.STOP_ON_MAPCHANGE);
    }

    /// Runs each map start: the mode's cfg and the companion plugins it needs.
    private void ApplyModeRuntime()
    {
        var (gt, gm) = CurrentMode.GameTypeMode();
        var cvGt = ConVar.Find("game_type")?.GetPrimitiveValue<int>();
        var cvGm = ConVar.Find("game_mode")?.GetPrimitiveValue<int>();
        if (cvGt != null && cvGm != null && (cvGt != gt || cvGm != gm) && _reloadedForMode != CurrentMode.Key() + Server.MapName)
        {
            _reloadedForMode = CurrentMode.Key() + Server.MapName; // only once, never a reload loop
            // The map was loaded with another game_type/mode (e.g. started from the console).
            // Put the right one in place and reload the map once so it really takes effect.
            Logger_Info($"Map loaded as game_type {cvGt}/game_mode {cvGm}, {CurrentMode.Key()} needs {gt}/{gm}: reloading map.");
            Server.ExecuteCommand($"game_type {gt}; game_mode {gm}");
            var entry = FindMap(Server.MapName);
            AddTimer(1.0f, () => ChangeLevel(entry ?? new MapEntry { Name = Server.MapName }));
            return;
        }

        // MatchZy first (it is only loaded in the modes that use it), then the server's settings and the mode cfg.
        SetCompanionPlugins(CurrentMode);
        var mode = CurrentMode;
        AddTimer(1.5f, () =>
        {
            Server.ExecuteCommand("exec cs2pack/server_settings.cfg");
            Server.ExecuteCommand($"exec cs2pack/mode_{mode.Key()}.cfg");
            switch (mode)
            {
                case Mode.Arenas:
                    BuildArenas();
                    break;
                case Mode.Duels:
                    StartDuelLobby();
                    break;
            }
        }, TimerFlags.STOP_ON_MAPCHANGE);
    }

    // ---------------------------------------------------------------- companion plugins

    private readonly record struct Companion(string Match, Mode[] NeededIn);

    private static readonly Companion[] Companions =
    [
        new("matchzy", [Mode.Competitive, Mode.Wingman, Mode.Practice]),
    ];

    /// Companion plugins currently loaded. CounterStrikeSharp loads every plugin at startup, so they all start as loaded.
    private HashSet<string>? _loadedCompanionsSet;
    private HashSet<string> _loadedCompanions
    {
        get
        {
            if (_loadedCompanionsSet == null)
            {
                _loadedCompanionsSet = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach (var c in Companions)
                    foreach (var f in FindPluginFolders(c.Match))
                        _loadedCompanionsSet.Add(CompanionDll(f));
            }
            return _loadedCompanionsSet;
        }
    }

    /// The .dll path of another plugin, written exactly like CounterStrikeSharp writes ModulePath.
    /// Its root is e.g. "C:\...\game\csgo/addons/counterstrikesharp" (mixed slashes) and plugin paths are
    /// compared as plain strings, so the path is cut from our own ModulePath by string operations only.
    private string CompanionDll(string folder)
    {
        var own = ModulePath;                                  // ...plugins\CS2Match\CS2Match.dll
        var cut = own.LastIndexOfAny(['\\', '/']);              // before CS2Match.dll
        cut = cut > 0 ? own.LastIndexOfAny(['\\', '/'], cut - 1) : -1; // before CS2Match\\
        var sep = cut >= 0 ? own[cut] : Path.DirectorySeparatorChar;
        var baseDir = cut >= 0 ? own[..(cut + 1)] : _pluginsDir + sep;
        return baseDir + folder + sep + folder + ".dll";
    }

    private IEnumerable<string> FindPluginFolders(string match)
    {
        if (!Directory.Exists(_pluginsDir)) yield break;
        foreach (var dir in Directory.GetDirectories(_pluginsDir))
        {
            var name = Path.GetFileName(dir);
            if (name.Equals("disabled", StringComparison.OrdinalIgnoreCase)) continue;
            if (!name.Contains(match, StringComparison.OrdinalIgnoreCase)) continue;
            if (!File.Exists(Path.Combine(dir, name + ".dll"))) continue;
            yield return name;
        }
    }

    private void SetCompanionPlugins(Mode mode)
    {
        foreach (var c in Companions)
        {
            var want = c.NeededIn.Contains(mode);
            foreach (var folder in FindPluginFolders(c.Match))
            {
                // Always the full .dll path, built the same way CounterStrikeSharp builds ModulePath
                // (Path.Combine). Its "css_plugins load/unload <Name>" shortcut makes ".../plugins/Name/Name.dll"
                // with forward slashes, which never equals the real path on Windows: unload then fails and
                // load loads a SECOND copy of an already loaded plugin.
                var dll = CompanionDll(folder);
                var loaded = _loadedCompanions.Contains(dll);
                if (want && !loaded) { Server.ExecuteCommand($"css_plugins load \"{dll}\""); _loadedCompanions.Add(dll); }
                else if (!want && loaded) { Server.ExecuteCommand($"css_plugins unload \"{dll}\""); _loadedCompanions.Remove(dll); }
            }
        }

        // Queued right behind the unloads, so anything a plugin's unload cfg changed is put back at once.
        Server.ExecuteCommand("exec cs2pack/server_settings.cfg");

        // Put MatchZy in the right state once it has loaded.
        if (mode == Mode.Practice)
            AddTimer(3.0f, () => Server.ExecuteCommand("css_prac"), TimerFlags.STOP_ON_MAPCHANGE);
        else if (mode is Mode.Competitive or Mode.Wingman)
            AddTimer(3.0f, () => Server.ExecuteCommand("css_match"), TimerFlags.STOP_ON_MAPCHANGE);
    }

    // ---------------------------------------------------------------- maps and modes

    internal MapEntry? FindMap(string name)
    {
        name = name.Trim();
        if (name.Length == 0) return null;
        var exact = MapsFile.Maps.FirstOrDefault(m =>
            m.Name.Equals(name, StringComparison.OrdinalIgnoreCase) || m.WorkshopId == name);
        if (exact != null) return exact;
        var partial = MapsFile.Maps.Where(m => m.Name.Contains(name, StringComparison.OrdinalIgnoreCase)).ToList();
        if (partial.Count == 1) return partial[0];
        if (Server.IsMapValid(name)) return new MapEntry { Name = name };
        return null;
    }

    internal bool MapAvailable(MapEntry m) => m.IsWorkshop || Server.IsMapValid(m.Name);

    internal List<MapEntry> MapsFor(Mode mode) =>
        MapsFile.Maps.Where(m => m.Fits(mode) && MapAvailable(m))
            .OrderBy(m => m.Name, StringComparer.OrdinalIgnoreCase).ToList();

    internal MapEntry? PickMapFor(Mode mode)
    {
        var maps = MapsFor(mode);
        if (maps.Count == 0) return null;
        var current = maps.FirstOrDefault(m => m.Name.Equals(Server.MapName, StringComparison.OrdinalIgnoreCase));
        return current ?? maps[Random.Shared.Next(maps.Count)];
    }

    internal bool IsCurrentMap(MapEntry m) => m.Name.Equals(Server.MapName, StringComparison.OrdinalIgnoreCase);

    private void ChangeLevel(MapEntry map)
    {
        if (map.IsWorkshop) Server.ExecuteCommand($"host_workshop_map {map.WorkshopId}");
        else Server.ExecuteCommand($"changelevel {map.Name}");
    }

    internal void SwitchMode(Mode mode, MapEntry? map, CCSPlayerController? by)
    {
        if (map == null)
        {
            Reply(by, $"{mode.Title()} has no maps installed. Add some in CS2 Server Control.exe (Maps tab).");
            return;
        }
        if (!map.Fits(mode))
        {
            Reply(by, $"{map.Name} isn't made for {mode.Title()}.");
            return;
        }
        if (_modeSwitchPending) { Reply(by, "A map change is already on the way."); return; }
        _modeSwitchPending = true;

        CurrentMode = mode;
        State.Mode = mode.Key();
        SaveState();

        var (gt, gm) = mode.GameTypeMode();
        Server.ExecuteCommand($"game_type {gt}; game_mode {gm}");
        Server.PrintToChatAll($"{Prefix}Switching to {ChatColors.Gold}{mode.Title()}{ChatColors.Default} on {ChatColors.Gold}{map.Name}{ChatColors.Default} in 5 seconds.");
        AddTimer(5.0f, () => ChangeLevel(map), TimerFlags.STOP_ON_MAPCHANGE);
    }

    internal void ChangeMapSameMode(MapEntry map, CCSPlayerController? by)
    {
        if (_modeSwitchPending) { Reply(by, "A map change is already on the way."); return; }
        _modeSwitchPending = true;
        Server.PrintToChatAll($"{Prefix}Changing map to {ChatColors.Gold}{map.Name}{ChatColors.Default} in 5 seconds.");
        AddTimer(5.0f, () => ChangeLevel(map), TimerFlags.STOP_ON_MAPCHANGE);
    }

    /// Live match = MatchZy mode out of its warmup / ready-up phase.
    internal bool LiveMatch()
    {
        if (CurrentMode is not (Mode.Competitive or Mode.Wingman)) return false;
        var rules = Rules();
        return rules != null && !rules.WarmupPeriod;
    }

    internal static CCSGameRules? Rules() =>
        Utilities.FindAllEntitiesByDesignerName<CCSGameRulesProxy>("cs_gamerules").FirstOrDefault()?.GameRules;

    // ---------------------------------------------------------------- commands

    private void CmdGameMode(CCSPlayerController? player, CommandInfo info)
    {
        if (!IsAdmin(player)) { Reply(player, "Admins only. Use /login <password> first."); return; }

        if (info.ArgCount >= 2 && Modes.TryParse(info.GetArg(1), out var direct))
        {
            var map = info.ArgCount >= 3 ? FindMap(info.GetArg(2)) : PickMapFor(direct);
            SwitchMode(direct, map, player);
            return;
        }
        if (player == null) { Reply(null, "Usage: css_gamemode <mode> [map]"); return; }

        var menu = new ChatMenu($"Game mode (now: {CurrentMode.Title()})");
        foreach (var m in Modes.All)
        {
            var count = MapsFor(m).Count;
            var label = count > 0 ? m.Title() : $"{m.Title()} (no maps installed)";
            menu.AddMenuOption(label, (p, _) => OpenMapMenu(p, m, true), count == 0);
        }
        MenuManager.OpenChatMenu(player, menu);
    }

    private void OpenMapMenu(CCSPlayerController player, Mode mode, bool switchMode)
    {
        var menu = new ChatMenu($"{mode.Title()}: pick a map");
        foreach (var map in MapsFor(mode))
        {
            var label = IsCurrentMap(map) ? $"{map.Name} (current)" : map.Name;
            menu.AddMenuOption(label, (p, _) =>
            {
                if (switchMode || mode != CurrentMode) SwitchMode(mode, map, p);
                else ChangeMapSameMode(map, p);
            });
        }
        MenuManager.OpenChatMenu(player, menu);
    }

    private void CmdMaps(CCSPlayerController? player, CommandInfo info)
    {
        if (!IsAdmin(player)) { Reply(player, "Admins only. Use /login <password> first."); return; }
        if (info.ArgCount >= 2)
        {
            var map = FindMap(info.GetArg(1));
            if (map == null) { Reply(player, $"No map called {info.GetArg(1)}."); return; }
            if (map.Fits(CurrentMode)) { ChangeMapSameMode(map, player); return; }

            var fitting = map.Modes().ToList();
            if (fitting.Count == 0) { Reply(player, $"{map.Name} doesn't fit any mode."); return; }
            if (player == null) { SwitchMode(fitting[0], map, null); return; }
            var ask = new ChatMenu($"{map.Name} doesn't suit {CurrentMode.Title()}. Switch to:");
            foreach (var m in fitting) ask.AddMenuOption(m.Title(), (p, _) => SwitchMode(m, map, p));
            MenuManager.OpenChatMenu(player, ask);
            return;
        }
        if (player == null) { Reply(null, "Usage: css_maps <map>"); return; }
        OpenMapMenu(player, CurrentMode, false);
    }

    private void CmdStatus(CCSPlayerController? player, CommandInfo info)
    {
        Reply(player, $"Mode: {ChatColors.Gold}{CurrentMode.Title()}{ChatColors.Default}, map: {ChatColors.Gold}{Server.MapName}{ChatColors.Default}, players: {RealPlayers().Count()}");
        if (CurrentMode == Mode.Duels) ReplyDuelStatus(player);
    }

    /// Tick rate from the engine's tick interval, then measured over 3 seconds of real time.
    private void CmdTickrate(CCSPlayerController? player, CommandInfo info)
    {
        var interval = Server.TickInterval;
        var set = interval > 0 ? 1f / interval : 0;
        Reply(player, $"Server tick rate: {ChatColors.Gold}{set:0.#}{ChatColors.Default} (tick interval {interval * 1000:0.###} ms). Measuring for 3 s...");
        int t0 = Server.TickCount;
        double e0 = Server.EngineTime;
        AddTimer(3.0f, () =>
        {
            var secs = Server.EngineTime - e0;
            var ticks = Server.TickCount - t0;
            var measured = secs > 0 ? ticks / secs : 0;
            var p = player != null && player.IsValid ? player : null;
            Reply(p, $"Measured: {ChatColors.Gold}{measured:0.0}{ChatColors.Default} ticks per second ({ticks} ticks in {secs:0.00} s).");
        });
    }

    private void CmdCommands(CCSPlayerController? player, CommandInfo info)
    {
        var lines = new List<string> { "!status !tickrate !rtv !rtg !commands" };
        switch (CurrentMode)
        {
            case Mode.Competitive:
            case Mode.Wingman:
                lines.Add("MatchZy: .ready .unready .pause .tech .unpause .stop .stay .switch .help");
                break;
            case Mode.Practice:
                lines.Add("MatchZy practice: .spawn .bot .clear .god .traj .noflash .help");
                break;
            case Mode.Duels:
                lines.Add("!duel [name] !accept !decline !forfeit !1v1");
                break;
            case Mode.Arenas:
                lines.Add("!guns !sitout !rank !duel !accept !decline !forfeit");
                break;
        }
        if (IsAdmin(player)) lines.Add("Admin: !gamemode !maps <map> !logout");
        else lines.Add("Admin: /login <password>");
        foreach (var l in lines) Reply(player, l);
    }

    private void Cmd1v1(CCSPlayerController? player, CommandInfo info)
    {
        if (player == null) return;
        var menu = new ChatMenu("1v1");
        if (CurrentMode is Mode.Duels or Mode.Arenas)
            menu.AddMenuOption("Challenge someone (!duel)", (p, _) => OpenDuelOpponentMenu(p));
        else
            menu.AddMenuOption("Duels only work in 1v1 duels / arenas", (_, _) => { }, true);
        var admin = IsAdmin(player);
        menu.AddMenuOption($"Switch to {Mode.Duels.Title()}" + (MapsFor(Mode.Duels).Count == 0 ? " (no maps installed)" : ""),
            (p, _) => OpenMapMenu(p, Mode.Duels, true), !admin || MapsFor(Mode.Duels).Count == 0);
        menu.AddMenuOption($"Switch to {Mode.Arenas.Title()}" + (MapsFor(Mode.Arenas).Count == 0 ? " (no maps installed)" : ""),
            (p, _) => OpenMapMenu(p, Mode.Arenas, true), !admin || MapsFor(Mode.Arenas).Count == 0);
        MenuManager.OpenChatMenu(player, menu);
    }

    // ---------------------------------------------------------------- admin login

    private readonly Dictionary<ulong, (int fails, DateTime until)> _loginFails = new();

    private void CmdLogin(CCSPlayerController? player, CommandInfo info)
    {
        if (player == null || !player.IsValid) return;
        var id = player.SteamID;
        if (_loginFails.TryGetValue(id, out var f) && f.until > DateTime.UtcNow)
        {
            Reply(player, $"Too many wrong passwords. Try again in {(int)(f.until - DateTime.UtcNow).TotalSeconds + 1} s.");
            return;
        }
        if (string.IsNullOrEmpty(Settings.AdminPassword))
        {
            Reply(player, "No admin password is set. Set one in CS2 Server Control.exe (Setup tab).");
            return;
        }
        var given = info.ArgString.Trim().Trim('"');
        if (given != Settings.AdminPassword)
        {
            var fails = (f.fails + 1);
            _loginFails[id] = fails >= 3 ? (0, DateTime.UtcNow.AddMinutes(1)) : (fails, DateTime.MinValue);
            Reply(player, fails >= 3 ? "Wrong password. Locked out for 1 minute." : "Wrong password.");
            return;
        }
        _loginFails.Remove(id);
        AdminManager.AddPlayerPermissions(player, "@css/root");
        if (!State.RememberedAdmins.Contains(id)) { State.RememberedAdmins.Add(id); SaveState(); }
        Reply(player, $"{ChatColors.Lime}You're admin now{ChatColors.Default} (remembered for next time). !commands for the list.");
        Logger_Info($"{player.PlayerName} ({id}) logged in as admin.");
    }

    private void CmdLogout(CCSPlayerController? player, CommandInfo info)
    {
        if (player == null) return;
        AdminManager.RemovePlayerPermissions(player, "@css/root");
        State.RememberedAdmins.Remove(player.SteamID);
        SaveState();
        Reply(player, "Admin rights removed.");
    }

    internal static bool IsAdmin(CCSPlayerController? player) =>
        player == null || AdminManager.PlayerHasPermissions(player, "@css/root") ||
        AdminManager.PlayerHasPermissions(player, "@css/changemap");

    // ---------------------------------------------------------------- events

    private HookResult OnPlayerConnectFull(EventPlayerConnectFull ev, GameEventInfo info)
    {
        var p = ev.Userid;
        if (p == null || !p.IsValid || p.IsBot) return HookResult.Continue;

        if (State.RememberedAdmins.Contains(p.SteamID))
            AdminManager.AddPlayerPermissions(p, "@css/root");

        if (Settings.SkipTeamMenu)
        {
            AddTimer(1.0f, () =>
            {
                if (!p.IsValid || p.Team is CsTeam.Terrorist or CsTeam.CounterTerrorist) return;
                if (CurrentMode == Mode.Duels && DuelRunning) { p.ChangeTeam(CsTeam.Spectator); return; }
                p.ChangeTeam(SmallerTeam());
            }, TimerFlags.STOP_ON_MAPCHANGE);
        }

        AddTimer(4.0f, () =>
        {
            if (p.IsValid)
                p.PrintToChat($"{Prefix}Playing {ChatColors.Gold}{CurrentMode.Title()}{ChatColors.Default}. Type {ChatColors.Gold}!commands{ChatColors.Default} for the commands.");
        }, TimerFlags.STOP_ON_MAPCHANGE);
        return HookResult.Continue;
    }

    private void OnClientDisconnect(int slot)
    {
        OnDisconnectVotes(slot);
        OnDisconnectDuels(slot);
        OnDisconnectArenas(slot);
    }

    private HookResult OnPlayerSpawn(EventPlayerSpawn ev, GameEventInfo info)
    {
        var p = ev.Userid;
        if (p == null || !p.IsValid) return HookResult.Continue;
        switch (CurrentMode)
        {
            case Mode.Deathmatch:
                AddTimer(0.1f, () => GiveLoadout(p, Settings.DmWeapons), TimerFlags.STOP_ON_MAPCHANGE);
                break;
            case Mode.Duels:
                DuelsOnSpawn(p);
                break;
        }
        return HookResult.Continue;
    }

    private HookResult OnPlayerDeath(EventPlayerDeath ev, GameEventInfo info)
    {
        switch (CurrentMode)
        {
            case Mode.Deathmatch:
                var a = ev.Attacker;
                if (a != null && a.IsValid && a != ev.Userid && a.PawnIsAlive && Settings.DmKillHp > 0)
                {
                    var pawn = a.PlayerPawn.Value;
                    if (pawn != null && pawn.IsValid)
                    {
                        pawn.Health = Math.Min(100, pawn.Health + Settings.DmKillHp);
                        Utilities.SetStateChanged(pawn, "CBaseEntity", "m_iHealth");
                    }
                }
                break;
            case Mode.Arenas:
                ArenasOnDeath(ev);
                break;
        }
        return HookResult.Continue;
    }

    private HookResult OnRoundStart(EventRoundStart ev, GameEventInfo info)
    {
        if (CurrentMode == Mode.Arenas) AddTimer(0.2f, ArenasOnRoundStart, TimerFlags.STOP_ON_MAPCHANGE);
        if (CurrentMode == Mode.Duels) DuelsOnRoundStart();
        return HookResult.Continue;
    }

    private HookResult OnRoundEnd(EventRoundEnd ev, GameEventInfo info)
    {
        if (CurrentMode == Mode.Duels) DuelsOnRoundEnd(ev);
        if (CurrentMode == Mode.Arenas) ArenasOnRoundEnd();
        return HookResult.Continue;
    }

    /// Deathmatch: only the allowed guns (plus knife). Anything else picked up is removed.
    private HookResult OnItemPickup(EventItemPickup ev, GameEventInfo info)
    {
        if (CurrentMode != Mode.Deathmatch) return HookResult.Continue;
        var p = ev.Userid;
        if (p == null || !p.IsValid) return HookResult.Continue;
        var item = "weapon_" + ev.Item;
        if (item.Contains("knife") || item.Contains("bayonet") || Settings.DmWeapons.Contains(item)) return HookResult.Continue;
        Server.NextFrame(() =>
        {
            var pawn = p.PlayerPawn.Value;
            if (pawn?.WeaponServices == null) return;
            foreach (var h in pawn.WeaponServices.MyWeapons)
            {
                var w = h.Value;
                if (w != null && w.IsValid && w.DesignerName == item)
                    w.AddEntityIOEvent("Kill", w, null, "", 0.01f);
            }
        });
        return HookResult.Continue;
    }

    // ---------------------------------------------------------------- helpers

    internal static IEnumerable<CCSPlayerController> RealPlayers() =>
        Utilities.GetPlayers().Where(p => p.IsValid && !p.IsBot && !p.IsHLTV && p.Connected == PlayerConnectedState.Connected);

    internal static IEnumerable<CCSPlayerController> Playing() =>
        Utilities.GetPlayers().Where(p => p.IsValid && !p.IsHLTV && p.Team is CsTeam.Terrorist or CsTeam.CounterTerrorist);

    internal static CsTeam SmallerTeam()
    {
        var t = Playing().Count(p => p.Team == CsTeam.Terrorist);
        var ct = Playing().Count(p => p.Team == CsTeam.CounterTerrorist);
        return t <= ct ? CsTeam.Terrorist : CsTeam.CounterTerrorist;
    }

    internal static void GiveLoadout(CCSPlayerController p, IEnumerable<string> weapons)
    {
        if (!p.IsValid || !p.PawnIsAlive) return;
        p.RemoveWeapons();
        p.GiveNamedItem("weapon_knife");
        foreach (var w in weapons) p.GiveNamedItem(w);
    }

    internal static void Reply(CCSPlayerController? player, string msg)
    {
        if (player == null || !player.IsValid) Server.PrintToConsole("[CS2 Match] " + StripColors(msg));
        else player.PrintToChat(Prefix + msg);
    }

    private static string StripColors(string s) => new(s.Where(c => c >= ' ').ToArray());

    internal void Logger_Info(string msg) => Server.PrintToConsole("[CS2 Match] " + msg);
}
