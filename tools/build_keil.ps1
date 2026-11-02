[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$KeilExe,
    [ValidatePattern('^[A-Za-z0-9_.-]+\.hex$')]
    [string]$ReleaseName = 'TraceBox_Final_v1.0.0.hex'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$projectDir = Join-Path $repoRoot 'firmware\TraceBox_F407\MDK-ARM'
$projectFile = Join-Path $projectDir 'TraceBox_F407.uvprojx'
$buildLog = Join-Path $projectDir 'build.log'
$objectsDir = Join-Path $projectDir 'Objects'
$builtHex = Join-Path $objectsDir 'TraceBox_Bringup.hex'
$releaseHex = Join-Path $repoRoot ('firmware\releases\' + $ReleaseName)

if (-not (Test-Path -LiteralPath $KeilExe -PathType Leaf)) {
    throw "Keil executable not found: $KeilExe"
}

if (-not (Test-Path -LiteralPath $projectFile -PathType Leaf)) {
    throw "Keil project not found: $projectFile"
}

$activeKeil = Get-Process 'UV4' -ErrorAction SilentlyContinue
if ($null -ne $activeKeil) {
    throw 'Keil UV4 is already running. Close it before the scripted clean build.'
}

$projectDirFull = [IO.Path]::GetFullPath($projectDir).TrimEnd('\') + '\'
$objectsDirFull = [IO.Path]::GetFullPath($objectsDir)
if (-not $objectsDirFull.StartsWith($projectDirFull, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to clean unexpected output path: $objectsDirFull"
}

if (Test-Path -LiteralPath $objectsDirFull) {
    Remove-Item -LiteralPath $objectsDirFull -Recurse -Force
}
if (Test-Path -LiteralPath $buildLog) {
    Remove-Item -LiteralPath $buildLog -Force
}

Push-Location $projectDir
try {
    # PowerShell intentionally does not block on this GUI-subsystem executable.
    # We serialize the invocation ourselves by polling the fresh build log below.
    # -b is a clean build here because Objects was removed above. This Keil
    # version accepts -j0 for its command-line worker; -j1 opens the GUI only.
    Start-Process -FilePath $KeilExe -ArgumentList @('-b', 'TraceBox_F407.uvprojx', '-j0', '-o', 'build.log') -WorkingDirectory $projectDir -WindowStyle Hidden
} finally {
    Pop-Location
}

$spawnedKeil = Get-Process 'UV4' -ErrorAction SilentlyContinue |
    Sort-Object StartTime -Descending |
    Select-Object -First 1

$deadline = [DateTime]::UtcNow.AddSeconds(60)
$buildFinished = $false
while ([DateTime]::UtcNow -lt $deadline) {
    if (Test-Path -LiteralPath $buildLog -PathType Leaf) {
        $currentLog = Get-Content -Raw -LiteralPath $buildLog
        if (($currentLog -match '\d+ Error\(s\), \d+ Warning\(s\)') -or
            ($currentLog -match 'Target not created')) {
            $buildFinished = $true
            break
        }
    }

    Start-Sleep -Milliseconds 250
}

if (-not $buildFinished) {
    if (($null -ne $spawnedKeil) -and
        ($null -ne (Get-Process -Id $spawnedKeil.Id -ErrorAction SilentlyContinue))) {
        Stop-Process -Id $spawnedKeil.Id -Force
    }
    throw 'Keil build timed out or exited without a final build summary.'
}

if ($null -ne $spawnedKeil) {
    $exitDeadline = [DateTime]::UtcNow.AddSeconds(5)
    while (($null -ne (Get-Process -Id $spawnedKeil.Id -ErrorAction SilentlyContinue)) -and
           ([DateTime]::UtcNow -lt $exitDeadline)) {
        Start-Sleep -Milliseconds 100
    }
    if ($null -ne (Get-Process -Id $spawnedKeil.Id -ErrorAction SilentlyContinue)) {
        Stop-Process -Id $spawnedKeil.Id -Force
    }
}

if (-not (Test-Path -LiteralPath $buildLog -PathType Leaf)) {
    throw 'Keil did not produce build.log.'
}

$logText = Get-Content -Raw -LiteralPath $buildLog
if ($logText -notmatch '0 Error\(s\), 0 Warning\(s\)') {
    Write-Host $logText
    throw 'Keil build did not finish with 0 errors and 0 warnings.'
}

if (-not (Test-Path -LiteralPath $builtHex -PathType Leaf)) {
    throw "Build passed but HEX was not found: $builtHex"
}

New-Item -ItemType Directory -Path (Split-Path -Parent $releaseHex) -Force | Out-Null
Copy-Item -LiteralPath $builtHex -Destination $releaseHex -Force
$hash = Get-FileHash -Algorithm SHA256 -LiteralPath $releaseHex

Write-Host 'Build PASS: 0 errors, 0 warnings'
Write-Host "Release: $releaseHex"
Write-Host "SHA256:  $($hash.Hash)"
