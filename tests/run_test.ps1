# Runs the NoveskeDev harness in the installed game, flat (-nohmd) in a window, with the built Noveske Chainsaw
# plugin. [Test] Scenario picks the run (default movables). The player's SaveGames, every GML config and the plugins folder are
# snapshotted first and put back after the game is closed (hash-verified); the harness never stays installed.
# The game's "shot" screenshots taken during the run are copied to -Out in order, named after the log's SNAP lines.
# Run it plainly (never pipe it into Select-Object -First: that stops the script before the restore).
param([int]$Timeout = 300, [string]$Scenario = "movables", [string]$Out = "$PSScriptRoot\..\build\shots")
$ErrorActionPreference = "Stop"
$g = "C:\Program Files (x86)\Steam\steamapps\common\GERONIMO\Geronimo\Binaries\Win64"
$saves = "$env:LOCALAPPDATA\Geronimo\Saved\SaveGames"
$shots = "$env:LOCALAPPDATA\Geronimo\Saved\Screenshots"
$build = Resolve-Path "$PSScriptRoot\..\build"
$snap = Join-Path $env:TEMP ("noveske_snap_" + (Get-Date -Format yyyyMMdd_HHmmss))
if (Get-Process -Name "Geronimo*" -ErrorAction SilentlyContinue) { "Geronimo is running - not launching"; exit 1 }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
Get-ChildItem $Out -Filter *.png -ErrorAction SilentlyContinue | Remove-Item -Force

# Plugins' Mods folders (content plugins: Myrne alone is 3.5 GB) stay out of the snapshot, the hash check and the
# restore - a test never changes them.
$hash = { param($p) Get-ChildItem $p -File -Recurse | Where-Object { $_.FullName -notmatch '\\plugins\\[^\\]+\\Mods\\' } | Sort-Object FullName | ForEach-Object { $_.FullName.Substring($p.Length) + ':' + (Get-FileHash $_.FullName -Algorithm MD5).Hash } }
robocopy $saves "$snap\SaveGames" /MIR /NJH /NJS /NFL /NDL /NP | Out-Null
robocopy "$g\GML\config" "$snap\config" /MIR /NJH /NJS /NFL /NDL /NP | Out-Null
robocopy "$g\GML\plugins" "$snap\plugins" /MIR /XD Mods /R:2 /W:1 /NJH /NJS /NFL /NDL /NP | Out-Null
$before = (& $hash $saves) + (& $hash "$g\GML\config") + (& $hash "$g\GML\plugins")
$appid = -not (Test-Path "$g\steam_appid.txt")
$doorstop = "$g\doorstop_config.ini"
$doorstopBefore = [IO.File]::ReadAllBytes($doorstop)
"snapshot: $snap"

$log = "$g\GML\LogOutput.log"
$t0 = Get-Date
try {
    foreach ($p in "NoveskeChainsaw", "NoveskeDev") { New-Item -ItemType Directory -Force "$g\GML\plugins\$p" | Out-Null }
    Copy-Item "$build\NoveskeChainsaw\NoveskeChainsaw.dll" "$g\GML\plugins\NoveskeChainsaw\"
    Copy-Item "$build\tests\NoveskeDev\NoveskeDev.dll" "$g\GML\plugins\NoveskeDev\"
    Set-Content "$g\GML\config\com.remarrow.noveskedev.cfg" "[Test]`r`nScenario = $Scenario`r`n" -Encoding ascii
    if ($appid) { Set-Content "$g\steam_appid.txt" "2019620" -NoNewline }
    $ds = [Text.Encoding]::UTF8.GetString($doorstopBefore)
    if ($ds -match '(?m)^enabled\s*=\s*false') { [IO.File]::WriteAllText($doorstop, ($ds -replace '(?m)^enabled\s*=\s*false', 'enabled = true')); "GML was switched off: on for this run" }
    Start-Process -FilePath "$g\Geronimo-Win64-Shipping.exe" -ArgumentList "-nohmd -windowed -ResX=1280 -ResY=720" | Out-Null
    $deadline = $t0.AddSeconds($Timeout)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep 3
        if ((Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $t0 -and (Select-String -Path $log -Pattern "DEV TEST COMPLETE" -Quiet)) { "harness finished after $([int]((Get-Date) - $t0).TotalSeconds) s"; break }
        if (-not (Get-Process -Name "Geronimo-Win64-Shipping" -ErrorAction SilentlyContinue) -and ((Get-Date) - $t0).TotalSeconds -gt 30) { "THE GAME EXITED"; break }
    }
    if ((Get-Date) -ge $deadline) { "TIMEOUT after $Timeout s" }
} finally {
    Get-Process -Name "Geronimo*" -ErrorAction SilentlyContinue | Stop-Process -Force
    for ($i = 0; $i -lt 30 -and (Get-Process -Name "Geronimo*" -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }
    Start-Sleep 4
    if (Test-Path $log) { Copy-Item $log "$Out\LogOutput.log" -Force }
    if ($appid) { Remove-Item "$g\steam_appid.txt" -ErrorAction SilentlyContinue }
    [IO.File]::WriteAllBytes($doorstop, $doorstopBefore)
    robocopy "$snap\SaveGames" $saves /MIR /NJH /NJS /NFL /NDL /NP | Out-Null
    robocopy "$snap\config" "$g\GML\config" /MIR /NJH /NJS /NFL /NDL /NP | Out-Null
    robocopy "$snap\plugins" "$g\GML\plugins" /MIR /XD Mods /R:2 /W:1 /NJH /NJS /NFL /NDL /NP | Out-Null
    if (-not (Test-Path "$snap\plugins\NoveskeDev")) { Remove-Item -Recurse -Force "$g\GML\plugins\NoveskeDev" -ErrorAction SilentlyContinue }
    $after = (& $hash $saves) + (& $hash "$g\GML\config") + (& $hash "$g\GML\plugins")
    if (Compare-Object $before $after) { "RESTORE MISMATCH - snapshot kept at $snap" } else { "saves, configs and plugins restored (hash verified)"; Remove-Item -Recurse -Force $snap }
    # The screenshots, in the order taken, named after the SNAP lines.
    $names = @()
    if (Test-Path "$Out\LogOutput.log") { $names = Select-String -Path "$Out\LogOutput.log" -Pattern "SNAP (\S+)" | ForEach-Object { $_.Matches[0].Groups[1].Value } }
    $pngs = @(Get-ChildItem $shots -Recurse -Filter *.png -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $t0 } | Sort-Object LastWriteTime)
    for ($i = 0; $i -lt $pngs.Count; $i++) {
        $n = if ($i -lt $names.Count) { "{0:D2}_{1}.png" -f $i, $names[$i] } else { "{0:D2}_extra.png" -f $i }
        Move-Item $pngs[$i].FullName (Join-Path $Out $n) -Force
    }
    "$($pngs.Count) screenshots ($($names.Count) SNAP lines) in $Out"
}
