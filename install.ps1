# Dragon's Dogma 2 NTC texture mod installer.
# Installs the REFramework plugin with the NTC data and zeroes, in the PAKs, the texture blocks the plugin rebuilds.
# Flexible by design: it finds the plugin's hook points in DD2.exe by signature, picks the matching game version
# from the block contents, and only zeroes blocks that are byte-identical to what the NTC data was made from.
# Anything else (modded or updated blocks) is left untouched. Zeroing cannot be undone by this script: keep a backup.
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
using System.Collections.Generic;
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

    static bool Fill(FileStream file, long offset, int length) {
        if (offset < 0 || offset + length > file.Length) return false;
        if (length > buffer.Length) buffer = new byte[length];
        file.Position = offset;
        int read = 0;
        while (read < length) {
            int n = file.Read(buffer, read, length - read);
            if (n <= 0) return false;
            read += n;
        }
        return true;
    }

    // 0 = original, 1 = already zeroed, 2 = different content (or outside the file).
    public static int Check(FileStream file, long offset, int length, string expected) {
        if (!Fill(file, offset, length)) return 2;
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
        if (!Fill(file, offset, length)) return false;
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

    // Counts how many times each signature ("41 57 ?? ...") appears in the .text section of an executable file.
    public static int[] CountSignatures(string exePath, string[] patterns) {
        byte[] data = File.ReadAllBytes(exePath);
        int pe = BitConverter.ToInt32(data, 0x3C);
        int sections = BitConverter.ToUInt16(data, pe + 6);
        int optional = BitConverter.ToUInt16(data, pe + 20);
        int table = pe + 24 + optional, start = 0, size = 0;
        for (int i = 0; i < sections; ++i) {
            int s = table + i * 40;
            if (data[s] == '.' && data[s + 1] == 't' && data[s + 2] == 'e' && data[s + 3] == 'x' && data[s + 4] == 't') {
                size = BitConverter.ToInt32(data, s + 16);   // SizeOfRawData
                start = BitConverter.ToInt32(data, s + 20);  // PointerToRawData
            }
        }
        var counts = new int[patterns.Length];
        for (int p = 0; p < patterns.Length; ++p) {
            string[] tokens = patterns[p].Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
            var bytes = new byte[tokens.Length];
            var fixedByte = new bool[tokens.Length];
            for (int i = 0; i < tokens.Length; ++i) {
                fixedByte[i] = tokens[i] != "??";
                if (fixedByte[i]) bytes[i] = Convert.ToByte(tokens[i], 16);
            }
            int anchor = 0, anchorLen = 0;
            for (int i = 0; i < tokens.Length;) {
                int j = i;
                while (j < tokens.Length && fixedByte[j]) ++j;
                if (j - i > anchorLen) { anchor = i; anchorLen = j - i; }
                i = j + 1;
            }
            int end = start + size - tokens.Length;
            for (int at = start; at <= end; ++at) {
                if (data[at + anchor] != bytes[anchor]) continue;
                bool ok = true;
                for (int k = 0; k < tokens.Length && ok; ++k) ok = !fixedByte[k] || data[at + k] == bytes[k];
                if (ok) counts[p]++;
            }
        }
        return counts;
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
if (-not (Test-Path -LiteralPath (Join-Path $Payload 'versions'))) { throw "Package not found in $Payload. Extract all ZIP parts into the same folder." }
if (Get-Process -Name DD2 -ErrorAction SilentlyContinue) { throw 'Close Dragon''s Dogma 2 before installing.' }

Step 'Checking package files'
$files = Get-Content -LiteralPath (Join-Path $Payload 'payload-files.json') -Raw | ConvertFrom-Json
$broken = @()
foreach ($entry in $files.PSObject.Properties) {
    $path = Join-Path $Payload $entry.Name
    if (-not (Test-Path -LiteralPath $path) -or [NtcPak]::HashFile($path) -ne $entry.Value) { $broken += $entry.Name }
}
if ($broken.Count) { throw "$($broken.Count) package files missing or corrupted (e.g. $($broken[0])). Download and extract all parts again." }

# The PAK blocks will be gone after install, so the plugin has no fallback for them: the GPU route must work here.
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

# DD2.exe: no hash check. The plugin finds its hook points by signature, so any build with the same code works.
$exe = Join-Path $GamePath 'DD2.exe'
$exeItem = Get-Item -LiteralPath $exe
Step "Checking DD2.exe (version $($exeItem.VersionInfo.FileVersion))"
$signatures = Get-Content -LiteralPath (Join-Path $Payload 'signatures.json') -Raw | ConvertFrom-Json
$names = @($signatures.PSObject.Properties.Name)
$counts = [NtcPak]::CountSignatures($exe, [string[]]@($names | ForEach-Object { $signatures.$_ }))
$missing = @()
for ($i = 0; $i -lt $names.Count; ++$i) {
    Write-Host "   $($names[$i]): found $($counts[$i]) time(s)"
    # The streaming notice is optional for the plugin; the others are required.
    if ($counts[$i] -ne 1 -and $names[$i] -ne 'streaming_notice') { $missing += $names[$i] }
}
if ($missing.Count) { throw "This DD2.exe is not supported yet: the plugin cannot find $($missing -join ', '). Nothing was changed. Please report your game version." }
if (-not (Test-Path -LiteralPath (Join-Path $GamePath 'dinput8.dll'))) {
    throw 'REFramework not found (dinput8.dll). Install REFramework for DD2 and run again.'
}
$drive = (Get-Item -LiteralPath $GamePath).PSDrive.Name
if ((Get-Volume -DriveLetter $drive).FileSystem -ne 'NTFS') { throw 'The game must be on an NTFS drive.' }

# Pick the game version whose texture blocks match this copy, by sampling a few hundred blocks of each.
Step 'Detecting the game version from the PAK files'
$best = $null
foreach ($folder in Get-ChildItem -LiteralPath (Join-Path $Payload 'versions') -Directory) {
    $ranges = Get-Content -LiteralPath (Join-Path $folder.FullName 'ranges.json') -Raw | ConvertFrom-Json
    $sampled = 0; $matched = 0
    foreach ($pak in $ranges.PSObject.Properties) {
        $path = Join-Path $GamePath $pak.Name
        if (-not (Test-Path -LiteralPath $path)) { continue }
        $list = @($pak.Value)
        $step = [Math]::Max(1, [int]($list.Count / 200))
        $file = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
        try {
            for ($i = 0; $i -lt $list.Count; $i += $step) {
                $r = $list[$i]
                $sampled++
                if ([NtcPak]::Check($file, [long]$r[0], [int]$r[1], [string]$r[2]) -ne 2) { $matched++ }
            }
        } finally { $file.Dispose() }
    }
    $score = if ($sampled) { $matched / $sampled } else { 0 }
    Write-Host ("   version {0}: {1} of {2} sampled blocks match ({3:P0})" -f $folder.Name, $matched, $sampled, $score)
    if (-not $best -or $score -gt $best.Score) { $best = [pscustomobject]@{ Name = $folder.Name; Folder = $folder.FullName; Ranges = $ranges; Score = $score } }
}
if (-not $best -or $best.Score -lt 0.5) {
    throw 'Your game files do not match any version this pack supports. Nothing was changed. Please report your game version (DD2.exe version shown above).'
}
Write-Host "   using version $($best.Name)" -ForegroundColor Green

# Full check: every block is either original (will be zeroed), already zeroed, or different (left untouched).
$plan = @{}
$owners = New-Object 'System.Collections.Generic.HashSet[string]'
$totalOriginal = 0; $totalZeroed = 0; $totalDifferent = 0
foreach ($pak in $best.Ranges.PSObject.Properties) {
    $path = Join-Path $GamePath $pak.Name
    if (-not (Test-Path -LiteralPath $path)) {
        Write-Host "   $($pak.Name) not found: skipping it" -ForegroundColor Yellow
        continue
    }
    Step "Checking $(@($pak.Value).Count) blocks of $($pak.Name)"
    $toZero = New-Object System.Collections.ArrayList
    $count = @(0, 0, 0)
    $file = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try {
        foreach ($r in $pak.Value) {
            $state = [NtcPak]::Check($file, [long]$r[0], [int]$r[1], [string]$r[2])
            $count[$state]++
            if ($state -ne 2) { foreach ($asset in $r[3]) { [void]$owners.Add($asset) } }
            if ($state -eq 0) { [void]$toZero.Add($r) }
        }
    } finally { $file.Dispose() }
    Write-Host "   original: $($count[0])  already zeroed: $($count[1])  different (left untouched): $($count[2])"
    $totalOriginal += $count[0]; $totalZeroed += $count[1]; $totalDifferent += $count[2]
    $plan[$pak.Name] = $toZero
}
if (-not ($totalOriginal + $totalZeroed)) { throw 'No texture block matches. Nothing was changed.' }

if ($VerifyOnly) {
    Write-Host ("Check complete: game version {0} is compatible. {1} blocks would be freed, {2} kept original. Nothing was changed." -f $best.Name, ($totalOriginal + $totalZeroed), $totalDifferent) -ForegroundColor Green
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
$data = Join-Path $plugins 'dd2_ntc_data'
Copy-Item -LiteralPath (Join-Path $best.Folder 'batch-01.tsv'), (Join-Path $best.Folder 'gpu-lot.txt') -Destination $data
$expected = (Get-ChildItem -LiteralPath (Join-Path $Payload 'reframework\plugins\dd2_ntc_data') -File).Count + 2
if ((Get-ChildItem -LiteralPath $data -File).Count -ne $expected) { throw 'Copying the data was incomplete. The PAKs were not changed.' }
# Only textures whose blocks are zeroed on this PC are required to come from NTC; the rest can still use the PAK.
$required = @($owners) | Sort-Object
[IO.File]::WriteAllText((Join-Path $data 'gpu-lot-required.txt'), (($required -join "`n") + "`n"))
Write-Host "   $($required.Count) textures will be served from NTC"

foreach ($name in $plan.Keys) {
    $path = Join-Path $GamePath $name
    $list = $plan[$name]
    Step "Zeroing $($list.Count) blocks of $name"
    $file = [IO.File]::Open($path, 'Open', 'ReadWrite', 'None')
    try {
        [NtcPak]::SetSparse($file)
        foreach ($r in $list) { [NtcPak]::Zero($file, [long]$r[0], [long]$r[1]) }
        $file.Flush()
        foreach ($r in $list) {
            if (-not [NtcPak]::IsZero($file, [long]$r[0], [int]$r[1])) { throw "Block not zeroed in ${name}: $($r[0])" }
        }
    } finally { $file.Dispose() }
}

$state = [ordered]@{ installed = (Get-Date).ToString('s'); game_version = $best.Name; exe_version = $exeItem.VersionInfo.FileVersion
                     freed_blocks = $totalOriginal + $totalZeroed; kept_original = $totalDifferent; ntc_textures = $required.Count }
$state | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $GamePath 'dd2-ntc-install.json') -Encoding utf8
Write-Host ("Installation complete (game version {0}): {1} blocks freed, {2} kept original. Start the game as usual." -f $best.Name, ($totalOriginal + $totalZeroed), $totalDifferent) -ForegroundColor Green
Write-Host 'If this mod saved you space, you can support it: https://buymeacoffee.com/saviojeliel' -ForegroundColor Yellow
Write-Host 'If something goes wrong in game, run report.bat and attach the ZIP to an issue.'
Stop-Transcript | Out-Null
