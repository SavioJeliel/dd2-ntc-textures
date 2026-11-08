# Dragon's Dogma 2 NTC texture mod installer.
# Checks the game build, installs the REFramework plugin with the NTC data, and zeroes in the PAKs
# the texture blocks the plugin rebuilds. Zeroing cannot be undone by this script: keep a backup of the game.
#
# Usage:  install.bat  (asks for the game folder)
#     or: powershell -ExecutionPolicy Bypass -File install.ps1 -GamePath "C:\...\Dragons Dogma 2" [-VerifyOnly]
param(
    [string]$GamePath = '',
    [string]$Payload = '',
    [switch]$VerifyOnly
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Payload) { $Payload = Join-Path $here 'payload' }

# Every run leaves a log next to the installer, in success or failure. report.bat collects it.
$logs = Join-Path $here 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
Start-Transcript -LiteralPath (Join-Path $logs "install-$stamp.log") | Out-Null
Write-Host "DD2 NTC Textures installer, $(Get-Date -Format s), VerifyOnly=$VerifyOnly"
Write-Host "Windows: $([Environment]::OSVersion.VersionString), PowerShell $($PSVersionTable.PSVersion)"
trap {
    Write-Host "ERROR: $_" -ForegroundColor Red
    Write-Host "Log saved in $logs. Run report.bat and attach the ZIP when asking for help."
    Stop-Transcript | Out-Null
    break
}

Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using Microsoft.Win32.SafeHandles;

public static class NtcPak {
    const uint FSCTL_SET_SPARSE = 0x900C4, FSCTL_SET_ZERO_DATA = 0x980C8;
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] input, int inputSize,
                                       IntPtr output, int outputSize, out int returned, IntPtr overlapped);
    static byte[] buffer = new byte[64 * 1024 * 1024];
    static SHA256 sha = SHA256.Create();

    static int Fill(FileStream file, long offset, int length) {
        if (length > buffer.Length) buffer = new byte[length];
        file.Position = offset;
        int read = 0;
        while (read < length) {
            int n = file.Read(buffer, read, length - read);
            if (n <= 0) throw new IOException("Unexpected end of file");
            read += n;
        }
        return length;
    }

    // 0 = original, 1 = already zeroed, 2 = other content.
    public static int Check(FileStream file, long offset, int length, string expected) {
        Fill(file, offset, length);
        bool zero = true;
        for (int i = 0; i < length; ++i) if (buffer[i] != 0) { zero = false; break; }
        if (zero) return 1;
        string hash = BitConverter.ToString(sha.ComputeHash(buffer, 0, length)).Replace("-", "").ToLowerInvariant();
        return hash == expected ? 0 : 2;
    }

    public static string HashFile(string path) {
        using (var stream = File.OpenRead(path))
            return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "").ToLowerInvariant();
    }

    public static bool IsZero(FileStream file, long offset, int length) {
        Fill(file, offset, length);
        for (int i = 0; i < length; ++i) if (buffer[i] != 0) return false;
        return true;
    }

    static void Ioctl(FileStream file, uint code, byte[] input) {
        int returned;
        if (!DeviceIoControl(file.SafeFileHandle, code, input, input == null ? 0 : input.Length, IntPtr.Zero, 0, out returned, IntPtr.Zero))
            throw new IOException("DeviceIoControl failed: " + Marshal.GetLastWin32Error());
    }

    public static void SetSparse(FileStream file) { Ioctl(file, FSCTL_SET_SPARSE, null); }

    public static void Zero(FileStream file, long offset, long length) {
        byte[] input = new byte[16];
        BitConverter.GetBytes(offset).CopyTo(input, 0);
        BitConverter.GetBytes(offset + length).CopyTo(input, 8);
        Ioctl(file, FSCTL_SET_ZERO_DATA, input);
    }
}
'@

function Step($text) { Write-Host "== $text" -ForegroundColor Cyan }

if (-not $GamePath) {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = 'Choose the Dragon''s Dogma 2 folder (the one with DD2.exe)'
    if ($dialog.ShowDialog() -ne 'OK') { throw 'No folder chosen.' }
    $GamePath = $dialog.SelectedPath
}
if (-not (Test-Path -LiteralPath (Join-Path $GamePath 'DD2.exe'))) {
    throw "DD2.exe not found in '$GamePath'. Choose the folder that contains DD2.exe."
}
$GamePath = (Resolve-Path -LiteralPath $GamePath).Path
$manifestPath = Join-Path $Payload 'zero-ranges.json'
if (-not (Test-Path -LiteralPath $manifestPath)) { throw "Package not found: $manifestPath. Extract all ZIP parts into the same folder." }
if (Get-Process -Name DD2 -ErrorAction SilentlyContinue) { throw 'Close Dragon''s Dogma 2 before installing.' }

Step 'Checking package files'
$files = Get-Content -LiteralPath (Join-Path $Payload 'payload-files.json') -Raw | ConvertFrom-Json
$broken = @()
foreach ($entry in $files.PSObject.Properties) {
    $path = Join-Path $Payload $entry.Name
    if (-not (Test-Path -LiteralPath $path) -or [NtcPak]::HashFile($path) -ne $entry.Value) { $broken += $entry.Name }
}
if ($broken.Count) { throw "$($broken.Count) package files missing or corrupted (e.g. $($broken[0])). Download and extract all parts again." }

# The PAK blocks will be gone after install, so the plugin has no fallback: the GPU route must work here.
$gpus = Get-CimInstance Win32_VideoController
$gpuName = ($gpus | Select-Object -ExpandProperty Name) -join ', '
foreach ($gpu in $gpus) { Write-Host "GPU: $($gpu.Name), driver $($gpu.DriverVersion)" }
Step "GPU self-test ($gpuName)"
$selftest = Join-Path $Payload 'selftest'
$result = Join-Path $logs "selftest-$stamp.csv"
$benchErr = Join-Path $logs "selftest-$stamp.stderr.txt"
& (Join-Path $selftest 'ntc_gpu_bench.exe') gpu (Join-Path $Payload 'reframework\plugins\dd2_ntc_data') (Join-Path $selftest 'list.tsv') $result 2> $benchErr | Out-Null
Write-Host "   self-test exit code $LASTEXITCODE"
$rows = @(if (Test-Path -LiteralPath $result) { Import-Csv -LiteralPath $result })
$failed = @($rows | Where-Object { $_.route -ne 'gpu' -or $_.status -ne 'ok' })
if (-not $rows.Count -or $failed.Count) {
    $why = if ($failed.Count) { "$($failed.Count) of $($rows.Count) textures failed on the GPU (e.g. $($failed[0].asset) $($failed[0].variant): $($failed[0].route) $($failed[0].status))" } else { 'the self-test did not run' }
    throw "This PC cannot run the mod: $why. Nothing was changed. Please report your GPU and driver version."
}
Write-Host "   $($rows.Count) textures rebuilt on the GPU, no failures"

Step 'Reading the block list'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json

Step "Checking the game build ($($manifest.game_build))"
$exe = Join-Path $GamePath $manifest.exe.name
$exeItem = Get-Item -LiteralPath $exe
if ($exeItem.Length -ne $manifest.exe.size -or (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLower() -ne $manifest.exe.sha256) {
    throw 'DD2.exe is from a different build. This package only works with the build named above.'
}
if (-not (Test-Path -LiteralPath (Join-Path $GamePath 'dinput8.dll'))) {
    throw 'REFramework not found (dinput8.dll). Install REFramework for DD2 and run again.'
}
$drive = (Get-Item -LiteralPath $GamePath).PSDrive.Name
if ((Get-Volume -DriveLetter $drive).FileSystem -ne 'NTFS') { throw 'The game must be on an NTFS drive.' }

$totals = @{}
foreach ($pak in $manifest.paks.PSObject.Properties) {
    $path = Join-Path $GamePath $pak.Name
    if (-not (Test-Path -LiteralPath $path)) { throw "PAK not found: $path" }
    if ((Get-Item -LiteralPath $path).Length -ne $pak.Value.size) { throw "Unexpected size of $($pak.Name): different game build." }
    Step "Checking $($pak.Value.ranges.Count) blocks of $($pak.Name)"
    $count = @(0, 0, 0)
    $bad = @()
    $file = [IO.File]::Open($path, 'Open', 'Read', 'Read')
    try {
        $i = 0
        foreach ($r in $pak.Value.ranges) {
            $state = [NtcPak]::Check($file, [long]$r[0], [int]$r[1], [string]$r[2])
            $count[$state]++
            if ($state -eq 2 -and $bad.Count -lt 5) { $bad += $r[0] }
            if ((++$i % 5000) -eq 0) { Write-Host "   $i blocks" }
        }
    } finally { $file.Dispose() }
    Write-Host "   original: $($count[0])  already zeroed: $($count[1])  different: $($count[2])"
    if ($count[2]) { throw "Unexpected content in $($pak.Name) (offsets $($bad -join ', ')). Nothing was changed." }
    $totals[$pak.Name] = $count
}

if ($VerifyOnly) {
    Write-Host 'Check complete: the game is compatible. Nothing was changed.' -ForegroundColor Green
    Stop-Transcript | Out-Null
    return
}

Step 'Installing the plugin and NTC data'
$plugins = Join-Path $GamePath 'reframework\plugins'
New-Item -ItemType Directory -Force -Path $plugins | Out-Null
foreach ($name in 'dd2_ntc_bridge.dll', 'dd2_ntc_data') {
    $old = Join-Path $plugins $name
    if (Test-Path -LiteralPath $old) { Rename-Item -LiteralPath $old -NewName "$name.backup-$stamp" }
}
Copy-Item -LiteralPath (Join-Path $Payload 'reframework\plugins\dd2_ntc_bridge.dll') -Destination $plugins
Copy-Item -LiteralPath (Join-Path $Payload 'reframework\plugins\dd2_ntc_data') -Destination $plugins -Recurse
$expected = (Get-ChildItem -LiteralPath (Join-Path $Payload 'reframework\plugins\dd2_ntc_data') -File).Count
if ((Get-ChildItem -LiteralPath (Join-Path $plugins 'dd2_ntc_data') -File).Count -ne $expected) { throw 'Copying the data was incomplete. The PAKs were not changed.' }

foreach ($pak in $manifest.paks.PSObject.Properties) {
    $path = Join-Path $GamePath $pak.Name
    Step "Zeroing $($totals[$pak.Name][0]) blocks of $($pak.Name)"
    $file = [IO.File]::Open($path, 'Open', 'ReadWrite', 'None')
    try {
        [NtcPak]::SetSparse($file)
        foreach ($r in $pak.Value.ranges) { [NtcPak]::Zero($file, [long]$r[0], [long]$r[1]) }
        $file.Flush()
        foreach ($r in $pak.Value.ranges) {
            if (-not [NtcPak]::IsZero($file, [long]$r[0], [int]$r[1])) { throw "Block not zeroed in $($pak.Name): $($r[0])" }
        }
    } finally { $file.Dispose() }
}

$state = [ordered]@{ installed = (Get-Date).ToString('s'); game_build = $manifest.game_build; paks = $totals }
$state | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $GamePath 'dd2-ntc-install.json') -Encoding utf8
Write-Host 'Installation complete. Start the game as usual.' -ForegroundColor Green
Write-Host 'If this mod saved you space, you can support it: https://buymeacoffee.com/saviojeliel' -ForegroundColor Yellow
Write-Host 'If something goes wrong in game, run report.bat and attach the ZIP to an issue.'
Stop-Transcript | Out-Null
