# ===========================================================================
#  BEYOND: TWO SOULS - VR MOD : installer / launcher / uninstaller
#
#  Called by the two .cmd files in the B2S_VR_MOD folder:
#    "Play Beyond Two Souls in VR.cmd"  ->  -Mode Play       (install or update, then start the game)
#    "Uninstall VR mod.cmd"             ->  -Mode Uninstall  (put the game folder back as it was)
#  For checking without changing anything:  -Mode Check
#
#  The mod is a single file, dinput8.dll, placed next to the game's exe (BeyondTwoSouls_Steam.exe or
#  the exe of another store's copy). Windows loads it into the game whichever way the game is started
#  (Steam, the Epic Games Launcher or the exe). Nothing is injected, and no game file is replaced,
#  patched or edited on disk. Steam copies also get a steam_appid.txt (only when there is none) so the
#  exe can be started directly; the uninstaller removes it only if this script created it.
#
#  -NoLaunch    do everything except starting SteamVR and the game, and (uninstall) deleting this
#               folder (testing)
#  -GameDir     the game folder, instead of finding it
#  -Unattended  answer every question with its default instead of asking (testing)
#
#  Kept ASCII only: Windows PowerShell 5.1 reads a script without a BOM in the ANSI code page.
# ===========================================================================
param(
    [ValidateSet('Play', 'Uninstall', 'Check')][string]$Mode = 'Play',
    [switch]$NoLaunch,
    [switch]$Unattended,
    [string]$GameDir = ''
)

$ErrorActionPreference = 'Stop'
$ModDir = Split-Path -Parent $PSScriptRoot
$Files = Join-Path $ModDir 'files'
$StateGame = Join-Path $ModDir 'game_dir.txt'   # where the mod was installed (for the uninstaller)
# Everything this script prints also goes to setup_log.txt in the B2S_VR_MOD folder (the previous run:
# setup_log.prev.txt), with the Windows user name replaced by <user>: the file players send when the
# installer / launcher fails.
$SetupLog = Join-Path $ModDir 'setup_log.txt'

# The game's exe: the Steam name first, then any other store's (BeyondTwoSouls*.exe).
$ExeNames = @('BeyondTwoSouls_Steam.exe', 'BeyondTwoSouls.exe')
$ExeName = 'BeyondTwoSouls_Steam.exe'   # set to the found exe once the game folder is known
# The build the mod's game addresses were recorded on (PE header). Any other build works too: the
# mod finds its addresses by signature at startup and switches off only what it cannot find.
$KnownTimeStamp = 0x5eb531da
$KnownImageSize = 0x02120000
$AppIdMarker = 'b2svr_steam_appid.created'  # in the game folder: steam_appid.txt was written by us

# Files the mod writes into the game folder (removed on request by the uninstaller). The last six
# are written only by development builds of the mod.
$ModDataFiles = @('b2svr.ini', 'b2svr.log', 'b2svr.prev.log', 'b2svr_cmd.txt', 'b2svr_cmd.done',
                  'b2svr_camdump.bin', 'b2svr_vpdump.bin')

# Privacy: setup_log.txt never names the player. Folder paths become <game folder> / <mod folder> /
# <user profile>, the Windows user name (as a path part) <user>, the computer name <pc>. The screen
# still shows the real paths.
$script:ScrubPaths = New-Object System.Collections.ArrayList
function Add-Scrub([string]$path, [string]$with) {
    if ($path -and $path.Length -gt 3) { [void]$script:ScrubPaths.Add(@($path.TrimEnd('\', '/'), $with)) }
}
Add-Scrub $ModDir '<mod folder>'
Add-Scrub $env:USERPROFILE '<user profile>'
function Protect-Text([string]$t) {
    # longest paths first, so a folder inside another one is named by its own label
    foreach ($e in ($script:ScrubPaths | Sort-Object { $_[0].Length } -Descending)) {
        $t = $t -replace [regex]::Escape($e[0]), $e[1]
    }
    $u = $env:USERNAME
    if ($u -and $u.Length -gt 1) { $t = $t -replace ('(?i)(?<=[\\/])' + [regex]::Escape($u) + '(?=[\\/]|$)'), '<user>' }
    $c = $env:COMPUTERNAME
    if ($c -and $c.Length -gt 1) { $t = $t -replace [regex]::Escape($c), '<pc>' }
    return $t
}
function Write-SetupLog([string]$t) {
    try {
        $t = Protect-Text $t
        Add-Content -LiteralPath $SetupLog -Value ('[{0}] {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $t) -Encoding UTF8
    } catch { }
}
function Say([string]$t) { Write-Host "   $t"; Write-SetupLog $t }
function Warn([string]$t) { Write-Host "   [!] $t" -ForegroundColor Yellow; Write-SetupLog "[!] $t" }
function Bad([string]$t) { Write-Host "   [X] $t" -ForegroundColor Red; Write-SetupLog "[X] $t" }
function Ask([string]$prompt, [string]$default) {
    if ($Unattended) { Say "$prompt -> $default (unattended)"; return $default }
    return (Read-Host "   $prompt")
}
function Done([int]$code) {
    Write-Host ''
    if (-not $Unattended) { [void](Read-Host '   Press Enter to close') }
    exit $code
}
# A new setup log for this run (the previous one kept as setup_log.prev.txt).
try {
    if (Test-Path -LiteralPath $SetupLog) {
        Move-Item -LiteralPath $SetupLog -Destination (Join-Path $ModDir 'setup_log.prev.txt') -Force
    }
} catch { }
Write-SetupLog ("B2S VR mod setup, mode $Mode; PowerShell $($PSVersionTable.PSVersion); " +
                "$([Environment]::OSVersion.VersionString); 64-bit OS: $([Environment]::Is64BitOperatingSystem)")

# Any error this script did not expect: shown, written to setup_log.txt, and the window stays open.
trap {
    Bad ('Unexpected error: ' + $_.Exception.Message)
    Write-SetupLog ('  at line ' + $_.InvocationInfo.ScriptLineNumber + ': ' + $_.InvocationInfo.Line.Trim())
    Say 'Please send setup_log.txt (in this B2S_VR_MOD folder) with your bug report.'
    Done 1
}

function Banner([string]$title) {
    Write-Host ''
    Write-Host '  ================================================================'
    Write-Host "    $title"
    Write-Host '  ================================================================'
    Write-Host ''
}

# ---- the game folder ------------------------------------------------------------------------------
function Get-GameExe([string]$d) {
    if (-not $d -or -not (Test-Path -LiteralPath $d)) { return $null }
    foreach ($n in $ExeNames) { if (Test-Path -LiteralPath (Join-Path $d $n)) { return $n } }
    $e = Get-ChildItem -LiteralPath $d -Filter 'BeyondTwoSouls*.exe' -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($e) { return $e.Name }
    return $null
}
function Test-GameDir([string]$d) { return [bool](Get-GameExe $d) }
function Use-GameDir([string]$d) { $script:ExeName = Get-GameExe $d; return $d }

function Find-GameDir {
    if ($GameDir) {
        if (Test-GameDir $GameDir) { return (Resolve-Path -LiteralPath $GameDir).Path }
        Bad "-GameDir $GameDir does not contain a Beyond: Two Souls exe"
        return $null
    }
    # 1. this folder sits inside the game folder (the normal way, see README)
    $parent = Split-Path -Parent $ModDir
    if (Test-GameDir $parent) { return $parent }
    # 2. where the last install went
    if (Test-Path -LiteralPath $StateGame) {
        $d = (Get-Content -LiteralPath $StateGame -Raw).Trim()
        if (Test-GameDir $d) { return $d }
    }
    # 3. Steam libraries
    try {
        $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath
        if ($steam) {
            $libs = @($steam)
            $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
            if (Test-Path -LiteralPath $vdf) {
                foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) {
                    $libs += ($m.Groups[1].Value -replace '\\\\', '\')
                }
            }
            foreach ($l in ($libs | Select-Object -Unique)) {
                $common = Join-Path $l 'steamapps\common'
                if (-not (Test-Path -LiteralPath $common)) { continue }
                foreach ($g in Get-ChildItem -LiteralPath $common -Directory -ErrorAction SilentlyContinue) {
                    if (Test-GameDir $g.FullName) { return $g.FullName }
                }
            }
        }
    } catch { }
    # 4. Epic Games Launcher installs
    foreach ($m in (Get-EpicManifests)) { if (Test-GameDir $m.InstallLocation) { return $m.InstallLocation } }
    # 5. ask
    Warn 'Could not find Beyond: Two Souls by itself.'
    Say 'Easiest fix: move this whole B2S_VR_MOD folder INTO your Beyond: Two Souls folder'
    Say '(the one that contains BeyondTwoSouls_Steam.exe or BeyondTwoSouls.exe) and run this again.'
    Write-Host ''
    if ($Unattended) { return $null }
    $d = (Read-Host '   Or paste the Beyond: Two Souls folder here and press Enter').Trim().Trim('"')
    if (Test-GameDir $d) { return (Resolve-Path -LiteralPath $d).Path }
    return $null
}

function Test-GameRunning { return [bool](Get-Process -Name 'BeyondTwoSouls*' -ErrorAction SilentlyContinue) }

# ---- stores ---------------------------------------------------------------------------------------
function Get-EpicManifests {
    $dir = Join-Path $env:ProgramData 'Epic\EpicGamesLauncher\Data\Manifests'
    $out = @()
    foreach ($f in Get-ChildItem -LiteralPath $dir -Filter '*.item' -ErrorAction SilentlyContinue) {
        try {
            $j = Get-Content -LiteralPath $f.FullName -Raw | ConvertFrom-Json
            if ($j.InstallLocation -and $j.AppName) { $out += $j }
        } catch { }
    }
    return $out
}
function Get-EpicApp([string]$game) {
    foreach ($m in (Get-EpicManifests)) {
        try { if ((Resolve-Path -LiteralPath $m.InstallLocation).Path -eq $game) { return $m } } catch { }
    }
    return $null
}
# A Steam copy: <library>\steamapps\common\<dir> with an appmanifest whose installdir is <dir>.
# Returns its app id, or $null.
function Get-SteamAppId([string]$game) {
    $common = Split-Path -Parent $game
    if ((Split-Path -Leaf $common) -ne 'common') { return $null }
    $apps = Split-Path -Parent $common
    foreach ($f in Get-ChildItem -LiteralPath $apps -Filter 'appmanifest_*.acf' -ErrorAction SilentlyContinue) {
        $t = Get-Content -LiteralPath $f.FullName -Raw
        $inst = [regex]::Match($t, '"installdir"\s+"([^"]+)"').Groups[1].Value
        if ($inst -eq (Split-Path -Leaf $game)) {
            $id = [regex]::Match($t, '"appid"\s+"(\d+)"').Groups[1].Value
            if ($id) { return $id }
        }
    }
    return $null
}
function Get-Store([string]$game) {
    if (Get-SteamAppId $game) { return 'Steam' }
    if (Get-EpicApp $game) { return 'Epic Games' }
    return 'other'
}

# ---- dinput8.dll ----------------------------------------------------------------------------------
# Ours = a B2SVR build (its first log line "B2SVR <version> built ..." is in the file). Any other
# dinput8.dll belongs to another mod or tool (ASI loaders, overlays) and is never replaced or deleted.
function Test-OurDll([string]$path) {
    $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path))
    return $text.Contains('B2SVR ') -and $text.Contains(' built ')
}

function Read-ExeId([string]$exe) {
    $fs = [IO.File]::OpenRead($exe)
    try {
        $br = New-Object IO.BinaryReader($fs)
        $fs.Position = 0x3C; $pe = $br.ReadInt32()
        $fs.Position = $pe + 8; $ts = $br.ReadUInt32()
        $fs.Position = $pe + 24 + 56; $size = $br.ReadUInt32()
        return @{ TimeStamp = $ts; Size = $size }
    } finally { $fs.Close() }
}

function Test-KnownBuild([string]$game) {
    $id = Read-ExeId (Join-Path $game (Get-GameExe $game))
    return @{ Known = ($id.TimeStamp -eq $KnownTimeStamp -and $id.Size -eq $KnownImageSize); Id = $id }
}

# ---- b2svr.ini ------------------------------------------------------------------------------------
# One key of the [b2svr] section. Every other line is kept as it is; the file may mix CRLF and LF
# line ends, so lines are split on either and written back with CRLF.
function Get-IniValue([string]$path, [string]$key) {
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    foreach ($line in ([IO.File]::ReadAllText($path) -split "\r?\n")) {
        if ($line -match ('^\s*' + [regex]::Escape($key) + '\s*=\s*(.*?)\s*$')) { return $Matches[1] }
    }
    return $null
}
function Set-IniValue([string]$path, [string]$key, [string]$value) {
    $text = ''
    if (Test-Path -LiteralPath $path) { $text = [IO.File]::ReadAllText($path) }
    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($l in ($text -split "\r?\n")) { $lines.Add($l) }
    while ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }
    $section = -1
    for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^\s*\[b2svr\]\s*$') { $section = $i; break } }
    if ($section -lt 0) { $lines.Add('[b2svr]'); $section = $lines.Count - 1 }
    $end = $lines.Count   # the next section starts here
    for ($i = $section + 1; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^\s*\[') { $end = $i; break } }
    $set = $false
    for ($i = $section + 1; $i -lt $end; $i++) {
        if ($lines[$i] -match ('^\s*' + [regex]::Escape($key) + '\s*=')) { $lines[$i] = "$key=$value"; $set = $true; break }
    }
    if (-not $set) { $lines.Insert($end, "$key=$value") }
    [IO.File]::WriteAllText($path, (($lines -join "`r`n") + "`r`n"), (New-Object Text.UTF8Encoding $false))
}

# The VR settings asked on every Play (just Enter keeps the current choice).
$RenderSizes = @(1440, 1728, 2048, 2304, 2560, 2816)
$RenderNotes = @('fastest', '', 'recommended', '', '', 'sharpest')
function Select-VrSettings([string]$ini) {
    Write-Host ''
    Say 'VR picture size: a square picture for each eye, in pixels.'
    Say 'Bigger = sharper, but it needs a faster graphics card.'
    $current = Get-IniValue $ini 'render_size'
    $default = 3
    for ($i = 0; $i -lt $RenderSizes.Count; $i++) { if ("$($RenderSizes[$i])" -eq $current) { $default = $i + 1 } }
    for ($i = 0; $i -lt $RenderSizes.Count; $i++) {
        $note = ''
        if ($RenderNotes[$i]) { $note = '   (' + $RenderNotes[$i] + ')' }
        Write-Host ('     {0}. {1} x {1}{2}' -f ($i + 1), $RenderSizes[$i], $note)
    }
    $a = "$(Ask "Type 1-6 and press Enter (just Enter keeps $default)" "$default")".Trim()
    if (-not $a) { $a = "$default" }
    $n = 0
    if (-not [int]::TryParse($a, [ref]$n) -or $n -lt 1 -or $n -gt $RenderSizes.Count) {
        Warn "'$a' is not 1-6: keeping $default"
        $n = $default
    }
    $size = $RenderSizes[$n - 1]
    Set-IniValue $ini 'render_size' "$size"
    Say "VR picture size: $size x $size"

    Write-Host ''
    Say 'Turning with the right stick in first person (F1):'
    Write-Host '     1. Smooth turning'
    Write-Host '     2. Snap turning (turns in steps)'
    $default = 1
    if ((Get-IniValue $ini 'turn') -eq 'snap') { $default = 2 }
    $a = "$(Ask "Type 1 or 2 and press Enter (just Enter keeps $default)" "$default")".Trim()
    if (-not $a) { $a = "$default" }
    if ($a -ne '1' -and $a -ne '2') {
        Warn "'$a' is not 1 or 2: keeping $default"
        $a = "$default"
    }
    if ($a -eq '2') { $turn = 'snap' } else { $turn = 'smooth' }
    Set-IniValue $ini 'turn' $turn
    Say "Turning in first person: $turn"
}

# ---- starting things ------------------------------------------------------------------------------
function Start-Game([string]$game) {
    $id = Get-SteamAppId $game
    if ($id) {
        Say "Starting Beyond: Two Souls through Steam (app $id)"
        Start-Process "steam://rungameid/$id"
        return
    }
    $epic = Get-EpicApp $game
    if ($epic) {
        Say 'Starting Beyond: Two Souls through the Epic Games Launcher'
        Start-Process ('com.epicgames.launcher://apps/' + $epic.AppName + '?action=launch&silent=true')
        return
    }
    Say "Starting $ExeName"
    Start-Process -FilePath (Join-Path $game $ExeName) -WorkingDirectory $game
}

function Start-SteamVRIfNeeded {
    if (Get-Process -Name 'vrserver' -ErrorAction SilentlyContinue) { Say 'SteamVR is running'; return }
    if (Get-Process -Name 'VirtualDesktop.Streamer' -ErrorAction SilentlyContinue) { Say 'Virtual Desktop is running'; return }
    if (Test-Path -LiteralPath 'Registry::HKEY_CLASSES_ROOT\steam') {
        Say 'Starting SteamVR...'
        Start-Process 'steam://run/250820'
    } else {
        Warn 'SteamVR is not running - start it (or your OpenXR runtime) now.'
    }
}

# ---- status (all modes) ---------------------------------------------------------------------------
function Show-Status([string]$game) {
    $b = Test-KnownBuild $game
    Say ("Store: {0}" -f (Get-Store $game))
    if ($b.Known) { Say "${ExeName}: verified build" }
    else { Say ("{0}: build stamp 0x{1:x8} (not the verified build: game addresses are found by signature)" -f $ExeName, $b.Id.TimeStamp) }
    $dll = Join-Path $game 'dinput8.dll'
    if (-not (Test-Path -LiteralPath $dll)) { Say 'dinput8.dll: not installed' }
    elseif (-not (Test-OurDll $dll)) { Warn 'dinput8.dll: present, but it is NOT this mod (another mod or tool)' }
    elseif ((Test-Path -LiteralPath (Join-Path $Files 'dinput8.dll')) -and
            (Get-FileHash -LiteralPath $dll).Hash -eq (Get-FileHash -LiteralPath (Join-Path $Files 'dinput8.dll')).Hash) {
        Say 'dinput8.dll: this release is installed'
    } else { Say 'dinput8.dll: an older or newer build of this mod is installed' }
    $ini = Join-Path $game 'b2svr.ini'
    if (Test-Path -LiteralPath $ini) {
        Say ('b2svr.ini: present (VR picture size {0}, turning {1})' -f (Get-IniValue $ini 'render_size'), (Get-IniValue $ini 'turn'))
    } else { Say 'b2svr.ini: not created yet' }
    $reports = Join-Path $game 'b2svr_logs\crash_reports'
    if (Test-Path -LiteralPath $reports) {
        $n = @(Get-ChildItem -LiteralPath $reports -Filter 'crash_*.txt' -ErrorAction SilentlyContinue).Count
        if ($n -gt 0) { Say "crash reports: $n in b2svr_logs\crash_reports" }
    }
}

# ===================================================================================================
if ($Mode -eq 'Check') {
    Banner 'BEYOND: TWO SOULS VR MOD  -  CHECK (nothing is changed)'
    $game = Find-GameDir
    Add-Scrub $game '<game folder>'
    if (-not $game) { Bad 'No Beyond: Two Souls folder found.'; Done 1 }
    $game = Use-GameDir $game
    Say "Game folder: $game"
    if (Test-GameRunning) { Say 'The game is running' }
    Show-Status $game
    Done 0
}

# ===================================================================================================
if ($Mode -eq 'Play') {
    Banner 'BEYOND: TWO SOULS  -  VR MOD'
    $payload = Join-Path $Files 'dinput8.dll'
    if (-not (Test-Path -LiteralPath $payload)) { Bad 'files\dinput8.dll is missing - re-extract the mod.'; Done 1 }

    $game = Find-GameDir
    Add-Scrub $game '<game folder>'
    if (-not $game) { Bad 'No Beyond: Two Souls folder - nothing was changed.'; Done 1 }
    $game = Use-GameDir $game
    Say "Game folder: $game"
    if (Test-GameRunning) {
        Warn 'Beyond: Two Souls is running. Close it, then run this again (the mod is installed while the game is closed).'
        Done 1
    }

    # the game build
    $b = Test-KnownBuild $game
    Say ("Store: {0}   exe: {1}" -f (Get-Store $game), $ExeName)
    if ($b.Known) {
        Say "${ExeName}: verified build"
    } else {
        Say ('{0}: build stamp 0x{1:x8} - not the verified build. The mod finds its game addresses' -f $ExeName, $b.Id.TimeStamp)
        Say 'by signature when the game starts; anything it cannot find stays off, and if the core VR'
        Say 'view cannot be found the game simply runs normally on the monitor (see b2svr_logs\b2svr.log).'
    }

    # dinput8.dll
    $dst = Join-Path $game 'dinput8.dll'
    if (Test-Path -LiteralPath $dst) {
        if (-not (Test-OurDll $dst)) {
            Bad 'There is already a dinput8.dll in the game folder that is NOT this mod'
            Say '(another mod, an ASI loader or an overlay). It was left alone. Move it somewhere'
            Say 'else if you want this VR mod instead, then run this again.'
            Done 1
        }
        if ((Get-FileHash -LiteralPath $dst).Hash -eq (Get-FileHash -LiteralPath $payload).Hash) {
            Say 'dinput8.dll: installed and up to date'
        } else {
            Copy-Item -LiteralPath $payload -Destination $dst -Force
            Say 'dinput8.dll: updated to this release'
        }
    } else {
        Copy-Item -LiteralPath $payload -Destination $dst
        Say 'dinput8.dll: installed'
    }
    try { Unblock-File -LiteralPath $dst } catch { }
    Set-Content -LiteralPath $StateGame -Value $game -Encoding UTF8

    # steam_appid.txt (Steam copies): lets the exe start directly; never replaces an existing one
    $sid = Get-SteamAppId $game
    if ($sid) {
        $appid = Join-Path $game 'steam_appid.txt'
        if (-not (Test-Path -LiteralPath $appid)) {
            Set-Content -LiteralPath $appid -Value $sid -Encoding ASCII -NoNewline
            Set-Content -LiteralPath (Join-Path $game $AppIdMarker) -Value $sid -Encoding ASCII
            Say "steam_appid.txt: created (app $sid)"
        } else {
            Say 'steam_appid.txt: already present, left alone'
        }
    }

    # b2svr.ini - the player's settings are never overwritten
    $ini = Join-Path $game 'b2svr.ini'
    if (-not (Test-Path -LiteralPath $ini)) {
        Copy-Item -LiteralPath (Join-Path $Files 'b2svr.ini') -Destination $ini
        Say 'b2svr.ini: default VR settings created'
    } else {
        Say 'b2svr.ini: your VR settings kept'
    }
    Select-VrSettings $ini

    Write-Host ''
    Say 'In the game''s graphics settings: Windowed mode, Motion blur off, Depth of field off.'
    Write-Host ''
    Say 'In the game:'
    Say '  - VR starts by itself a few seconds after the game starts'
    Say '  - F9 = VR on / off      F2 = recenter the view'
    Say '  - F1 = first person while playing      F3 = first person in cutscenes'
    Say '  - more in GUIDE.md'
    Write-Host ''
    if ($NoLaunch) { Say 'Installed (-NoLaunch: SteamVR and the game were not started).'; exit 0 }
    Start-SteamVRIfNeeded
    Write-Host ''
    [void](Ask 'Put the headset on, then press Enter to start the game' '')
    Start-Game $game
    Say 'The mod stays installed: starting the game from Steam or its exe also runs in VR'
    Say '(set xr=off in b2svr.ini to start flat and switch with F9). Uninstall with "Uninstall VR mod.cmd".'
    Start-Sleep -Seconds 4
    exit 0
}

# ===================================================================================================
if ($Mode -eq 'Uninstall') {
    Banner 'BEYOND: TWO SOULS VR MOD  -  UNINSTALL'
    $game = Find-GameDir
    Add-Scrub $game '<game folder>'
    if (-not $game) { Bad 'No Beyond: Two Souls folder found - nothing was changed.'; Done 1 }
    $game = Use-GameDir $game
    Say "Game folder: $game"
    Write-Host ''
    Say 'This will:'
    Say '  1. remove the mod (dinput8.dll) from the game folder'
    Say '  2. optionally delete b2svr.ini (your VR settings) and the b2svr_logs folder'
    Say '  3. delete this whole folder:'
    Say "     $ModDir"
    Say 'Your saves and every game file are left alone.'
    Write-Host ''
    $a = Ask 'Type YES and press Enter to uninstall (anything else cancels)' 'YES'
    if ($a -ne 'YES' -and $a -ne 'yes') { Say 'Cancelled. Nothing was changed.'; Done 0 }
    if (Test-GameRunning) { Warn 'Beyond: Two Souls is running - close it first, then run this again.'; Done 1 }
    Write-Host ''

    $dll = Join-Path $game 'dinput8.dll'
    if (Test-Path -LiteralPath $dll) {
        if (Test-OurDll $dll) { Remove-Item -LiteralPath $dll -Force; Say 'Removed dinput8.dll (the mod)' }
        else { Warn 'dinput8.dll in the game folder is not this mod - left alone' }
    } else { Say 'dinput8.dll: not installed' }

    $marker = Join-Path $game $AppIdMarker
    if (Test-Path -LiteralPath $marker) {
        $appid = Join-Path $game 'steam_appid.txt'
        if (Test-Path -LiteralPath $appid) { Remove-Item -LiteralPath $appid -Force; Say 'Removed steam_appid.txt (created by this mod)' }
        Remove-Item -LiteralPath $marker -Force
    }

    $k = Ask 'Also delete b2svr.ini (your VR settings) and the b2svr_logs folder? Type Y to delete' 'Y'
    if ($k -eq 'Y' -or $k -eq 'y') {
        foreach ($f in $ModDataFiles) {
            $p = Join-Path $game $f
            if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force; Say "Removed $f" }
        }
        $logs = Join-Path $game 'b2svr_logs'
        if (Test-Path -LiteralPath $logs) { Remove-Item -LiteralPath $logs -Recurse -Force; Say 'Removed b2svr_logs' }
    } else {
        Say 'b2svr.ini and b2svr_logs kept'
    }

    Write-Host ''
    Say 'Done. Beyond: Two Souls will start normally on your monitor again.'
    if ($NoLaunch) { Say '(-NoLaunch: this folder is kept)'; exit 0 }
    Say 'This folder is deleted when you close this window.'
    [void](Ask 'Press Enter to finish' '')
    # The running scripts live in this folder: a detached shell deletes it once they have exited.
    $parent = Split-Path -Parent $ModDir
    Start-Process -FilePath 'cmd.exe' -ArgumentList ('/c ping -n 4 127.0.0.1 >nul & rd /s /q "' + $ModDir + '"') `
        -WorkingDirectory $parent -WindowStyle Hidden
    exit 0
}
