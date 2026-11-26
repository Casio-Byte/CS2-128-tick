using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Commands;
using CounterStrikeSharp.API.Modules.Menu;
using CounterStrikeSharp.API.Modules.Timers;
using CounterStrikeSharp.API.Modules.Utils;

namespace CS2Match;

/// !rtv (rock the vote) and !rtg (rock the game mode).
public partial class CS2Match
{
    private readonly HashSet<ulong> _rtv = new();
    private readonly HashSet<ulong> _rtg = new();
    private float _nextVoteAllowed;
    private bool _voteRunning;

    private void RegisterVoteCommands()
    {
        AddCommand("css_rtv", "Rock the vote (change map)", (p, i) => Rock(p, _rtv, "rtv"));
        AddCommand("css_unrtv", "Take back your !rtv", (p, i) => Unrock(p, _rtv, "rtv"));
        AddCommand("css_rtg", "Rock the game mode", (p, i) => Rock(p, _rtg, "rtg"));
        AddCommand("css_unrtg", "Take back your !rtg", (p, i) => Unrock(p, _rtg, "rtg"));
        AddCommandListener("say", OnSayPlain);
        AddCommandListener("say_team", OnSayPlain);
    }

    // Plain "rtv" / "rock the vote" / "rtg" / "rock the gamemode" in chat work too.
    private HookResult OnSayPlain(CCSPlayerController? player, CommandInfo info)
    {
        if (player == null) return HookResult.Continue;
        var text = info.ArgString.Trim().Trim('"').Trim().ToLowerInvariant();
        if (text is "rtv" or "rock the vote") Server.NextFrame(() => Rock(player, _rtv, "rtv"));
        else if (text is "rtg" or "rock the gamemode" or "rock the game mode") Server.NextFrame(() => Rock(player, _rtg, "rtg"));
        return HookResult.Continue;
    }

    private void ResetVotes()
    {
        _rtv.Clear();
        _rtg.Clear();
        _voteRunning = false;
        _nextVoteAllowed = Server.CurrentTime + 30f;
    }

    private void OnDisconnectVotes(int slot)
    {
        // Recount on the next frame, once the player is gone.
        Server.NextFrame(() =>
        {
            var present = RealPlayers().Select(p => p.SteamID).ToHashSet();
            _rtv.RemoveWhere(id => !present.Contains(id));
            _rtg.RemoveWhere(id => !present.Contains(id));
            CheckThreshold(_rtv, "rtv");
            CheckThreshold(_rtg, "rtg");
        });
    }

    private int Needed() => Math.Max(1, (int)Math.Ceiling(RealPlayers().Count() * Settings.RtvRatio));

    private bool VoteBlocked(CCSPlayerController? p)
    {
        if (!Settings.RtvEnabled) { Reply(p, "Map votes are turned off on this server."); return true; }
        if (LiveMatch() || DuelRunning && CurrentMode == Mode.Duels) { Reply(p, "Not during a live match."); return true; }
        if (_voteRunning || _modeSwitchPending) { Reply(p, "A vote or map change is already running."); return true; }
        var wait = _nextVoteAllowed - Server.CurrentTime;
        if (wait > 0) { Reply(p, $"Voting opens in {(int)wait + 1} s."); return true; }
        return false;
    }

    private void Rock(CCSPlayerController? p, HashSet<ulong> set, string what)
    {
        if (p == null || !p.IsValid || p.IsBot) return;
        if (VoteBlocked(p)) return;
        if (!set.Add(p.SteamID)) { Reply(p, $"You already typed {what}. {set.Count}/{Needed()}"); return; }
        Server.PrintToChatAll($"{Prefix}{p.PlayerName} wants to {(what == "rtv" ? "change the map" : "change the game mode")} ({set.Count}/{Needed()}). Type !{what}.");
        CheckThreshold(set, what);
    }

    private void Unrock(CCSPlayerController? p, HashSet<ulong> set, string what)
    {
        if (p == null) return;
        if (set.Remove(p.SteamID)) Reply(p, $"Removed your {what} ({set.Count}/{Needed()}).");
    }

    private void CheckThreshold(HashSet<ulong> set, string what)
    {
        if (_voteRunning || set.Count == 0 || set.Count < Needed()) return;
        _rtv.Clear();
        _rtg.Clear();
        if (what == "rtv") StartMapVote(CurrentMode, keepOption: true);
        else StartModeVote();
    }

    private void StartMapVote(Mode mode, bool keepOption)
    {
        var maps = MapsFor(mode).Where(m => !IsCurrentMap(m)).OrderBy(_ => Random.Shared.Next()).Take(6).ToList();
        var options = maps.Select(m => (label: m.Name, value: (object)m)).ToList();
        var current = FindMap(Server.MapName);
        if (keepOption)
            options.Add(("Stay on " + Server.MapName, "stay"));
        else if (current != null && current.Fits(mode))
            options.Add(("Keep " + Server.MapName, current));

        if (maps.Count == 0 && keepOption) { Server.PrintToChatAll($"{Prefix}No other maps for {mode.Title()}."); return; }

        RunVote($"Vote: next map ({mode.Title()})", options, winner =>
        {
            if (winner is MapEntry map)
            {
                if (mode != CurrentMode) SwitchMode(mode, map, null);
                else ChangeMapSameMode(map, null);
            }
            else
            {
                Server.PrintToChatAll($"{Prefix}Staying on {Server.MapName}.");
                _nextVoteAllowed = Server.CurrentTime + 180f;
            }
        });
    }

    private void StartModeVote()
    {
        var options = Modes.All.Where(m => m != CurrentMode && MapsFor(m).Count > 0)
            .Take(8).Select(m => (label: m.Title(), value: (object)m)).ToList();
        if (options.Count == 0) { Server.PrintToChatAll($"{Prefix}No other game mode has maps installed."); return; }
        options.Add(("Stay on " + CurrentMode.Title(), "stay"));

        RunVote("Vote: next game mode", options, winner =>
        {
            if (winner is Mode m)
            {
                Server.PrintToChatAll($"{Prefix}{m.Title()} won. Now pick the map.");
                AddTimer(2f, () => StartMapVote(m, keepOption: false), TimerFlags.STOP_ON_MAPCHANGE);
            }
            else
            {
                Server.PrintToChatAll($"{Prefix}Staying on {CurrentMode.Title()}.");
                _nextVoteAllowed = Server.CurrentTime + 180f;
            }
        });
    }

    private void RunVote(string title, List<(string label, object value)> options, Action<object> onDone)
    {
        _voteRunning = true;
        var votes = new Dictionary<ulong, int>();
        var menu = new ChatMenu(title);
        for (var i = 0; i < options.Count; i++)
        {
            var idx = i;
            menu.AddMenuOption(options[i].label, (p, _) =>
            {
                votes[p.SteamID] = idx;
                p.PrintToChat($"{Prefix}You voted for {ChatColors.Gold}{options[idx].label}");
            });
        }
        foreach (var p in RealPlayers()) MenuManager.OpenChatMenu(p, menu);
        Server.PrintToChatAll($"{Prefix}{title}: type the number in chat (e.g. !1). {Settings.VoteSeconds} s.");

        AddTimer(Settings.VoteSeconds, () =>
        {
            _voteRunning = false;
            foreach (var p in RealPlayers()) MenuManager.CloseActiveMenu(p);
            var counts = new int[options.Count];
            foreach (var v in votes.Values) counts[v]++;
            int best;
            if (votes.Count == 0)
            {
                best = Random.Shared.Next(options.Count);
                Server.PrintToChatAll($"{Prefix}Nobody voted, picked at random: {ChatColors.Gold}{options[best].label}");
            }
            else
            {
                var top = counts.Max();
                var tied = Enumerable.Range(0, counts.Length).Where(i => counts[i] == top).ToList();
                best = tied[Random.Shared.Next(tied.Count)];
                Server.PrintToChatAll($"{Prefix}Vote result: {ChatColors.Gold}{options[best].label}{ChatColors.Default} ({top} vote{(top == 1 ? "" : "s")}{(tied.Count > 1 ? ", tie broken at random" : "")})");
            }
            onDone(options[best].value);
        }, TimerFlags.STOP_ON_MAPCHANGE);
    }
}
