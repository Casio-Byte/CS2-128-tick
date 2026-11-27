# cs2server.ps1 - helper for CS2 Server Launcher (plugins, modes, metamod, checks).
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File cs2server.ps1 <action> <serverRoot> [arg]
#   install-plugins <root>        download + install Metamod, CSS, MatchZy, Retakes, InstaDefuse
#   set-mode <root> <mode>        competitive|wingman|practice|retakes  (moves plugins in/out of disabled)
#   metamod <root> on|off         add/remove the Metamod line in gameinfo.gi
#   check <root>                  CS2 build vs latest CounterStrikeSharp / plugin versions
#   steamcmd <dir>                download SteamCMD into <dir> (the chosen SteamCMD folder)
#   events <root> <minutes>       Windows error events about cs2.exe
#   resolve <in> <out>            Steam profiles -> SteamID64 + name
#   firewall                      (elevated) remove Block rules for cs2.exe, allow UDP/TCP 27015 + cs2.exe
param([string]$Action, [string]$Root, [string]$Arg)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
function Say($m) { [Console]::Out.WriteLine([string]$m); [Console]::Out.Flush() }   # not the pipeline: functions return values

# Pinned versions (tags). CounterStrikeSharp and Metamod: latest.
$Pins = @{
  MatchZy     = @{ repo = 'shobhit-pathak/MatchZy';  tag = '0.9.3'; include = '*.zip'; exclude = '*cssharp*'; dll = 'MatchZy.dll' }
  Retakes     = @{ repo = 'B3none/cs2-retakes';      tag = '3.1.2'; include = 'RetakesPlugin*.zip'; exclude = '*no-map-configs*'; dll = 'RetakesPlugin.dll' }
  InstaDefuse = @{ repo = 'B3none/cs2-instadefuse';  tag = '2.0.0'; include = '*.zip'; exclude = '*source*'; dll = '*nstadefuse*.dll' }
}
$Hdr = @{ 'User-Agent' = 'cs2-server-launcher'; 'Accept' = 'application/vnd.github+json' }

function Csgo($r) { Join-Path $r 'game\csgo' }
function CssDir($r) { Join-Path (Csgo $r) 'addons\counterstrikesharp' }
function StateFile($r) { Join-Path $r 'cs2launcher_components.txt' }

function Get-Release($repo, $tag) {
  $u = if ($tag) { "https://api.github.com/repos/$repo/releases/tags/$tag" } else { "https://api.github.com/repos/$repo/releases/latest" }
  try { return Invoke-RestMethod -Uri $u -Headers $Hdr }
  catch {
    if ($tag) {   # tags are sometimes prefixed with v
      try { return Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/releases/tags/v$tag" -Headers $Hdr } catch {}
    }
    throw "GitHub: cannot get release $repo $tag ($($_.Exception.Message))"
  }
}

function Pick-Asset($rel, $include, $exclude) {
  $a = $rel.assets | Where-Object { $_.name -like $include -and -not ($exclude -and $_.name -like $exclude) } | Select-Object -First 1
  if (-not $a) { throw "No asset matching $include in $($rel.tag_name): $(( $rel.assets | ForEach-Object name) -join ', ')" }
  return $a
}

# Short work folder: CounterStrikeSharp's bundled .NET has very deep paths, and Windows
# PowerShell 5.1 cannot handle paths over 260 characters (the user's TEMP is often too long).
function Get-WorkRoot {
  foreach ($r in @((Join-Path $env:SystemDrive 'cs2l'), (Join-Path $env:TEMP 'cs2l'))) {
    try { New-Item -ItemType Directory -Force -Path $r -ErrorAction Stop | Out-Null; return $r } catch {}
  }
  throw "cannot create a work folder"
}
function Download($url, $name) {
  $tmp = Join-Path (Get-WorkRoot) ([guid]::NewGuid().ToString('N').Substring(0, 6))
  New-Item -ItemType Directory -Path $tmp | Out-Null
  $ext = [IO.Path]::GetExtension($name); if ($name -like '*.tar.gz') { $ext = '.tar.gz' }
  $f = Join-Path $tmp ('d' + $ext)
  Say "  downloading $name"
  Invoke-WebRequest -Uri $url -OutFile $f -Headers @{ 'User-Agent' = 'cs2-server-launcher' } -UseBasicParsing
  return $f
}

function Expand-Any($file) {
  # Expand-Archive (PS 5.1) fails on some zips; use .NET ZipFile, then Windows tar.exe as fallback
  $dst = Join-Path (Split-Path $file) 'x'
  New-Item -ItemType Directory -Force -Path $dst | Out-Null
  $ok = $false
  if ($file -like '*.zip') {
    try {
      Add-Type -AssemblyName System.IO.Compression.FileSystem
      $z = [IO.Compression.ZipFile]::OpenRead($file)
      try {
        foreach ($e in $z.Entries) {
          $rel = $e.FullName -replace '\\', '/'
          if ($rel.StartsWith('/') -or $rel -match '(^|/)\.\.(/|$)') { continue }
          $out = Join-Path $dst ($rel -replace '/', '\')
          if ($rel.EndsWith('/')) { New-Item -ItemType Directory -Force -Path $out | Out-Null; continue }
          New-Item -ItemType Directory -Force -Path (Split-Path $out) | Out-Null
          [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $out, $true)
        }
      } finally { $z.Dispose() }
      $ok = $true
    } catch { Say "    zip extract failed ($($_.Exception.Message)), trying tar" }
  }
  if (-not $ok) {
    $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
    & $tar -xf $file -C $dst | Out-Null
    if ($LASTEXITCODE) { throw "could not unpack $file" }
  }
  return $dst
}

function Merge($src, $dst) {
  New-Item -ItemType Directory -Force -Path $dst | Out-Null
  Copy-Item -Path (Join-Path $src '*') -Destination $dst -Recurse -Force
}

# Install a CounterStrikeSharp plugin zip whatever its layout; returns the plugin folder name.
function Install-PluginZip($root, $file, $dllPattern) {
  $x = Expand-Any $file
  $css = CssDir $root
  $addons = Get-ChildItem -Path $x -Directory -Recurse -Depth 2 | Where-Object { $_.Name -eq 'addons' } | Select-Object -First 1
  if ($addons) {
    Merge $addons.FullName (Join-Path (Csgo $root) 'addons')
    $cfg = Join-Path $addons.Parent.FullName 'cfg'      # e.g. MatchZy ships cfg\MatchZy next to addons
    if (Test-Path $cfg) { Merge $cfg (Join-Path (Csgo $root) 'cfg') }
  }
  else {
    $plugins = Get-ChildItem -Path $x -Directory -Recurse -Depth 2 | Where-Object { $_.Name -eq 'plugins' } | Select-Object -First 1
    if ($plugins) {
      # the zip is the inside of addons\counterstrikesharp (plugins, shared, configs ...)
      Merge $plugins.Parent.FullName $css
      $cfg = Join-Path $plugins.Parent.FullName 'cfg'
      if (Test-Path $cfg) { Merge $cfg (Join-Path (Csgo $root) 'cfg'); Remove-Item (Join-Path $css 'cfg') -Recurse -Force -ErrorAction SilentlyContinue }
    }
    else {
      $dll = Get-ChildItem -Path $x -Recurse -Filter $dllPattern | Select-Object -First 1
      if (-not $dll) { throw "no $dllPattern in $file" }
      $dir = $dll.Directory
      if ($dir.FullName -eq (Get-Item $x).FullName) {
        $name = [IO.Path]::GetFileNameWithoutExtension($dll.Name)
        Merge $x (Join-Path $css "plugins\$name")
      } else { Merge $dir.FullName (Join-Path $css "plugins\$($dir.Name)") }
    }
  }
  $found = Get-ChildItem -Path (Join-Path $css 'plugins') -Recurse -Filter $dllPattern -ErrorAction SilentlyContinue |
           Where-Object { $_.Directory.Parent.Name -eq 'plugins' } | Select-Object -First 1
  if (-not $found) { throw "installed, but $dllPattern not found under plugins" }
  return $found.Directory.Name
}

function Save-State($root, $h) {
  ($h.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" }) | Set-Content -Path (StateFile $root) -Encoding ASCII
}
function Load-State($root) {
  $h = @{}
  if (Test-Path (StateFile $root)) { Get-Content (StateFile $root) | ForEach-Object { $k, $v = $_ -split '=', 2; if ($k) { $h[$k] = $v } } }
  return $h
}

function Write-NoBom($path, $lines) {
  [IO.File]::WriteAllLines($path, [string[]]$lines, (New-Object System.Text.UTF8Encoding($false)))
}

function Set-Metamod($root, $on) {
  $gi = Join-Path (Csgo $root) 'gameinfo.gi'
  if (-not (Test-Path $gi)) { throw "gameinfo.gi not found - install the server first" }
  $lines = [IO.File]::ReadAllLines($gi)
  $has = $lines | Where-Object { $_ -match '^\s*Game\s+csgo/addons/metamod\s*$' }
  if ($on -and -not $has) {
    $out = New-Object System.Collections.Generic.List[string]; $done = $false
    foreach ($l in $lines) {
      $out.Add($l)
      if (-not $done -and $l -match '^\s*Game_LowViolence\s+csgo_lv') { $out.Add("`t`t`tGame`tcsgo/addons/metamod"); $done = $true }
    }
    if (-not $done) { throw "gameinfo.gi: Game_LowViolence line not found; add 'Game csgo/addons/metamod' by hand" }
    Write-NoBom $gi $out
    Say "Metamod enabled in gameinfo.gi"
  } elseif (-not $on -and $has) {
    Write-NoBom $gi ($lines | Where-Object { $_ -notmatch '^\s*Game\s+csgo/addons/metamod\s*$' })
    Say "Metamod disabled in gameinfo.gi (running without plugins)"
  } else { Say ("Metamod already " + ($(if ($on) { 'enabled' } else { 'disabled' }))) }
}

function Install-Plugins($root) {
  if (-not (Test-Path (Join-Path $root 'game\bin\win64\cs2.exe'))) { throw "No CS2 server in $root - click Install / Update first" }
  $state = Load-State $root
  # leftovers from older launcher versions / failed attempts
  Get-ChildItem -Path $env:TEMP -Directory -Filter 'cs2l_*' -ErrorAction SilentlyContinue | ForEach-Object { cmd /c rd /s /q "\\?\$($_.FullName)" 2>$null }
  Remove-Item (Join-Path $env:SystemDrive 'cs2l') -Recurse -Force -ErrorAction SilentlyContinue
  # Metamod (latest 2.0 dev build)
  Say "Metamod:Source"
  $mmRaw = (Invoke-WebRequest -Uri 'https://mms.alliedmods.net/mmsdrop/2.0/mmsource-latest-windows' -UseBasicParsing -Headers @{ 'User-Agent' = 'cs2-server-launcher' }).Content
  if ($mmRaw -is [byte[]]) { $mmRaw = [Text.Encoding]::ASCII.GetString($mmRaw) }   # served as octet-stream
  $mmName = ([string]$mmRaw).Trim()
  if ($mmName -notmatch '^mmsource-[\w.\-]+-windows\.zip$') { throw "unexpected Metamod file name '$mmName'" }
  $f = Download "https://mms.alliedmods.net/mmsdrop/2.0/$mmName" $mmName
  $x = Expand-Any $f
  Merge (Join-Path $x 'addons') (Join-Path (Csgo $root) 'addons')
  $state['metamod'] = $mmName
  Say "  installed $mmName"
  Set-Metamod $root $true
  # CounterStrikeSharp (latest, with runtime)
  Say "CounterStrikeSharp"
  $rel = Get-Release 'roflmuffin/CounterStrikeSharp' $null
  $a = Pick-Asset $rel '*with-runtime*windows*.zip' $null
  $f = Download $a.browser_download_url $a.name
  $x = Expand-Any $f
  $ad = Get-ChildItem -Path $x -Directory -Recurse -Depth 1 | Where-Object Name -eq 'addons' | Select-Object -First 1
  Merge $ad.FullName (Join-Path (Csgo $root) 'addons')
  $state['counterstrikesharp'] = $rel.tag_name
  Say "  installed $($rel.tag_name) ($($rel.published_at))"
  New-Item -ItemType Directory -Force -Path (Join-Path (CssDir $root) 'plugins\disabled') | Out-Null
  foreach ($k in 'MatchZy', 'Retakes', 'InstaDefuse') {
    $p = $Pins[$k]
    Say "$k $($p.tag)"
    # if it is currently disabled, bring it back first so the update lands in one place
    $cur = $state["$k.folder"]
    if ($cur) {
      $dis = Join-Path (CssDir $root) "plugins\disabled\$cur"
      if (Test-Path $dis) { Move-Item $dis (Join-Path (CssDir $root) "plugins\$cur") -Force }
    }
    $rel = Get-Release $p.repo $p.tag
    $a = Pick-Asset $rel $p.include $p.exclude
    $f = Download $a.browser_download_url $a.name
    $folder = Install-PluginZip $root $f $p.dll
    $state["$k.folder"] = $folder
    $state[$k] = $rel.tag_name
    Say "  installed $($rel.tag_name) -> plugins\$folder"
  }
  if ($state['Retakes.folder']) {
    $mc = Get-ChildItem -Path (Join-Path (CssDir $root) "plugins\$($state['Retakes.folder'])") -Recurse -Filter '*.json' -ErrorAction SilentlyContinue |
          Where-Object { $_.BaseName -match '^(de|cs)_' } | ForEach-Object BaseName
    if ($mc) { Say "  Retakes map spawns: $($mc -join ', ')" } else { Say "  WARNING: no Retakes map spawn configs found" }
  }
  Save-State $root $state
  Remove-Item (Get-WorkRoot) -Recurse -Force -ErrorAction SilentlyContinue
  Say "PLUGINS_OK"
}

function Set-Mode($root, $mode) {
  $state = Load-State $root
  $css = CssDir $root
  if (-not (Test-Path $css)) { Say "Plugins not installed; mode $mode uses stock CS2 only"; return }
  $want = switch ($mode) {
    'retakes' { @('Retakes', 'InstaDefuse') }
    default   { @('MatchZy') }
  }
  $dis = Join-Path $css 'plugins\disabled'; New-Item -ItemType Directory -Force -Path $dis | Out-Null
  foreach ($k in 'MatchZy', 'Retakes', 'InstaDefuse') {
    $folder = $state["$k.folder"]; if (-not $folder) { continue }
    $on = Join-Path $css "plugins\$folder"; $off = Join-Path $dis $folder
    if ($want -contains $k) {
      if (Test-Path $off) { if (Test-Path $on) { Remove-Item $on -Recurse -Force }; Move-Item $off $on }
      if (Test-Path $on) { Say "  $k enabled" } else { Say "  WARNING: $k not installed" }
    } else {
      if (Test-Path $on) { if (Test-Path $off) { Remove-Item $off -Recurse -Force }; Move-Item $on $off }
      Say "  $k disabled"
    }
  }
  Say "MODE_OK $mode"
}

function Check($root) {
  $state = Load-State $root
  $inf = Join-Path (Csgo $root) 'steam.inf'
  $cs2date = $null
  if (Test-Path $inf) {
    $c = Get-Content $inf
    $pv = ($c | Where-Object { $_ -like 'PatchVersion=*' }) -replace 'PatchVersion=', ''
    $vd = ($c | Where-Object { $_ -like 'VersionDate=*' }) -replace 'VersionDate=', ''
    $cs2date = [datetime]::ParseExact(($vd.Trim() -replace '\s+', ' '), 'MMM d yyyy', [Globalization.CultureInfo]::InvariantCulture)
    Say "CS2 server build: $pv ($vd)"
  } else { Say "CS2 server not installed" }
  try {
    $rel = Get-Release 'roflmuffin/CounterStrikeSharp' $null
    $pub = [datetime]$rel.published_at
    Say "Latest CounterStrikeSharp: $($rel.tag_name) ($($pub.ToString('yyyy-MM-dd'))); installed: $($state['counterstrikesharp'])"
    if ($cs2date -and $cs2date.Date -gt $pub.Date) {
      Say "WARNING: CS2 ($($cs2date.ToString('yyyy-MM-dd'))) is newer than the latest CounterStrikeSharp. No newer CounterStrikeSharp is out yet."
      Say "         MatchZy, Retakes and InstaDefuse all need it. If the server crashes on start, tick 'Run without plugins'."
    } elseif ($state['counterstrikesharp'] -and $state['counterstrikesharp'] -ne $rel.tag_name) {
      Say "UPDATE: CounterStrikeSharp $($rel.tag_name) is available - click Install plugins."
    } else { Say "CounterStrikeSharp is up to date." }
  } catch { Say "Could not check CounterStrikeSharp: $($_.Exception.Message)" }
  foreach ($k in 'MatchZy', 'Retakes', 'InstaDefuse') {
    $p = $Pins[$k]
    try {
      $l = Get-Release $p.repo $null
      $note = if ($l.tag_name -replace '^v', '' -ne $p.tag) { "  (newer release $($l.tag_name) exists; launcher pins $($p.tag))" } else { '' }
      Say "${k}: installed $($state[$k]); latest $($l.tag_name)$note"
    } catch { Say "${k}: could not check ($($_.Exception.Message))" }
  }
}

function Firewall() {
  $exe = Get-NetFirewallApplicationFilter -ErrorAction SilentlyContinue | Where-Object { $_.Program -like '*\cs2.exe' }
  $n = 0
  foreach ($f in $exe) {
    $r = $f | Get-NetFirewallRule
    if ($r.Action -eq 'Block') { Say "Removing block rule: $($r.DisplayName) ($($f.Program))"; $r | Remove-NetFirewallRule; $n++ }
  }
  Say "Removed $n block rule(s)"
  foreach ($proto in 'UDP', 'TCP') {
    $name = "CS2 Server $proto 27015"
    if (-not (Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue)) {
      New-NetFirewallRule -DisplayName $name -Direction Inbound -Action Allow -Protocol $proto -LocalPort 27015 | Out-Null
      Say "Added allow rule: $name"
    } else { Say "Allow rule present: $name" }
  }
  if ($Root -and (Test-Path (Join-Path $Root 'game\bin\win64\cs2.exe'))) {
    $p = Join-Path $Root 'game\bin\win64\cs2.exe'
    if (-not (Get-NetFirewallRule -DisplayName 'CS2 Server (cs2.exe)' -ErrorAction SilentlyContinue)) {
      New-NetFirewallRule -DisplayName 'CS2 Server (cs2.exe)' -Direction Inbound -Action Allow -Program $p | Out-Null
      Say "Added allow rule for $p"
    }
  }
  Say "FIREWALL_OK"
}

# Steam profile -> SteamID64 + name. Accepts profile links, SteamID64, STEAM_0:x:y, [U:1:n], custom URL names.
function Resolve-Steam($t) {
  $first = ($t.Trim() -split '\s+')[0]
  $id = $null; $u = $null
  if ($first -match '^7656\d{13}$') { $id = $first }
  elseif ($first -match 'steamcommunity\.com/profiles/(7656\d{13})') { $id = $Matches[1] }
  elseif ($first -match '^STEAM_[0-5]:([01]):(\d+)$') { $id = [string]([int64]76561197960265728 + [int64]$Matches[2] * 2 + [int64]$Matches[1]) }
  elseif ($first -match '^\[?U:1:(\d+)\]?$') { $id = [string]([int64]76561197960265728 + [int64]$Matches[1]) }
  else {
    $v = if ($first -match 'steamcommunity\.com/id/([^/?#]+)') { $Matches[1] } else { $first }
    $u = "https://steamcommunity.com/id/$v/?xml=1"
  }
  if ($id) { $u = "https://steamcommunity.com/profiles/$id/?xml=1" }
  try {
    $c = (Invoke-WebRequest -Uri $u -UseBasicParsing -Headers @{ 'User-Agent' = 'cs2-server-launcher' }).Content
    if ($c -is [byte[]]) { $c = [Text.Encoding]::UTF8.GetString($c) }
    $x = [xml]$c
  } catch { if ($id) { return @{ id = $id; name = $id } }; throw "cannot reach Steam ($($_.Exception.Message))" }
  if ($x.response -and $x.response.error) { if ($id) { return @{ id = $id; name = $id } }; throw ([string]$x.response.error).Trim() }
  $sid = [string]$x.profile.steamID64
  $name = [string]$x.profile.steamID
  if ($x.profile.steamID.'#cdata-section') { $name = [string]$x.profile.steamID.'#cdata-section' }
  if (-not $sid) { if ($id) { return @{ id = $id; name = $id } }; throw "profile not found" }
  if (-not $name) { $name = $sid }
  return @{ id = $sid.Trim(); name = $name.Trim() }
}

try {
  switch ($Action) {
    'install-plugins' { Install-Plugins $Root }
    'set-mode'        { Set-Mode $Root $Arg }
    'metamod'         { Set-Metamod $Root ($Arg -eq 'on') }
    'check'           { Check $Root }
    'firewall'        { Firewall; Start-Sleep 4 }
    'resolve'         {
      # Root = input file (team<TAB>text per line), Arg = output file
      $out = New-Object System.Collections.Generic.List[string]
      foreach ($line in [IO.File]::ReadAllLines($Root)) {
        $team, $txt = $line -split "`t", 2
        if (-not $txt -or -not $txt.Trim()) { continue }
        try { $r = Resolve-Steam $txt; $out.Add("$team`t$($r.id)`t$($r.name -replace "`t", ' ')"); Say "  $($txt.Trim()) -> $($r.id) ($($r.name))" }
        catch { $out.Add("$team`tERR`t$($txt.Trim())`t$($_.Exception.Message)"); Say "  $($txt.Trim()) -> FAILED: $($_.Exception.Message)" }
      }
      [IO.File]::WriteAllLines($Arg, [string[]]$out, (New-Object System.Text.UTF8Encoding($false)))
    }
    'events'          {
      # Windows Application log entries about cs2.exe since <Arg> minutes ago
      $m = [int]$Arg; if ($m -le 0) { $m = 5 }
      $prov = 'Application Error', 'Windows Error Reporting', 'Application Hang', 'SideBySide', '.NET Runtime'
      $ev = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = (Get-Date).AddMinutes(-$m) } -ErrorAction SilentlyContinue |
            Where-Object { $prov -contains $_.ProviderName -and ($_.ProviderName -eq 'SideBySide' -or $_.Message -match 'cs2\.exe|engine2|server\.dll|tier0|metamod|counterstrikesharp') } |
            Select-Object -First 6
      if (-not $ev) { Say "No Windows error events for cs2.exe in the last $m min." }
      foreach ($e in $ev) {
        $txt = (($e.Message -split "`r?`n") | Where-Object { $_.Trim() } | Select-Object -First 10) -join ' | '
        Say "[$($e.TimeCreated.ToString('HH:mm:ss'))] $($e.ProviderName) (id $($e.Id)): $txt"
      }
    }
    'steamcmd'        {
      $d = $Root; New-Item -ItemType Directory -Force -Path $d | Out-Null   # Root = SteamCMD folder here
      if (-not (Test-Path (Join-Path $d 'steamcmd.exe'))) {
        $f = Download 'https://steamcdn-a.akamaihd.net/client/installer/steamcmd.zip' 'steamcmd.zip'
        $x = Expand-Any $f; Merge $x $d
        Say "SteamCMD downloaded"
      } else { Say "SteamCMD present" }
    }
    default           { throw "unknown action $Action" }
  }
  exit 0
} catch {
  Say "ERROR: $($_.Exception.Message)"
  if ($Action -eq 'firewall') { Start-Sleep 8 }
  exit 1
}
