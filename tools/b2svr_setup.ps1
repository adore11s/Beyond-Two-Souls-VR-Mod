# ===========================================================================
#  BEYOND: TWO SOULS - VR MOD : installer / uninstaller
#
#  Called by the two .cmd files in the B2S_VR_MOD folder:
#    "Install VR mod.cmd"    ->  -Mode Install    (once: dinput8.dll next to the game's exe)
#    "Uninstall VR mod.cmd"  ->  -Mode Uninstall  (the mod's own files to the Recycle Bin)
#
#  The mod is a single file, dinput8.dll, next to the game's exe (BeyondTwoSouls_Steam.exe or the exe of another
#  store's copy). Windows loads it into the game whichever way the game is started (Steam, Epic, the exe) - nothing
#  is injected and no game file is replaced or patched on disk. The mod writes its own b2svr.ini and b2svr_logs
#  folder next to the exe.
#
#  The uninstaller NEVER deletes anything for good: everything it removes goes to the Recycle Bin, and only from a
#  fixed list of the mod's own files - never an .exe, never a folder holding one, never the game folder itself or
#  anything outside it.
#
#  As little hassle as possible (VR porting playbook 28.6): what blocks the run (the game running, a missing file, another
#  mod's dinput8.dll) is checked before any question; a choice takes ONE key (no Enter); only the uninstall itself is
#  typed (YES + Enter); the game options set are said in one line; a clean run closes by itself after 10 s (any key
#  closes it at once), and the window waits for a key only when there is something to read (a [!] / [X] line).
#
#  Kept ASCII only: Windows PowerShell 5.1 reads a script without a BOM in the ANSI code page.
# ===========================================================================
param([ValidateSet('Install', 'Uninstall')][string]$Mode = 'Install')

$ErrorActionPreference = 'Stop'
$ModDir = Split-Path -Parent $PSScriptRoot
$Files = Join-Path $ModDir 'files'
$StateGame = Join-Path $ModDir 'game_dir.txt'           # where the mod was installed (for the uninstaller)
$StateSettings = Join-Path $ModDir 'settings_changed.txt' # game options the installer changed: KEY=original|set
$OptionsBackup = Join-Path $ModDir 'game_options_backup.bin'  # the game's options file before the first change
# An older launcher created steam_appid.txt (marked by this file): the uninstaller removes it.
$AppIdMarker = 'b2svr_steam_appid.created'

$script:NeedRead = $false  # a warning or an error was printed: the window waits for a key before closing
function Say([string]$t) { Write-Host "   $t" }
function Warn([string]$t) { Write-Host "   [!] $t" -ForegroundColor Yellow; $script:NeedRead = $true }
function Bad([string]$t) { Write-Host "   [X] $t" -ForegroundColor Red; $script:NeedRead = $true }
function Test-Console {
    try { return -not [Console]::IsInputRedirected } catch { return $false }
}
# One key from a list (no Enter); the key taken is shown after the question. Without a console: a typed answer.
function Read-Key([string]$question, [string[]]$keys) {
    if (-not (Test-Console)) {
        while ($true) {
            $a = (Read-Host "   $question").Trim()
            foreach ($k in $keys) { if ($a -ieq $k) { return $k.ToUpper() } }
        }
    }
    Write-Host "   $question " -NoNewline
    while ($true) {
        $c = [string][Console]::ReadKey($true).KeyChar
        foreach ($k in $keys) {
            if ($c -ieq $k) { Write-Host $k.ToUpper(); return $k.ToUpper() }
        }
    }
}
# A clean run closes by itself (10 s, any key at once); with something to read it waits for any key.
function Done([int]$code) {
    Write-Host ''
    if (-not (Test-Console)) { exit $code }
    try { while ([Console]::KeyAvailable) { [void][Console]::ReadKey($true) } } catch { }
    if ($script:NeedRead -or $code -ne 0) {
        Write-Host '   Press any key to close' -NoNewline
        [void][Console]::ReadKey($true)
        Write-Host ''
        exit $code
    }
    for ($t = 10; $t -gt 0; $t--) {
        Write-Host "`r   Closing in $t s - press any key to close now " -NoNewline
        for ($i = 0; $i -lt 10; $i++) {
            if ([Console]::KeyAvailable) { [void][Console]::ReadKey($true); Write-Host ''; exit $code }
            Start-Sleep -Milliseconds 100
        }
    }
    Write-Host ''
    exit $code
}

function Banner([string]$title) {
    Write-Host ''
    Write-Host '  ================================================================'
    Write-Host "    $title"
    Write-Host '  ================================================================'
    Write-Host ''
}

# ---- the game folder ---------------------------------------------------------------------------------------------
# The game's exe: the Steam name, another store's name, or any BeyondTwoSouls*.exe.
function Get-GameExe([string]$d) {
    if (-not $d -or -not (Test-Path -LiteralPath $d -PathType Container)) { return $null }
    foreach ($n in @('BeyondTwoSouls_Steam.exe', 'BeyondTwoSouls.exe')) {
        if (Test-Path -LiteralPath (Join-Path $d $n)) { return $n }
    }
    $e = Get-ChildItem -LiteralPath $d -Filter 'BeyondTwoSouls*.exe' -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($e) { return $e.Name }
    return $null
}
function Test-GameDir([string]$d) { return [bool](Get-GameExe $d) }

function Find-GameDir {
    # 0. the mod's files were copied straight into the game folder: that folder (never another install)
    if (Test-GameDir $ModDir) { return (Resolve-Path -LiteralPath $ModDir).Path }
    # 1. this B2S_VR_MOD folder sits inside the game folder (the normal way, see README)
    $parent = Split-Path -Parent $ModDir
    if (Test-GameDir $parent) { return (Resolve-Path -LiteralPath $parent).Path }
    # 2. where the last install went
    if (Test-Path -LiteralPath $StateGame) {
        $d = (Get-Content -LiteralPath $StateGame -Raw).Trim()
        if (Test-GameDir $d) { return (Resolve-Path -LiteralPath $d).Path }
    }
    # 3. Steam libraries
    $cands = @()
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
                $cands += (Join-Path $common 'Beyond Two Souls')
                foreach ($g in Get-ChildItem -LiteralPath $common -Directory -ErrorAction SilentlyContinue) { $cands += $g.FullName }
            }
        }
    } catch { }
    # 4. Epic Games Launcher installs
    $man = Join-Path $env:ProgramData 'Epic\EpicGamesLauncher\Data\Manifests'
    if (Test-Path -LiteralPath $man) {
        foreach ($f in Get-ChildItem -LiteralPath $man -Filter '*.item' -ErrorAction SilentlyContinue) {
            try { $j = Get-Content -LiteralPath $f.FullName -Raw | ConvertFrom-Json } catch { continue }
            if ($j.InstallLocation) { $cands += $j.InstallLocation }
        }
    }
    foreach ($c in $cands) { if (Test-GameDir $c) { return (Resolve-Path -LiteralPath $c).Path } }
    # 5. ask
    Warn 'Could not find Beyond: Two Souls by itself.'
    Say 'Easiest fix: put this whole B2S_VR_MOD folder INTO your Beyond: Two Souls folder'
    Say '(the one that contains BeyondTwoSouls_Steam.exe) and run this again.'
    Write-Host ''
    $d = (Read-Host '   Or paste the Beyond: Two Souls folder here and press Enter').Trim().Trim('"')
    if (Test-GameDir $d) { return (Resolve-Path -LiteralPath $d).Path }
    return $null
}

function Test-GameRunning { return [bool](Get-Process -Name 'BeyondTwoSouls*' -ErrorAction SilentlyContinue) }

# ---- dinput8.dll -------------------------------------------------------------------------------------------------
# Ours = a B2SVR build (its log line "B2SVR <version> built ..." is in the file). Anything else named dinput8.dll
# belongs to another mod / tool (ASI loaders, overlays) and is never overwritten or removed.
function Test-OurDll([string]$path) {
    $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path))
    return $text.Contains('B2SVR ') -and $text.Contains(' built ')
}

# ---- the Recycle Bin, with every check ---------------------------------------------------------------------------
Add-Type -AssemblyName Microsoft.VisualBasic

# Sends `path` to the Recycle Bin - never deletes for good. Refuses (and says so): anything outside the game folder,
# the game folder itself, an .exe, a folder that holds an .exe anywhere inside it.
function Send-ToRecycleBin([string]$path, [string]$game) {
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    $full = [IO.Path]::GetFullPath($path).TrimEnd('\')
    $g = [IO.Path]::GetFullPath($game).TrimEnd('\')
    if ($full -ieq $g -or -not $full.StartsWith($g + '\', [StringComparison]::OrdinalIgnoreCase)) {
        Warn "Not removed (not a file inside the game folder): $full"
        return $false
    }
    if (Test-Path -LiteralPath $full -PathType Leaf) {
        if ([IO.Path]::GetExtension($full) -ieq '.exe') { Warn "Not removed (a program file): $full"; return $false }
        [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile($full, 'OnlyErrorDialogs', 'SendToRecycleBin')
    } else {
        $exe = Get-ChildItem -LiteralPath $full -Recurse -Force -File -Filter '*.exe' -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($exe) { Warn "Not removed (it holds a program file, $($exe.Name)): $full"; return $false }
        [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($full, 'OnlyErrorDialogs', 'SendToRecycleBin')
    }
    return $true
}

# ---- the game's options file -------------------------------------------------------------------------------------
# Beyond keeps its options in a binary file, not a settings.ini (decompiled, analysed build):
#   path      %USERPROFILE%\Saved Games\Beyond Two Souls Steam[ Demo]\Save\Options\0  (FUN_140bbd9f0)
#   header    CRC-32 (zlib) of everything after it, then the fields in the order the loader reads them
#             (FUN_140d77290, version 0x23): every value is little-endian, strings are a length then the bytes.
#   DisplayMode   int  RenderState +0xCAC: 0 windowed, 1 borderless, 2 fullscreen (FUN_1408291b0, Alt+Enter 1407acb30)
#   Quality1..6   ints the Custom preset's choices (globals 141c8b6c8 6bc 6b8 6c4 6c0 6b0)
#   MotionBlur    int  the Custom preset's motion blur (141b9ad94; SetMotionBlur 140d5d7c0)
#   DepthOfField  int  the Custom preset's depth of field (141b9adbc; SetDOF 140d5d580)
#   Preset        int  0 Low, 1 Medium, 2 High, 3 Custom (141b9addc; FUN_140d5ea30 applies it)
# Motion blur and depth of field are only kept with the Custom preset; Low / Medium / High set their own. So the
# installer switches a Low / Medium / High preset to Custom with exactly that preset's choices (FUN_140d5ea30's
# table), only motion blur and depth of field off.
$PresetChoices = @{  # Quality1..6 for presets 0 / 1 / 2 (FUN_140d5ea30)
    0 = @(1, 0, 1, 1, 0, 0); 1 = @(2, 1, 1, 2, 1, 2); 2 = @(2, 2, 2, 2, 2, 3)
}
$CrcTable = $null
function Get-Crc32([byte[]]$data, [int]$start) {
    # (PowerShell 5.1 reads 0xEDB88320 / 0xFFFFFFFF as negative Int32: the constants are written as UInt32)
    $poly = [uint32]3988292384
    $all = [uint32]::MaxValue
    if (-not $script:CrcTable) {
        $script:CrcTable = New-Object 'uint32[]' 256
        for ($n = 0; $n -lt 256; $n++) {
            [uint32]$c = $n
            for ($k = 0; $k -lt 8; $k++) { if ($c -band 1) { $c = [uint32]($poly -bxor ($c -shr 1)) } else { $c = $c -shr 1 } }
            $script:CrcTable[$n] = $c
        }
    }
    [uint32]$crc = $all
    for ($i = $start; $i -lt $data.Length; $i++) {
        $crc = [uint32]($script:CrcTable[[int](($crc -bxor $data[$i]) -band 0xFF)] -bxor ($crc -shr 8))
    }
    return [uint32]($crc -bxor $all)
}

function Find-OptionsFile([string]$game) {
    $root = Join-Path $env:USERPROFILE 'Saved Games'
    $full = Join-Path $root 'Beyond Two Souls Steam\Save\Options\0'
    $demo = Join-Path $root 'Beyond Two Souls Steam Demo\Save\Options\0'
    $order = @($full, $demo)
    if ((Split-Path -Leaf $game) -match '(?i)demo') { $order = @($demo, $full) }
    foreach ($p in $order) { if (Test-Path -LiteralPath $p -PathType Leaf) { return $p } }
    return $null
}

# The options file's fields: offsets of the values the installer may change, or $null (unknown version, size or CRC).
function Read-Options([byte[]]$d) {
    if ($d.Length -lt 16) { return $null }
    if ([BitConverter]::ToUInt32($d, 0) -ne (Get-Crc32 $d 4)) { return $null }
    $p = 4
    if ([BitConverter]::ToInt32($d, $p) -ne 0x23) { return $null }
    $p += 4
    $p += 2                         # two bytes
    $p += 16                        # four volumes (floats)
    $p += 8                         # two ints
    for ($s = 0; $s -lt 3; $s++) {  # three languages
        if ($p + 4 -gt $d.Length) { return $null }
        $len = [BitConverter]::ToInt32($d, $p); if ($len -lt 0 -or $len -gt 64) { return $null }
        $p += 4 + $len
    }
    $p += 8                         # two ints
    $p += 8                         # a 64-bit value
    $p += 4                         # an int
    $f = [ordered]@{}
    $f['DisplayMode'] = $p; $p += 4
    $p += 8                         # the resolution (two floats)
    $p += 8                         # two ints
    for ($q = 1; $q -le 6; $q++) { $f["Quality$q"] = $p; $p += 4 }
    $f['MotionBlur'] = $p; $p += 4
    $f['DepthOfField'] = $p; $p += 4
    if ($p + 4 -gt $d.Length) { return $null }
    $len = [BitConverter]::ToInt32($d, $p); if ($len -lt 0 -or $len -gt 256) { return $null }
    $p += 4 + $len                  # the graphics card the preset was made for
    $f['Preset'] = $p; $p += 4
    $p += 16 + 12                   # seven ints
    $p += 2 * 26 * 4                # two button tables
    if ($p -ne $d.Length) { return $null }
    return $f
}
function Get-Field([byte[]]$d, $f, [string]$k) { return [BitConverter]::ToInt32($d, $f[$k]) }
function Set-Field([byte[]]$d, $f, [string]$k, [int]$v) { [Array]::Copy([BitConverter]::GetBytes($v), 0, $d, $f[$k], 4) }
function Write-Options([string]$path, [byte[]]$d) {
    [Array]::Copy([BitConverter]::GetBytes([uint32](Get-Crc32 $d 4)), 0, $d, 0, 4)
    [IO.File]::WriteAllBytes($path, $d)
}

function Read-Recorded {
    $rec = [ordered]@{}
    if (Test-Path -LiteralPath $StateSettings) {
        foreach ($l in Get-Content -LiteralPath $StateSettings) {
            $i = $l.IndexOf('=')
            if ($i -gt 0) { $rec[$l.Substring(0, $i)] = $l.Substring($i + 1) }
        }
    }
    return $rec
}
# A recorded entry is "original|set".
function Split-Recorded([string]$v) {
    $i = $v.IndexOf('|')
    if ($i -ge 0) { return @([int]$v.Substring(0, $i), [int]$v.Substring($i + 1)) }
    return @([int]$v, $null)
}

$Names = @{ 'DisplayMode' = @('Windowed', 'Borderless', 'Fullscreen'); 'Preset' = @('Low', 'Medium', 'High', 'Custom') }
function Format-Value([string]$k, [int]$v) {
    if ($Names.ContainsKey($k) -and $v -ge 0 -and $v -lt $Names[$k].Count) { return $Names[$k][$v] }
    if ($k -in @('MotionBlur', 'DepthOfField')) { if ($v) { return 'On' } else { return 'Off' } }
    return "$v"
}

function Restore-Settings([string]$game) {
    # steam_appid.txt that an older launcher created (it marked it): to the Recycle Bin
    $marker = Join-Path $game $AppIdMarker
    if (Test-Path -LiteralPath $marker) {
        if (Send-ToRecycleBin (Join-Path $game 'steam_appid.txt') $game) { Say 'steam_appid.txt (made by an older version of this mod) -> Recycle Bin' }
        [void](Send-ToRecycleBin $marker $game)
    }
    $rec = Read-Recorded
    if ($rec.Count -eq 0) { return }
    $opt = Find-OptionsFile $game
    if (-not $opt) { Warn 'The game options file was not found - nothing to put back.'; return }
    $d = [IO.File]::ReadAllBytes($opt)
    $f = Read-Options $d
    if (-not $f) {
        Warn 'The game options file is not in the expected form - left alone (set the options yourself in the game).'
        return
    }
    $back = @()
    foreach ($k in $rec.Keys) {
        if (-not $f.Contains($k)) { continue }
        $orig, $set = Split-Recorded $rec[$k]
        $cur = Get-Field $d $f $k
        if ($null -ne $set -and $cur -eq $set) {  # still what the installer set
            Set-Field $d $f $k $orig
            if ($k -notmatch '^Quality') { $back += "$k $(Format-Value $k $orig)" }
        } elseif ($k -notmatch '^Quality') {
            Say "Game option $k left at $(Format-Value $k $cur) (you changed it since)"
        }
    }
    if ($back.Count -gt 0) { Say ("Game options put back: " + ($back -join ', ')) }
    Write-Options $opt $d
    [void](Send-ToRecycleBin $StateSettings $game)
    [void](Send-ToRecycleBin $OptionsBackup $game)
}

# ---- the game's options for VR -----------------------------------------------------------------------------------
# Display mode Windowed (the square VR picture needs it), motion blur off, depth of field off. The original values
# are recorded (settings_changed.txt) so the uninstaller puts them back while they are still what this set. Anything
# that cannot be changed is listed with what to set by hand in the game's options.
function Set-VrGameSettings([string]$game) {
    $manual = [ordered]@{ 'DisplayMode' = 'Display mode: Windowed'; 'MotionBlur' = 'Motion blur: Off'; 'DepthOfField' = 'Depth of field: Off' }
    $want = [ordered]@{ 'DisplayMode' = 0; 'MotionBlur' = 0; 'DepthOfField' = 0 }
    $failed = @()
    $opt = Find-OptionsFile $game
    if (-not $opt) {
        Warn 'The game options file was not found - the game creates it the first time it runs.'
        Warn '(Or start the game once, close it, and run this installer again.)'
        $failed = @($want.Keys)
    } else {
        try {
            $d = [IO.File]::ReadAllBytes($opt)
            $f = Read-Options $d
            if (-not $f) { throw 'the options file is from an unknown version of the game' }
            if (-not (Test-Path -LiteralPath $OptionsBackup)) { [IO.File]::WriteAllBytes($OptionsBackup, $d) }
            $preset = Get-Field $d $f 'Preset'
            if ($preset -ne 3) {
                if (-not $PresetChoices.ContainsKey($preset)) { throw "unknown graphics preset $preset" }
                # Custom with exactly the chosen preset's choices: only motion blur and depth of field change
                for ($q = 1; $q -le 6; $q++) { $want["Quality$q"] = $PresetChoices[$preset][$q - 1] }
                $want['Preset'] = 3
            }
            $rec = Read-Recorded
            foreach ($k in $want.Keys) {
                $old = Get-Field $d $f $k
                Set-Field $d $f $k $want[$k]
                # the first original is kept (a second install must not record the mod's own values as yours)
                $orig = $old
                if ($rec.Contains($k)) { $orig = (Split-Recorded $rec[$k])[0] }
                $rec[$k] = "$orig|$($want[$k])"
            }
            Write-Options $opt $d
            $out = foreach ($k in $rec.Keys) { "$k=$($rec[$k])" }
            Set-Content -LiteralPath $StateSettings -Value $out -Encoding ASCII
            # read back: the file really holds the values now, with a valid checksum
            $check = [IO.File]::ReadAllBytes($opt)
            $cf = Read-Options $check
            if (-not $cf) { throw 'the options file did not read back' }
            foreach ($k in $want.Keys) { if ((Get-Field $check $cf $k) -ne $want[$k]) { $failed += $k } }
        } catch {
            Warn "The game options could not be changed ($($_.Exception.Message))."
            $failed = @($manual.Keys)
        }
    }
    $short = @{ 'DisplayMode' = 'Windowed'; 'MotionBlur' = 'motion blur off'; 'DepthOfField' = 'depth of field off' }
    $done = @($manual.Keys | Where-Object { $failed -notcontains $_ } | ForEach-Object { $short[$_] })
    if ($want.Contains('Preset') -and $failed.Count -eq 0) { $done += 'graphics preset Custom (the same quality)' }
    if ($done.Count -gt 0) { Say ("Game settings set for VR: " + ($done -join ', ')) }
    if ($failed.Count -gt 0) {
        Write-Host ''
        Warn 'These game options could NOT be changed by the installer.'
        Warn 'Please set them yourself in the game (Options -> Graphics):'
        foreach ($k in $failed) { if ($manual.Contains($k)) { Warn "   - $($manual[$k])" } }
    }
}

# ---- the mod's own starting choices ------------------------------------------------------------------------------
# Step 1: first person while walking on, first person in cutscenes off. Step 2 (0.18.1): rendering method AFR.
# Step 3 (1.0.0, owner: "make the mods default rendering native stereo"): rendering method Native. A new
# b2svr.ini (the release's default) has them already. An existing one gets each step ONCE: the key installer_defaults
# holds the last step done - from then on the player's own changes stay.
# The line after the [b2svr] section's last line (the section is added when missing).
function Get-SectionEnd([System.Collections.Generic.List[string]]$lines) {
    $section = -1
    for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^\s*\[b2svr\]\s*$') { $section = $i; break } }
    if ($section -lt 0) { $lines.Add('[b2svr]'); $section = $lines.Count - 1 }
    for ($i = $section + 1; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^\s*\[') { return $i } }
    return $lines.Count
}
function Set-IniKey([System.Collections.Generic.List[string]]$lines, [string]$key, [string]$value) {
    $end = Get-SectionEnd $lines
    $found = $false
    for ($i = 0; $i -lt $end; $i++) {
        if ($lines[$i] -match ('^\s*' + [regex]::Escape($key) + '\s*=')) { $lines[$i] = "$key=$value"; $found = $true }
    }
    if (-not $found) { $lines.Insert($end, "$key=$value") }
}
function Set-VrModDefaults([string]$game) {
    $ini = Join-Path $game 'b2svr.ini'
    if (-not (Test-Path -LiteralPath $ini)) {
        Copy-Item -LiteralPath (Join-Path $Files 'b2svr.ini') -Destination $ini
        Say 'VR settings: first person while walking on, in cutscenes off, rendering Native (b2svr.ini created)'
        return
    }
    try {
        $lines = [System.Collections.Generic.List[string]]([System.IO.File]::ReadAllLines($ini))
        $done = 0
        foreach ($l in $lines) {
            if ($l -match '^\s*installer_defaults\s*=\s*([0-9.]+)') { $done = [double]$Matches[1] }
        }
        if ($done -ge 3) {
            Say 'VR settings: your own choices kept (b2svr.ini)'
            return
        }
        while ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }
        if ($done -lt 1) {
            Set-IniKey $lines 'firstperson' 'on'
            Set-IniKey $lines 'fpcutscenes' 'off'
            Say 'VR settings: first person while walking on, in cutscenes off, rendering Native'
        } else {
            Say 'VR settings: rendering Native (AFR and Mono are in the VR settings panel)'
        }
        Set-IniKey $lines 'rendering_method' 'native'
        for ($i = $lines.Count - 1; $i -ge 0; $i--) {
            if ($lines[$i] -match '^\s*installer_defaults\s*=' -or $lines[$i] -match '^; Set once by the installer') { $lines.RemoveAt($i) }
        }
        $end = Get-SectionEnd $lines
        $lines.Insert($end, 'installer_defaults=3')
        $lines.Insert($end, '; Set once by the installer (first person while walking on, in cutscenes off; rendering method Native) - your own changes are kept.')
        [System.IO.File]::WriteAllLines($ini, $lines, (New-Object System.Text.UTF8Encoding $false))
    } catch {
        Warn "The VR settings (b2svr.ini) could not be changed ($($_.Exception.Message))."
    }
}

# =================================================================================================================
if ($Mode -eq 'Install') {
    Banner 'BEYOND: TWO SOULS VR MOD  -  INSTALL'
    $payload = Join-Path $Files 'dinput8.dll'
    if (-not (Test-Path -LiteralPath $payload)) { Bad 'files\dinput8.dll is missing - unzip the mod again.'; Done 1 }
    if (Test-GameRunning) { Bad 'Beyond: Two Souls is running. Close it, then run this again.'; Done 1 }
    $game = Find-GameDir
    if (-not $game) { Bad 'No Beyond: Two Souls folder found - nothing was changed.'; Done 1 }
    Say "Game folder: $game"
    $dst = Join-Path $game 'dinput8.dll'
    if ((Test-Path -LiteralPath $dst) -and -not (Test-OurDll $dst)) {
        Bad 'There is already a dinput8.dll in the game folder that is NOT this mod'
        Say '(another mod, an ASI loader or an overlay). It was left alone. Move it somewhere else if you want'
        Say 'this VR mod instead, then run this again.'
        Done 1
    }
    Set-Content -LiteralPath $StateGame -Value $game -Encoding UTF8

    if (Test-Path -LiteralPath $dst) {
        if ((Get-FileHash -LiteralPath $dst).Hash -eq (Get-FileHash -LiteralPath $payload).Hash) {
            Say 'dinput8.dll: already installed and up to date'
        } else {
            Copy-Item -LiteralPath $payload -Destination $dst -Force
            Say 'dinput8.dll: updated to this release'
        }
    } else {
        Copy-Item -LiteralPath $payload -Destination $dst
        Say 'dinput8.dll: installed'
    }
    try { Unblock-File -LiteralPath $dst } catch { }

    Set-VrGameSettings $game
    Set-VrModDefaults $game

    Write-Host ''
    Say 'Done. Start Beyond: Two Souls as usual with your headset on - VR starts by itself.'
    Say 'To remove the mod: "Uninstall VR mod.cmd" in this folder.'
    Done 0
}

if ($Mode -eq 'Uninstall') {
    Banner 'BEYOND: TWO SOULS VR MOD  -  UNINSTALL'
    if (Test-GameRunning) { Bad 'Beyond: Two Souls is running - close it first, then run this again.'; Done 1 }
    $game = Find-GameDir
    if (-not $game) { Bad 'No Beyond: Two Souls folder found - nothing was changed.'; Done 1 }
    Say "Game folder: $game"
    Write-Host ''
    Say 'This will move to the Recycle Bin (nothing is deleted for good):'
    Say '  - dinput8.dll (the mod) from the game folder'
    Say '  - if you want: b2svr.ini (your VR settings) and the b2svr_logs folder'
    Say '  - this B2S_VR_MOD folder'
    Say 'Game options the installer changed are put back. Your saves, the game''s exe and every'
    Say 'other game file are left alone.'
    Write-Host ''
    $a = Read-Host '   Type YES and press Enter to uninstall (anything else cancels)'
    if ($a -ne 'YES' -and $a -ne 'yes') { Say 'Cancelled. Nothing was changed.'; Done 0 }
    Write-Host ''

    $dll = Join-Path $game 'dinput8.dll'
    if (Test-Path -LiteralPath $dll) {
        if (Test-OurDll $dll) {
            if (Send-ToRecycleBin $dll $game) { Say 'dinput8.dll (the mod) -> Recycle Bin' }
        } else { Warn 'dinput8.dll in the game folder is not this mod - left alone' }
    } else { Say 'dinput8.dll: not installed' }

    Restore-Settings $game

    $k = Read-Key 'Also move b2svr.ini (your VR settings) and the b2svr_logs folder to the Recycle Bin? Press Y or N:' @('Y', 'N')
    if ($k -eq 'Y') {
        foreach ($f in @('b2svr.ini', 'b2svr_logs', 'b2svr.log', 'b2svr.prev.log', 'b2svr_cmd.txt', 'b2svr_cmd.done',
                         'b2svr_camdump.bin', 'b2svr_vpdump.bin')) {
            if (Send-ToRecycleBin (Join-Path $game $f) $game) { Say "$f -> Recycle Bin" }
        }
    } else {
        Say 'b2svr.ini and b2svr_logs kept'
    }

    # this folder: only when it is really the mod's own folder - inside the game folder but not the game folder,
    # with the mod's files in it and no program file anywhere inside (Send-ToRecycleBin checks the last two again)
    $modOk = (Test-Path -LiteralPath (Join-Path $ModDir 'tools\b2svr_setup.ps1')) -and
             (Test-Path -LiteralPath (Join-Path $ModDir 'Uninstall VR mod.cmd')) -and
             -not (Test-GameDir $ModDir)
    Write-Host ''
    if ($modOk -and (Send-ToRecycleBin $ModDir $game)) {
        Say "$ModDir -> Recycle Bin"
    } else {
        Say "This folder was left where it is: $ModDir"
        Say 'Delete it yourself if you do not need it any more.'
    }
    Write-Host ''
    Say 'Done. Beyond: Two Souls starts normally on your monitor again.'
    Done 0
}
