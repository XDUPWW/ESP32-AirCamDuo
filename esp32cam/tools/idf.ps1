param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Rest
)

$ErrorActionPreference = "Stop"

# The wrapper is usually launched from Git Bash, which leaks MSYS/MinGW settings
# into the environment; idf.py warns about them and the MinGW gcc breaks the build.
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:MSYS2_PATH_TYPE -ErrorAction SilentlyContinue

# idf.py refuses to run when a MSYS/MinGW gcc is visible in PATH,
# so rebuild PATH from the machine/user values that a plain shell would see.
$env:PATH = [Environment]::GetEnvironmentVariable('PATH', 'Machine') + ';' +
            [Environment]::GetEnvironmentVariable('PATH', 'User')

# Chinese Windows defaults to GBK, which breaks the IDF python tooling.
$env:PYTHONUTF8 = "1"

# Activation scripts written by the Espressif installation manager (EIM).
$candidates = @(
    'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1',
    'D:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
)

$activated = $false
foreach ($candidate in $candidates) {
    if (Test-Path $candidate) {
        . $candidate *> $null
        $activated = $true
        break
    }
}

if (-not $activated) {
    throw "No ESP-IDF activation profile found; check the EIM installation."
}

# Another board (an ESP32-C5 on COM8) is attached to this machine, and esptool
# happily picks whatever it finds first, so the camera port is pinned unless the
# caller asks for a different one. Override with $env:ESP32CAM_PORT.
if ($null -eq $Rest) { $Rest = @() }
$hasPort = $false
foreach ($arg in $Rest) {
    if ($arg -eq '-p' -or $arg -eq '--port') { $hasPort = $true }
}
if (-not $hasPort) {
    $port = $env:ESP32CAM_PORT
    if (-not $port) { $port = 'COM5' }
    $Rest = @('-p', $port) + $Rest
}

Set-Location (Join-Path $PSScriptRoot "..")
& idf.py @Rest
exit $LASTEXITCODE
