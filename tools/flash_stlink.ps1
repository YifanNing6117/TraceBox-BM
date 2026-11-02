[CmdletBinding()]
param(
    [string]$HexPath,
    [string]$ProgrammerCli,
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($HexPath)) {
    $HexPath = Join-Path $repoRoot 'firmware\releases\TraceBox_Final_v1.0.0.hex'
}

if (-not (Test-Path -LiteralPath $HexPath -PathType Leaf)) {
    throw "HEX file not found: $HexPath. Run tools\build_keil.ps1 first."
}

if ([string]::IsNullOrWhiteSpace($ProgrammerCli)) {
    $command = Get-Command 'STM32_Programmer_CLI.exe' -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        $ProgrammerCli = $command.Source
    }
}

if ([string]::IsNullOrWhiteSpace($ProgrammerCli)) {
    $knownCandidates = @(
        'C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe',
        'C:\Program Files (x86)\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe'
    )

    foreach ($candidate in $knownCandidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $ProgrammerCli = $candidate
            break
        }
    }
}

if ([string]::IsNullOrWhiteSpace($ProgrammerCli) -or
    -not (Test-Path -LiteralPath $ProgrammerCli -PathType Leaf)) {
    throw 'STM32_Programmer_CLI.exe not found. Pass it with -ProgrammerCli.'
}

Write-Host "Programmer: $ProgrammerCli"
Write-Host "Image:      $HexPath"

if ($CheckOnly) {
    $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $HexPath
    Write-Host "SHA256:     $($hash.Hash)"
    Write-Host 'Flash prerequisites PASS (probe connection not attempted)'
    exit 0
}

Write-Host 'Connecting by SWD, programming, verifying and resetting...'

& $ProgrammerCli -c port=SWD freq=4000 -w $HexPath -v -rst
if ($LASTEXITCODE -ne 0) {
    throw "STM32CubeProgrammer failed with exit code $LASTEXITCODE."
}

Write-Host 'Flash PASS'