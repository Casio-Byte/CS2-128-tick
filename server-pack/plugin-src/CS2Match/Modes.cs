using System.Text.Json;
using System.Text.Json.Serialization;

namespace CS2Match;

public enum Mode
{
    Competitive,
    Wingman,
    Casual,
    Deathmatch,
    ArmsRace,
    Practice,
    Duels,
    Arenas,
}

public static class Modes
{
    public static readonly Mode[] All = Enum.GetValues<Mode>();

    public static string Key(this Mode m) => m.ToString().ToLowerInvariant();

    public static string Title(this Mode m) => m switch
    {
        Mode.Competitive => "Competitive 5v5",
        Mode.Wingman => "Wingman 2v2",
        Mode.Casual => "Casual",
        Mode.Deathmatch => "Deathmatch (FFA)",
        Mode.ArmsRace => "Arms Race",
        Mode.Practice => "Practice",
        Mode.Duels => "1v1 duels",
        Mode.Arenas => "Multi-1v1 arenas",
        _ => m.ToString(),
    };

    /// game_type / game_mode pair. These must be set before the map loads.
    public static (int type, int mode) GameTypeMode(this Mode m) => m switch
    {
        Mode.Casual => (0, 0),
        Mode.Competitive => (0, 1),
        Mode.Practice => (0, 1),
        Mode.Wingman => (0, 2),
        Mode.ArmsRace => (1, 0),
        Mode.Deathmatch => (1, 2),
        Mode.Duels => (0, 0),
        Mode.Arenas => (0, 0),
        _ => (0, 0),
    };

    public static bool TryParse(string s, out Mode mode)
    {
        s = s.Trim().ToLowerInvariant().Replace(" ", "").Replace("-", "").Replace("_", "");
        foreach (var m in All)
        {
            if (m.Key() == s) { mode = m; return true; }
        }
        var alias = s switch
        {
            "comp" or "5v5" or "mm" => (Mode?)Mode.Competitive,
            "2v2" or "wm" => Mode.Wingman,
            "dm" or "ffa" => Mode.Deathmatch,
            "ar" or "gungame" => Mode.ArmsRace,
            "prac" => Mode.Practice,
            "1v1" or "duel" or "aim" => Mode.Duels,
            "arena" or "multi1v1" => Mode.Arenas,
            _ => null,
        };
        mode = alias ?? Mode.Casual;
        return alias != null;
    }

    /// Which modes a map is made for, from its name prefix (same rules as the CS:GO pack).
    public static Mode[] ModesForMapName(string name)
    {
        var n = name.ToLowerInvariant();
        if (n.StartsWith("am_")) return [Mode.Arenas];
        if (n.StartsWith("aim_") || n.StartsWith("awp_") || n.StartsWith("1v1_")) return [Mode.Duels];
        if (n.StartsWith("ar_")) return [Mode.ArmsRace];
        if (n.StartsWith("kz_") || n.StartsWith("bhop_") || n.StartsWith("surf_")) return [];
        if (n.StartsWith("de_"))
            return [Mode.Competitive, Mode.Wingman, Mode.Casual, Mode.Deathmatch, Mode.Practice];
        return [Mode.Competitive, Mode.Wingman, Mode.Casual, Mode.Deathmatch, Mode.Practice];
    }
}

public class MapEntry
{
    [JsonPropertyName("name")] public string Name { get; set; } = "";
    /// Steam Workshop id. Empty for official maps.
    [JsonPropertyName("workshop_id")] public string WorkshopId { get; set; } = "";
    /// Optional explicit list of mode keys. Empty = decided from the name prefix.
    [JsonPropertyName("modes")] public List<string> ModeKeys { get; set; } = [];

    [JsonIgnore] public bool IsWorkshop => !string.IsNullOrWhiteSpace(WorkshopId);

    public IEnumerable<Mode> Modes()
    {
        if (ModeKeys.Count > 0)
        {
            foreach (var k in ModeKeys)
                if (global::CS2Match.Modes.TryParse(k, out var m)) yield return m;
            yield break;
        }
        foreach (var m in global::CS2Match.Modes.ModesForMapName(Name)) yield return m;
    }

    public bool Fits(Mode m) => Modes().Contains(m);
}

public class MapsFile
{
    [JsonPropertyName("maps")] public List<MapEntry> Maps { get; set; } = [];

    public static MapsFile Default() => new()
    {
        Maps =
        [
            new() { Name = "de_dust2" }, new() { Name = "de_mirage" }, new() { Name = "de_inferno" },
            new() { Name = "de_nuke" }, new() { Name = "de_ancient" }, new() { Name = "de_anubis" },
            new() { Name = "de_overpass" }, new() { Name = "de_train" }, new() { Name = "de_vertigo" },
            new() { Name = "cs_office" }, new() { Name = "cs_italy" },
            new() { Name = "ar_baggage" }, new() { Name = "ar_shoots" }, new() { Name = "ar_pool_day" },
        ],
    };
}

public class Settings
{
    [JsonPropertyName("admin_password")] public string AdminPassword { get; set; } = "";
    [JsonPropertyName("default_mode")] public string DefaultMode { get; set; } = "competitive";
    [JsonPropertyName("rtv_enabled")] public bool RtvEnabled { get; set; } = true;
    [JsonPropertyName("rtv_ratio")] public double RtvRatio { get; set; } = 0.6;
    [JsonPropertyName("vote_seconds")] public int VoteSeconds { get; set; } = 20;
    [JsonPropertyName("skip_team_menu")] public bool SkipTeamMenu { get; set; } = true;
    [JsonPropertyName("dm_kill_hp")] public int DmKillHp { get; set; } = 50;
    [JsonPropertyName("dm_weapons")] public List<string> DmWeapons { get; set; } = ["weapon_ak47", "weapon_deagle"];
}

public class State
{
    [JsonPropertyName("mode")] public string Mode { get; set; } = "";
    [JsonPropertyName("remembered_admins")] public List<ulong> RememberedAdmins { get; set; } = [];
}

public static class Json
{
    public static readonly JsonSerializerOptions Opts = new()
    {
        WriteIndented = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
    };

    public static T Load<T>(string path, Func<T> fallback, bool writeIfMissing = true)
    {
        try
        {
            if (File.Exists(path))
                return JsonSerializer.Deserialize<T>(File.ReadAllText(path), Opts) ?? fallback();
        }
        catch (Exception)
        {
            // A broken file is kept aside so a typo never wipes the user's settings.
            try { File.Copy(path, path + ".broken", true); } catch { }
            return fallback();
        }
        var value = fallback();
        if (writeIfMissing) Save(path, value);
        return value;
    }

    public static void Save<T>(string path, T value)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var tmp = path + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(value, Opts));
        File.Move(tmp, path, true);
    }
}
