using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Commands;
using CounterStrikeSharp.API.Modules.Menu;
using CounterStrikeSharp.API.Modules.Timers;
using CounterStrikeSharp.API.Modules.Utils;

namespace CS2Match;

public class Duel
{
    public ulong A, B;
    public string GunLabel = "AK-47";
    public string[] Gun = ["weapon_ak47"];
    public bool RandomGun;
    public int Target = 10;
    public bool HsOnly;
    public int ScoreA, ScoreB;
    public bool Accepted;
    public float Expires;

    public bool Has(ulong id) => A == id || B == id;
    public ulong Other(ulong id) => A == id ? B : A;
}

/// !duel / !accept / !decline / !forfeit. In 1v1 duels mode a duel takes over the server
/// (real rounds, everyone else spectates, others queue); in arena mode duels get their own arena.
public partial class CS2Match
{
    internal static readonly (string label, string[] weapons)[] DuelGuns =
    [
        ("AK-47", ["weapon_ak47"]), ("M4A4", ["weapon_m4a1"]), ("M4A1-S", ["weapon_m4a1_silencer"]),
        ("AWP", ["weapon_awp"]), ("SSG 08", ["weapon_ssg08"]), ("Desert Eagle", ["weapon_deagle"]),
        ("USP-S", ["weapon_usp_silencer"]), ("Glock-18", ["weapon_glock"]), ("P250", ["weapon_p250"]),
        ("Random each round", []), ("Knives only", []),
    ];

    internal readonly List<Duel> Duels = new();
    internal Duel? ActiveDuel; // duels mode only
    internal bool DuelRunning => ActiveDuel != null;
    private readonly Dictionary<ulong, Duel> _draft = new();

    private static readonly string[] LobbyWeapons = ["weapon_ak47", "weapon_deagle"];

    private void RegisterDuelCommands()
    {
        AddCommand("css_duel", "Challenge a player: !duel [name]", CmdDuel);
        AddCommand("css_accept", "Accept a duel", (p, i) => RespondDuel(p, true));
        AddCommand("css_decline", "Decline a duel", (p, i) => RespondDuel(p, false));
        AddCommand("css_forfeit", "Give up your duel", CmdForfeit);
    }

    private void CmdDuel(CCSPlayerController? player, CommandInfo info)
    {
        if (player == null || !player.IsValid) return;
        if (CurrentMode is not (Mode.Duels or Mode.Arenas)) { Reply(player, "Duels only work in 1v1 duels or arenas (!1v1)."); return; }
        if (Duels.Any(d => d.Accepted && d.Has(player.SteamID))) { Reply(player, "You're already in a duel. !forfeit to give up."); return; }

        if (info.ArgCount >= 2)
        {
            var q = info.ArgString.Trim().Trim('"');
            var hits = RealPlayers().Where(p => p != player && p.PlayerName.Contains(q, StringComparison.OrdinalIgnoreCase)).ToList();
            if (hits.Count == 0) { Reply(player, $"Nobody called {q}."); return; }
            if (hits.Count > 1) { Reply(player, $"{hits.Count} players match {q}, be more exact."); return; }
            _draft[player.SteamID] = new Duel { A = player.SteamID, B = hits[0].SteamID };
            OpenDuelGunMenu(player);
            return;
        }
        OpenDuelOpponentMenu(player);
    }

    internal void OpenDuelOpponentMenu(CCSPlayerController player)
    {
        var menu = new ChatMenu("Duel 1/4: opponent");
        var any = false;
        foreach (var o in RealPlayers().Where(p => p != player))
        {
            var target = o;
            var busy = Duels.Any(d => d.Accepted && d.Has(o.SteamID));
            menu.AddMenuOption(busy ? $"{o.PlayerName} (in a duel)" : o.PlayerName, (p, _) =>
            {
                _draft[p.SteamID] = new Duel { A = p.SteamID, B = target.SteamID };
                OpenDuelGunMenu(p);
            }, busy);
            any = true;
        }
        if (!any) { Reply(player, "Nobody else is on the server."); return; }
        MenuManager.OpenChatMenu(player, menu);
    }

    private void OpenDuelGunMenu(CCSPlayerController player)
    {
        var menu = new ChatMenu("Duel 2/4: gun");
        foreach (var g in DuelGuns)
        {
            var gun = g;
            menu.AddMenuOption(gun.label, (p, _) =>
            {
                if (!_draft.TryGetValue(p.SteamID, out var d)) return;
                d.GunLabel = gun.label;
                d.RandomGun = gun.label.StartsWith("Random");
                d.Gun = d.RandomGun ? RandomDuelGun() : gun.weapons;
                OpenDuelRoundsMenu(p);
            });
        }
        MenuManager.OpenChatMenu(player, menu);
    }

    private static string[] RandomDuelGun()
    {
        var real = DuelGuns.Where(g => g.weapons.Length > 0).ToArray();
        return real[Random.Shared.Next(real.Length)].weapons;
    }

    private void OpenDuelRoundsMenu(CCSPlayerController player)
    {
        var menu = new ChatMenu("Duel 3/4: first to");
        foreach (var n in new[] { 5, 10, 16, 30 })
        {
            var target = n;
            menu.AddMenuOption($"{n} wins", (p, _) =>
            {
                if (!_draft.TryGetValue(p.SteamID, out var d)) return;
                d.Target = target;
                OpenDuelHsMenu(p);
            });
        }
        MenuManager.OpenChatMenu(player, menu);
    }

    private void OpenDuelHsMenu(CCSPlayerController player)
    {
        var menu = new ChatMenu("Duel 4/4: damage");
        menu.AddMenuOption("Normal", (p, _) => SendChallenge(p, false));
        menu.AddMenuOption("Headshots only", (p, _) => SendChallenge(p, true));
        MenuManager.OpenChatMenu(player, menu);
    }

    private void SendChallenge(CCSPlayerController p, bool hs)
    {
        if (!_draft.Remove(p.SteamID, out var d)) return;
        d.HsOnly = hs;
        var target = Utilities.GetPlayerFromSteamId(d.B);
        if (target == null || !target.IsValid) { Reply(p, "That player left."); return; }
        if (Duels.Any(x => x.Accepted && x.Has(d.B))) { Reply(p, $"{target.PlayerName} is already in a duel."); return; }

        Duels.RemoveAll(x => !x.Accepted && x.A == d.A); // one open challenge per player
        d.Expires = Server.CurrentTime + 30f;
        Duels.Add(d);
        var desc = $"{d.GunLabel}, first to {d.Target}{(d.HsOnly ? ", headshots only" : "")}";
        Reply(p, $"Challenge sent to {target.PlayerName}: {desc}.");
        target.PrintToChat($"{Prefix}{ChatColors.Gold}{p.PlayerName}{ChatColors.Default} challenges you to a duel ({desc}). {ChatColors.Lime}!accept{ChatColors.Default} or {ChatColors.Red}!decline{ChatColors.Default} (30 s).");

        AddTimer(30.5f, () =>
        {
            if (Duels.Contains(d) && !d.Accepted)
            {
                Duels.Remove(d);
                var a = Utilities.GetPlayerFromSteamId(d.A);
                if (a != null && a.IsValid) Reply(a, "Your duel challenge ran out.");
            }
        }, TimerFlags.STOP_ON_MAPCHANGE);
    }

    private void RespondDuel(CCSPlayerController? player, bool accept)
    {
        if (player == null) return;
        var d = Duels.LastOrDefault(x => !x.Accepted && x.B == player.SteamID && x.Expires > Server.CurrentTime);
        if (d == null) { Reply(player, "No duel challenge for you."); return; }
        var a = Utilities.GetPlayerFromSteamId(d.A);
        if (!accept)
        {
            Duels.Remove(d);
            Reply(player, "Declined.");
            if (a != null && a.IsValid) Reply(a, $"{player.PlayerName} declined your duel.");
            return;
        }
        if (a == null || !a.IsValid) { Duels.Remove(d); Reply(player, "The challenger left."); return; }
        if (Duels.Any(x => x.Accepted && (x.Has(d.A) || x.Has(d.B)))) { Reply(player, "One of you is already in a duel."); return; }

        d.Accepted = true;
        // Other open challenges involving either player are dropped.
        Duels.RemoveAll(x => !x.Accepted && (x.Has(d.A) || x.Has(d.B)));
        Server.PrintToChatAll($"{Prefix}Duel: {ChatColors.Gold}{a.PlayerName}{ChatColors.Default} vs {ChatColors.Gold}{player.PlayerName}{ChatColors.Default} ({d.GunLabel}, first to {d.Target}{(d.HsOnly ? ", HS only" : "")}).");

        if (CurrentMode == Mode.Duels)
        {
            if (ActiveDuel == null) BeginDuelsModeDuel(d);
            else Server.PrintToChatAll($"{Prefix}A duel is running, theirs starts next.");
        }
        else Server.PrintToChatAll($"{Prefix}They get their own arena from the next round.");
    }

    private void CmdForfeit(CCSPlayerController? player, CommandInfo info)
    {
        if (player == null) return;
        var d = Duels.FirstOrDefault(x => x.Accepted && x.Has(player.SteamID));
        if (d == null) { Reply(player, "You're not in a duel."); return; }
        Server.PrintToChatAll($"{Prefix}{player.PlayerName} forfeits.");
        FinishDuel(d, d.Other(player.SteamID));
    }

    internal void FinishDuel(Duel d, ulong winner)
    {
        Duels.Remove(d);
        var w = Utilities.GetPlayerFromSteamId(winner);
        var l = Utilities.GetPlayerFromSteamId(d.Other(winner));
        var (sw, sl) = winner == d.A ? (d.ScoreA, d.ScoreB) : (d.ScoreB, d.ScoreA);
        Server.PrintToChatAll($"{Prefix}{ChatColors.Lime}{w?.PlayerName ?? "?"}{ChatColors.Default} wins the duel against {l?.PlayerName ?? "?"} ({sw}-{sl}).");

        if (CurrentMode == Mode.Duels && ActiveDuel == d)
        {
            ActiveDuel = null;
            AddTimer(3f, () =>
            {
                var next = Duels.FirstOrDefault(x => x.Accepted);
                if (next != null) BeginDuelsModeDuel(next);
                else StartDuelLobby();
            }, TimerFlags.STOP_ON_MAPCHANGE);
        }
    }

    // ---------------------------------------------------------------- duels mode (one duel at a time)

    private void StartDuelLobby()
    {
        ActiveDuel = null;
        Server.ExecuteCommand("exec cs2pack/mode_duels.cfg");
        foreach (var p in RealPlayers().Where(p => p.Team == CsTeam.Spectator).ToList())
            p.ChangeTeam(SmallerTeam());
        Server.ExecuteCommand("mp_restartgame 1");
    }

    private void BeginDuelsModeDuel(Duel d)
    {
        var a = Utilities.GetPlayerFromSteamId(d.A);
        var b = Utilities.GetPlayerFromSteamId(d.B);
        if (a == null || b == null || !a.IsValid || !b.IsValid) { Duels.Remove(d); StartDuelLobby(); return; }

        ActiveDuel = d;
        d.ScoreA = d.ScoreB = 0;
        Server.ExecuteCommand(
            "mp_respawn_on_death_t 0; mp_respawn_on_death_ct 0; mp_teammates_are_enemies 0; " +
            "mp_ignore_round_win_conditions 0; mp_freezetime 2; mp_round_restart_delay 2; mp_roundtime 2; " +
            "mp_roundtime_defuse 2; mp_roundtime_hostage 2; mp_maxrounds 999; mp_halftime 0; mp_buytime 0; " +
            $"mp_damage_headshot_only {(d.HsOnly ? 1 : 0)}");

        foreach (var p in Utilities.GetPlayers().Where(p => p.IsValid && !p.IsHLTV))
        {
            if (p.SteamID == d.A) p.ChangeTeam(CsTeam.Terrorist);
            else if (p.SteamID == d.B) p.ChangeTeam(CsTeam.CounterTerrorist);
            else if (p.IsBot) Server.ExecuteCommand($"bot_kick \"{p.PlayerName}\"");
            else p.ChangeTeam(CsTeam.Spectator);
        }
        Server.ExecuteCommand("bot_quota 0");
        Server.ExecuteCommand("mp_restartgame 1");
    }

    private void DuelsOnSpawn(CCSPlayerController p)
    {
        AddTimer(0.15f, () =>
        {
            if (!p.IsValid) return;
            var d = ActiveDuel;
            if (d == null) GiveLoadout(p, LobbyWeapons);
            else if (d.Has(p.SteamID)) GiveLoadout(p, d.Gun);
        }, TimerFlags.STOP_ON_MAPCHANGE);
    }

    private void DuelsOnRoundStart()
    {
        var d = ActiveDuel;
        if (d == null) return;
        AddTimer(0.3f, () =>
        {
            foreach (var id in new[] { d.A, d.B })
            {
                var p = Utilities.GetPlayerFromSteamId(id);
                if (p != null && p.IsValid) GiveLoadout(p, d.Gun);
            }
        }, TimerFlags.STOP_ON_MAPCHANGE);
    }

    private void DuelsOnRoundEnd(EventRoundEnd ev)
    {
        var d = ActiveDuel;
        if (d == null) return;
        var a = Utilities.GetPlayerFromSteamId(d.A);
        var b = Utilities.GetPlayerFromSteamId(d.B);
        if (a == null || b == null) return;

        var winTeam = (CsTeam)ev.Winner;
        if (winTeam == a.Team) d.ScoreA++;
        else if (winTeam == b.Team) d.ScoreB++;
        else return;

        Server.PrintToChatAll($"{Prefix}{a.PlayerName} {ChatColors.Gold}{d.ScoreA}{ChatColors.Default} - {ChatColors.Gold}{d.ScoreB}{ChatColors.Default} {b.PlayerName} (first to {d.Target})");
        if (d.RandomGun) d.Gun = RandomDuelGun();
        if (d.ScoreA >= d.Target) FinishDuel(d, d.A);
        else if (d.ScoreB >= d.Target) FinishDuel(d, d.B);
    }

    private void ReplyDuelStatus(CCSPlayerController? p)
    {
        var d = ActiveDuel;
        if (d == null) { Reply(p, "No duel running. !duel to challenge someone."); return; }
        var a = Utilities.GetPlayerFromSteamId(d.A);
        var b = Utilities.GetPlayerFromSteamId(d.B);
        Reply(p, $"Duel: {a?.PlayerName} {d.ScoreA} - {d.ScoreB} {b?.PlayerName} (first to {d.Target}, {d.GunLabel})");
        var queued = Duels.Count(x => x.Accepted && x != d);
        if (queued > 0) Reply(p, $"{queued} more duel(s) waiting.");
    }

    private void OnDisconnectDuels(int slot)
    {
        var leaving = Utilities.GetPlayerFromSlot(slot);
        if (leaving == null) return;
        var id = leaving.SteamID;
        Duels.RemoveAll(x => !x.Accepted && x.Has(id));
        var d = Duels.FirstOrDefault(x => x.Accepted && x.Has(id));
        if (d != null)
        {
            Server.PrintToChatAll($"{Prefix}{leaving.PlayerName} left the duel.");
            FinishDuel(d, d.Other(id));
        }
    }

    private void ResetDuelsOnMapChange()
    {
        ActiveDuel = null;
        Duels.Clear();
        _draft.Clear();
    }
}
