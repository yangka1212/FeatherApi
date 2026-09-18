[CmdletBinding()]
param(
    [ValidateSet('Build', 'Test', 'Package', 'Clean')]
    [string]$Action = 'Build',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [ValidateSet('all', 'FeatherApi', 'FeatherApiSelfTests', 'FeatherApiUiTests')]
    [string]$Target = 'all',
    [string]$OutputName = 'FeatherApi.exe',
    [switch]$Rebuild
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$preset = $Configuration.ToLowerInvariant()

if ($Action -eq 'Package') {
    if ($Configuration -ne 'Release') { throw 'Packaging requires Release configuration.' }
    if ([IO.Path]::GetFileName($OutputName) -ne $OutputName -or
        $OutputName.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0 -or
        -not $OutputName.EndsWith('.exe', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'OutputName must be an .exe filename without a directory.'
    }
}

# Discover the toolchain instead of requiring a particular VS edition or version.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio C++ build tools and the Windows SDK.' }
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'No Visual Studio installation with x64 C++ tools was found.' }
$vcvars = Join-Path $vsPath 'VC/Auxiliary/Build/vcvars64.bat'
$environment = & $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the Visual Studio build environment.' }
$toolchainPath = $null
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') {
        if ($matches[1] -ieq 'Path') {
            if (-not $toolchainPath) { $toolchainPath = $matches[2] }
            continue
        }
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
# Use a stable diagnostic language for Ninja's include dependency scanner.
$env:VSLANG = '1033'
# Some host processes supply both Path and PATH; keep the toolchain's first entry.
[Environment]::SetEnvironmentVariable('Path', $null, 'Process')
[Environment]::SetEnvironmentVariable('PATH', $null, 'Process')
$env:Path = $toolchainPath
$cmakeBundle = Join-Path $vsPath 'Common7/IDE/CommonExtensions/Microsoft/CMake'
if (Test-Path -LiteralPath "$cmakeBundle/CMake/bin/cmake.exe") {
    $env:Path = "$cmakeBundle/CMake/bin;$cmakeBundle/Ninja;$env:Path"
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue) -or
    -not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    throw 'Install CMake and Ninja, or the Visual Studio C++ CMake tools component.'
}

function Invoke-BuildTool([string]$Tool, [string[]]$Arguments) {
    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Tool failed with exit code $LASTEXITCODE." }
}

Push-Location $projectRoot
try {
    Invoke-BuildTool cmake @('--preset', $preset)
    if ($Action -eq 'Clean') {
        Invoke-BuildTool cmake @('--build', '--preset', $preset, '--target', 'clean')
    } else {
        $buildTarget = $Target
        if ($Action -eq 'Test') { $buildTarget = 'all' }
        if ($Action -eq 'Package') { $buildTarget = 'FeatherApi' }
        $buildArguments = @('--build', '--preset', $preset, '--target', $buildTarget)
        if ($Rebuild) { $buildArguments += '--clean-first' }
        Invoke-BuildTool cmake $buildArguments
        if ($Action -eq 'Test') { Invoke-BuildTool ctest @('--preset', $preset) }
        if ($Action -eq 'Package') {
            $destination = Join-Path $projectRoot 'dist'
            New-Item -ItemType Directory -Path $destination -Force | Out-Null
            Copy-Item -LiteralPath "$projectRoot/build/$preset/FeatherApi.exe" -Destination (Join-Path $destination $OutputName) -Force
            Write-Host "Created: $destination/$OutputName"
        }
    }
} finally {
    Pop-Location
}
