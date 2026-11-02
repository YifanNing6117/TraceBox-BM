[CmdletBinding()]
param(
    [string]$PythonExe = 'python',
    [ValidateRange(1024, 65535)]
    [int]$ListenPort = 8765,
    [switch]$OpenBrowser
)

$ErrorActionPreference = 'Stop'
$serverPath = Join-Path $PSScriptRoot 'server\src\server.js'

if (-not (Test-Path -LiteralPath $PythonExe -PathType Leaf)) {
    $pythonCommand = Get-Command $PythonExe -ErrorAction SilentlyContinue
    if ($null -eq $pythonCommand) {
        throw "Python executable not found: $PythonExe"
    }
    $PythonExe = $pythonCommand.Source
}

$env:TRACEBOX_PYTHON = $PythonExe
$url = "http://127.0.0.1:$ListenPort"
Write-Host "TraceBox console: $url"
Write-Host "Python:          $PythonExe"

if ($OpenBrowser) {
    Start-Process $url
}

& node $serverPath --port $ListenPort
exit $LASTEXITCODE
