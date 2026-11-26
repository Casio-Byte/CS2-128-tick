using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Commands;
using CounterStrikeSharp.API.Modules.Entities.Constants;
using CounterStrikeSharp.API.Modules.Menu;
using CounterStrikeSharp.API.Modules.Timers;
using CounterStrikeSharp.API.Modules.Utils;

namespace CS2Match;

/// Multi-1v1 arenas on am_ maps: every arena runs its own 1v1 each round, players are
/// shuffled into new pairs every round, !duel pairs keep their own arena.
public partial class CS2Match
{
    private record Spot(Vector Pos, QAngle Ang);
    private record Arena(Spot T, Spot CT);

    private sealed class Pairing
    {
        public int Arena;
        public int T, CT;          // player slots (bots all have SteamID 0)
        public bool Done;
        public int Winner = -1;
        public Duel? Duel;
    }

    private readonly List<Arena> _arenas = new();
    private readonly List<Pairing> _pairs = new();
    private readonly HashSet<ulong> _sitOut = new();
    private readonly HashSet<int> _waitedLastRound = new();
    private readonly Dictionary<ulong, (int w, int l)> _rank = new();
    private readonly Dictionary<ulong, string> _arenaGun = new();
    private bool _arenaRoundEnding;

    private static readonly (string label, string weapon)[] ArenaGuns =
    [
        ("AK-47", "weapon_ak47"), ("M4A4", "weapon_m4a1"), ("M4A1-S", "weapon_m4a1_silencer"),
        ("AWP", "weapon_awp"), ("SSG 08", "weapon_ssg08"), ("Desert Eagle only", ""), ("Random", "random"),
    ];

    private void RegisterArenaCommands()
    {
        AddCommand("css_guns", "Arenas: pick your gun", (p, i) =>
        {
            if (p == null) return;
            if (CurrentMode != Mode.Arenas) return;
            var menu = new ChatMenu("Arena gun");
            foreach (var g in ArenaGuns)
            {
                var gun = g;
                menu.AddMenuOption(gun.label, (pl, _) =>
                {
                    _arenaGun[pl.SteamID] = gun.weapon;
                    Reply(pl, $"Arena gun: {gun.label} (from next round).");
                });
            }
            MenuManager.OpenChatMenu(p, menu);
        });
        AddCommand("css_sitout", "Arenas: sit out / play again", (p, i) =>
        {
            if (p == null || CurrentMode != Mode.Arenas) return;
            if (_sitOut.Remove(p.SteamID)) Reply(p, "You're back in from next round.");
            else { _sitOut.Add(p.SteamID); Reply(p, "You'll sit out from next round. !sitout again to play."); }
        });
        AddCommand("css_rank", "Arenas: your wins and losses", (p, i) =>
        {
            if (p == null || CurrentMode != Mode.Arenas) return;
            var (w, l) = _rank.GetValueOrDefault(p.SteamID);
            var place = _rank.OrderByDescending(kv => kv.Value.w).ThenBy(kv => kv.Value.l)
                .Select((kv, i) => (kv.Key, i)).FirstOrDefault(x => x.Key == p.SteamID).i + 1;
            Reply(p, $"Arena record: {w} wins, {l} losses (#{place} of {Math.Max(1, _rank.Count)}).");
        });
    }

    private void ResetArenasOnMapChange()
    {
        _arenas.Clear();
        _pairs.Clear();
        _waitedLastRound.Clear();
        _arenaRoundEnding = false;
    }

    private static List<Spot> Spots(string cls) =>
        Utilities.FindAllEntitiesByDesignerName<SpawnPoint>(cls)
            .Where(s => s.IsValid && s.AbsOrigin != null)
            .Select(s => new Spot(new Vector(s.AbsOrigin!.X, s.AbsOrigin.Y, s.AbsOrigin.Z),
                new QAngle(s.AbsRotation?.X ?? 0, s.AbsRotation?.Y ?? 0, s.AbsRotation?.Z ?? 0)))
            .ToList();

    private static float Dist(Vector a, Vector b)
    {
        float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
        return MathF.Sqrt(dx * dx + dy * dy + dz * dz);
    }

    /// Pairs each T spawn with its nearest free CT spawn (closest pairs first). On am_ maps
    /// every arena is its own room, so this gives one T/CT spot per arena.
    private void BuildArenas()
    {
        _arenas.Clear();
        var t = Spots("info_player_terrorist");
        var ct = Spots("info_player_counterterrorist");
        var candidates = new List<(float d, int ti, int ci)>();
        for (var i = 0; i < t.Count; i++)
            for (var j = 0; j < ct.Count; j++)
                candidates.Add((Dist(t[i].Pos, ct[j].Pos), i, j));
        var usedT = new HashSet<int>();
        var usedCt = new HashSet<int>();
        foreach (var (_, ti, ci) in candidates.OrderBy(c => c.d))
        {
            if (usedT.Contains(ti) || usedCt.Contains(ci)) continue;
            usedT.Add(ti);
            usedCt.Add(ci);
            _arenas.Add(new Arena(t[ti], ct[ci]));
        }
        Logger_Info($"Arenas: {_arenas.Count} arenas found on {Server.MapName}.");
        if (_arenas.Count == 0)
            Server.PrintToChatAll($"{Prefix}This map has no T/CT spawns to make arenas from.");

        // First round: everyone needs to be paired before spawning.
        PlanArenaRound();
        Server.ExecuteCommand("mp_restartgame 1");
    }

    /// Called at round end for the next round: decides pairs and puts players on the right team.
    private void PlanArenaRound()
    {
        _pairs.Clear();
        if (_arenas.Count == 0) return;

        var pool = Utilities.GetPlayers()
            .Where(p => p.IsValid && !p.IsHLTV && p.Connected == PlayerConnectedState.Connected)
            .Where(p => p.IsBot
                ? p.Team is CsTeam.Terrorist or CsTeam.CounterTerrorist || _waitedLastRound.Contains(p.Slot)
                : !_sitOut.Contains(p.SteamID) && p.Team != CsTeam.None)
            .ToList();

        var arenaIdx = 0;
        // Duels first: they keep their pairing.
        foreach (var d in Duels.Where(x => x.Accepted).ToList())
        {
            var a = pool.FirstOrDefault(p => !p.IsBot && p.SteamID == d.A);
            var b = pool.FirstOrDefault(p => !p.IsBot && p.SteamID == d.B);
            if (a == null || b == null || arenaIdx >= _arenas.Count) continue;
            pool.Remove(a); pool.Remove(b);
            _pairs.Add(new Pairing { Arena = arenaIdx++, T = a.Slot, CT = b.Slot, Duel = d });
        }

        // Everyone else is shuffled; whoever sat out last round goes first.
        var order = pool.OrderByDescending(p => _waitedLastRound.Contains(p.Slot)).ThenBy(_ => Random.Shared.Next()).ToList();
        _waitedLastRound.Clear();
        var i = 0;
        for (; i + 1 < order.Count && arenaIdx < _arenas.Count; i += 2)
            _pairs.Add(new Pairing { Arena = arenaIdx++, T = order[i].Slot, CT = order[i + 1].Slot });

        var waiting = order.Skip(i).ToList();
        foreach (var p in waiting) _waitedLastRound.Add(p.Slot);

        foreach (var pr in _pairs)
        {
            MoveTo(pr.T, CsTeam.Terrorist);
            MoveTo(pr.CT, CsTeam.CounterTerrorist);
        }
        foreach (var p in waiting)
        {
            if (p.Team != CsTeam.Spectator) p.ChangeTeam(CsTeam.Spectator);
            if (!p.IsBot) Reply(p, "No opponent this round, you're up first next round.");
        }
        foreach (var id in _sitOut)
        {
            var p = Utilities.GetPlayerFromSteamId(id);
            if (p != null && p.IsValid && p.Team != CsTeam.Spectator) p.ChangeTeam(CsTeam.Spectator);
        }
    }

    private static void MoveTo(int slot, CsTeam team)
    {
        var p = Utilities.GetPlayerFromSlot(slot);
        if (p == null || !p.IsValid || p.Team == team) return;
        if (p.Team == CsTeam.Spectator || p.Team == CsTeam.None) p.ChangeTeam(team);
        else p.SwitchTeam(team);
    }

    private static CCSPlayerController? BySlot(int slot)
    {
        var p = Utilities.GetPlayerFromSlot(slot);
        return p != null && p.IsValid ? p : null;
    }

    private int _arenaRoundId;

    private void ArenasOnRoundStart()
    {
        _arenaRoundEnding = false;
        var roundId = ++_arenaRoundId;
        // Nobody can hide forever: the round ends after 2 minutes (arena = draw).
        AddTimer(120f, () =>
        {
            if (roundId != _arenaRoundId || _arenaRoundEnding) return;
            _arenaRoundEnding = true;
            Server.PrintToChatAll($"{Prefix}Time's up.");
            Rules()?.TerminateRound(3f, RoundEndReason.RoundDraw);
        }, TimerFlags.STOP_ON_MAPCHANGE);
        if (_arenas.Count == 0) return;
        foreach (var pr in _pairs)
        {
            var arena = _arenas[pr.Arena];
            foreach (var (id, spot) in new[] { (pr.T, arena.T), (pr.CT, arena.CT) })
            {
                var p = BySlot(id);
                if (p == null || !p.IsValid || !p.PawnIsAlive) continue;
                p.PlayerPawn.Value?.Teleport(spot.Pos, spot.Ang, new Vector(0, 0, 0));
                if (pr.Duel != null) GiveLoadout(p, pr.Duel.Gun);
                else GiveLoadout(p, ArenaWeapons(p));
            }
            var tp = BySlot(pr.T);
            var ctp = BySlot(pr.CT);
            if (tp != null && ctp != null)
            {
                var label = pr.Duel != null ? "Duel" : "Arena";
                tp.PrintToCenter($"{label} {pr.Arena + 1}: vs {ctp.PlayerName}");
                ctp.PrintToCenter($"{label} {pr.Arena + 1}: vs {tp.PlayerName}");
            }
        }
        // Anyone alive who isn't in a pair this round (joined late) waits.
        var paired = _pairs.SelectMany(p => new[] { p.T, p.CT }).ToHashSet();
        foreach (var p in Playing().Where(p => p.PawnIsAlive && !paired.Contains(p.Slot)))
            p.CommitSuicide(false, true);
        CheckArenasDone();
    }

    private string[] ArenaWeapons(CCSPlayerController p)
    {
        var choice = _arenaGun.GetValueOrDefault(p.SteamID, "weapon_ak47");
        if (choice == "random")
        {
            var real = ArenaGuns.Where(g => g.weapon.StartsWith("weapon_")).ToArray();
            choice = real[Random.Shared.Next(real.Length)].weapon;
        }
        return choice.Length == 0 ? ["weapon_deagle"] : [choice, "weapon_deagle"];
    }

    private void ArenasOnDeath(EventPlayerDeath ev)
    {
        var victim = ev.Userid;
        if (victim == null) return;
        var pr = _pairs.FirstOrDefault(x => !x.Done && (x.T == victim.Slot || x.CT == victim.Slot));
        if (pr == null) return;
        pr.Done = true;
        pr.Winner = pr.T == victim.Slot ? pr.CT : pr.T;
        Record(BySlot(pr.Winner), true);
        Record(victim, false);
        AddTimer(0.1f, CheckArenasDone, TimerFlags.STOP_ON_MAPCHANGE);
    }

    private void Record(CCSPlayerController? p, bool win)
    {
        if (p == null || p.IsBot) return;
        var (w, l) = _rank.GetValueOrDefault(p.SteamID);
        _rank[p.SteamID] = win ? (w + 1, l) : (w, l + 1);
    }

    private void CheckArenasDone()
    {
        if (_arenaRoundEnding || _pairs.Count == 0) return;
        foreach (var pr in _pairs.Where(x => !x.Done))
        {
            var t = BySlot(pr.T);
            var c = BySlot(pr.CT);
            var tAlive = t != null && t.IsValid && t.PawnIsAlive;
            var cAlive = c != null && c.IsValid && c.PawnIsAlive;
            if (tAlive && cAlive) continue;
            pr.Done = true;
            pr.Winner = tAlive ? pr.T : cAlive ? pr.CT : -1;
        }
        if (_pairs.Any(x => !x.Done)) return;

        _arenaRoundEnding = true;
        Rules()?.TerminateRound(3f, RoundEndReason.RoundDraw);
    }

    private void ArenasOnRoundEnd()
    {
        // Duel scores move on by arena result.
        foreach (var pr in _pairs.Where(x => x.Duel != null && x.Done && x.Winner >= 0))
        {
            var d = pr.Duel!;
            if (!Duels.Contains(d)) continue;
            var winner = BySlot(pr.Winner);
            if (winner == null) continue;
            if (winner.SteamID == d.A) d.ScoreA++; else d.ScoreB++;
            var a = Utilities.GetPlayerFromSteamId(d.A);
            var b = Utilities.GetPlayerFromSteamId(d.B);
            Server.PrintToChatAll($"{Prefix}Duel: {a?.PlayerName} {d.ScoreA} - {d.ScoreB} {b?.PlayerName} (first to {d.Target})");
            if (d.RandomGun) d.Gun = RandomDuelGun();
            if (d.ScoreA >= d.Target) FinishDuel(d, d.A);
            else if (d.ScoreB >= d.Target) FinishDuel(d, d.B);
        }
        // Teams for the next round have to be set before it spawns.
        AddTimer(1.0f, PlanArenaRound, TimerFlags.STOP_ON_MAPCHANGE);
    }

    private void OnDisconnectArenas(int slot)
    {
        if (CurrentMode == Mode.Arenas) AddTimer(0.2f, CheckArenasDone, TimerFlags.STOP_ON_MAPCHANGE);
    }
}
