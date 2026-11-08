# Collects everything needed to diagnose a problem with DD2 NTC Textures into one ZIP on the Desktop.
# Usage: report.bat  (asks for the game folder)  or  report.ps1 -GamePath "C:\...\Dragons Dogma 2"
# The ZIP contains logs and system info only (GPU, driver, RAM, disk, crash events). Review it before sharing.
param([string]$GamePath = '')
$ErrorActionPreference = 'Continue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $GamePath) {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = 'Choose the Dragon''s Dogma 2 folder (the one with DD2.exe)'
    if ($dialog.ShowDialog() -ne 'OK') { Write-Host 'No folder chosen.'; return }
    $GamePath = $dialog.SelectedPath
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class DiskSize {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern uint GetCompressedFileSizeW(string path, out uint high);
    public static long OnDisk(string path) {
        uint high; uint low = GetCompressedFileSizeW(path, out high);
        return ((long)high << 32) + low;
    }
}
'@

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$work = Join-Path $env:TEMP "dd2-ntc-report-$stamp"
New-Item -ItemType Directory -Force -Path $work | Out-Null
$info = New-Object System.Collections.Generic.List[string]
function Add($line) { $info.Add($line) }

Add "DD2 NTC Textures report, $(Get-Date -Format s)"
Add "Game folder: $GamePath"
Add ''
Add '== System'
$os = Get-CimInstance Win32_OperatingSystem
Add "Windows: $($os.Caption) $($os.Version) build $($os.BuildNumber)"
Add "CPU: $((Get-CimInstance Win32_Processor | Select-Object -First 1).Name)"
Add ("RAM: {0:N1} GiB" -f ((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB))
foreach ($gpu in Get-CimInstance Win32_VideoController) {
    Add ("GPU: {0}, driver {1} ({2}), VRAM reported {3:N1} GiB" -f $gpu.Name, $gpu.DriverVersion, $gpu.DriverDate, ($gpu.AdapterRAM / 1GB))
}
try {
    $drive = (Get-Item -LiteralPath $GamePath).PSDrive.Name
    $volume = Get-Volume -DriveLetter $drive
    Add ("Game drive: {0}: {1}, {2:N1} GiB free of {3:N1} GiB" -f $drive, $volume.FileSystem, ($volume.SizeRemaining / 1GB), ($volume.Size / 1GB))
} catch { Add "Game drive: unknown ($_)" }

Add ''
Add '== Game files'
$exe = Join-Path $GamePath 'DD2.exe'
if (Test-Path -LiteralPath $exe) {
    Add ("DD2.exe: {0} bytes, SHA-256 {1}" -f (Get-Item -LiteralPath $exe).Length, (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash)
} else { Add 'DD2.exe: NOT FOUND' }
Add "REFramework (dinput8.dll): $(Test-Path -LiteralPath (Join-Path $GamePath 'dinput8.dll'))"
foreach ($pak in 're_chunk_000.pak', 'dlc\re_dlc_stm_2593290.pak') {
    $path = Join-Path $GamePath $pak
    if (Test-Path -LiteralPath $path) {
        $item = Get-Item -LiteralPath $path
        $sparse = [bool]($item.Attributes -band [IO.FileAttributes]::SparseFile)
        Add ("{0}: size {1:N2} GiB, on disk {2:N2} GiB, sparse={3}" -f $pak, ($item.Length / 1GB), ([DiskSize]::OnDisk($path) / 1GB), $sparse)
    } else { Add "${pak}: NOT FOUND" }
}
$plugins = Join-Path $GamePath 'reframework\plugins'
$dll = Join-Path $plugins 'dd2_ntc_bridge.dll'
if (Test-Path -LiteralPath $dll) { Add "dd2_ntc_bridge.dll: SHA-256 $((Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash)" } else { Add 'dd2_ntc_bridge.dll: NOT FOUND' }
$data = Join-Path $plugins 'dd2_ntc_data'
if (Test-Path -LiteralPath $data) {
    $files = Get-ChildItem -LiteralPath $data -File
    Add ("dd2_ntc_data: {0} files, {1:N2} GiB" -f $files.Count, (($files | Measure-Object Length -Sum).Sum / 1GB))
    $markers = $files | Where-Object { $_.Extension -eq '.enabled' } | ForEach-Object Name
    if ($markers) { Add "Test markers present: $($markers -join ', ')" }
} else { Add 'dd2_ntc_data: NOT FOUND' }
Add "Other plugins: $((Get-ChildItem -LiteralPath $plugins -File -ErrorAction SilentlyContinue | ForEach-Object Name) -join ', ')"

Add ''
Add '== Recent DD2 crashes (Windows Application log)'
$events = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000, 1001 } -MaxEvents 300 -ErrorAction SilentlyContinue |
    Where-Object { $_.Message -match 'DD2\.exe' } | Select-Object -First 8
if ($events) { foreach ($e in $events) { Add "--- $($e.TimeCreated.ToString('s')) id $($e.Id)"; Add ($e.Message -split "`r?`n" | Select-Object -First 14 | Out-String).Trim() } }
else { Add 'none found' }

$info | Set-Content -LiteralPath (Join-Path $work 'system.txt') -Encoding utf8

# Logs: REFramework keeps this session and the previous one. The plugin writes its errors and warnings there.
foreach ($name in 're2_framework_log.txt', 're2_framework_log.prev.txt', 'dd2-ntc-install.json', 'reframework_crash.dmp') {
    $path = Join-Path $GamePath $name
    if (Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination $work }
}
foreach ($name in 're2_framework_log.txt', 're2_framework_log.prev.txt') {
    $path = Join-Path $GamePath $name
    if (Test-Path -LiteralPath $path) {
        Select-String -LiteralPath $path -Pattern 'dd2_ntc_bridge' | ForEach-Object Line |
            Set-Content -LiteralPath (Join-Path $work ($name -replace 're2_framework_log', 'plugin') ) -Encoding utf8
    }
}
$installLogs = Join-Path $here 'logs'
if (Test-Path -LiteralPath $installLogs) { Copy-Item -LiteralPath $installLogs -Destination (Join-Path $work 'installer-logs') -Recurse }

$zip = Join-Path ([Environment]::GetFolderPath('Desktop')) "DD2-NTC-report-$stamp.zip"
Compress-Archive -Path (Join-Path $work '*') -DestinationPath $zip -Force
Remove-Item -LiteralPath $work -Recurse -Force
Write-Host "Report saved: $zip" -ForegroundColor Green
Write-Host 'Attach it to an issue on GitHub or a bug report on Nexus, with a short description of what happened.'
