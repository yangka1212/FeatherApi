[CmdletBinding()]
param(
    [ValidateNotNullOrEmpty()][string]$Url,
    [ValidateRange(100, 599)][int]$ExpectedStatus,
    [ValidateNotNullOrEmpty()][string]$ExpectedBodyFragment,
    [ValidateNotNullOrEmpty()][string]$LiveBaseUrl = 'https://httpbin.org',
    [ValidateNotNullOrEmpty()][string]$OpenApiUrl = 'https://raw.githubusercontent.com/openai/openai-openapi/main/openapi.json',
    [switch]$SkipOpenApiImport,
    [string]$ErrorUrl,
    [ValidateRange(100, 599)][int]$ErrorStatus,
    [ValidateNotNullOrEmpty()][string]$ErrorBodyFragment,
    [switch]$SkipErrorResponse,
    [ValidateNotNullOrEmpty()][string]$PythonCommand
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$buildScript = Join-Path $PSScriptRoot 'build.ps1'
$e2eScript = Join-Path $PSScriptRoot 'real_e2e.py'
$coreScript = Join-Path $PSScriptRoot 'core_e2e.py'
$visualScript = Join-Path $PSScriptRoot 'visual_e2e.py'
$exePath = Join-Path $projectRoot 'build/release/FeatherApi.exe'
$liveExePath = Join-Path $projectRoot 'build/release/FeatherApiLiveHttpTests.exe'

if ($SkipErrorResponse -and ($PSBoundParameters.ContainsKey('ErrorUrl') -or
                             $PSBoundParameters.ContainsKey('ErrorStatus') -or
                             $PSBoundParameters.ContainsKey('ErrorBodyFragment'))) {
    throw '-SkipErrorResponse cannot be combined with error response options.'
}

if ($PSBoundParameters.ContainsKey('PythonCommand')) {
    $python = Get-Command -Name $PythonCommand -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    $launcherArgs = @()
} else {
    $python = Get-Command -Name python -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    $launcherArgs = @()
    if (-not $python) {
        $python = Get-Command -Name py -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        $launcherArgs = @('-3')
    }
}
if (-not $python) { throw 'Python 3.10+ is required. Install it or pass -PythonCommand with its executable path.' }
if (-not (Test-Path -LiteralPath $e2eScript -PathType Leaf)) {
    throw "End-to-end test script was not found: $e2eScript"
}
foreach ($script in @($coreScript, $visualScript)) {
    if (-not (Test-Path -LiteralPath $script -PathType Leaf)) {
        throw "End-to-end test script was not found: $script"
    }
}
& $python.Source @launcherArgs -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)'
if ($LASTEXITCODE -ne 0) { throw 'Python 3.10+ is required for scripts/real_e2e.py.' }

Write-Host 'Running Release build and registered CTest checks...'
& $buildScript -Action Test -Configuration Release
if (-not $?) { throw 'Release build or CTest checks failed.' }
if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    throw "Production executable was not built: $exePath"
}
if (-not (Test-Path -LiteralPath $liveExePath -PathType Leaf)) {
    throw "Live HTTP test executable was not built: $liveExePath"
}

Write-Host "Running live WinHTTP tests against $LiveBaseUrl..."
$liveArgs = @('--base-url', $LiveBaseUrl)
if ($SkipOpenApiImport) { $liveArgs += '--skip-openapi' }
else { $liveArgs += @('--openapi-url', $OpenApiUrl) }
& $liveExePath @liveArgs
$liveExitCode = $LASTEXITCODE
if ($liveExitCode -ne 0) {
    throw "Live HTTP tests failed with exit code $liveExitCode."
}

$e2eArgs = @($e2eScript, '--exe', $exePath)
if ($PSBoundParameters.ContainsKey('Url')) { $e2eArgs += @('--url', $Url) }
if ($PSBoundParameters.ContainsKey('ExpectedStatus')) {
    $e2eArgs += @('--expected-status', [string]$ExpectedStatus)
}
if ($PSBoundParameters.ContainsKey('ExpectedBodyFragment')) {
    $e2eArgs += @('--expected-body-fragment', $ExpectedBodyFragment)
}
if ($PSBoundParameters.ContainsKey('ErrorUrl')) {
    if ($ErrorUrl.Length -eq 0) { $e2eArgs += '--error-url=' }
    else { $e2eArgs += @('--error-url', $ErrorUrl) }
}
if ($PSBoundParameters.ContainsKey('ErrorStatus')) { $e2eArgs += @('--error-status', [string]$ErrorStatus) }
if ($PSBoundParameters.ContainsKey('ErrorBodyFragment')) {
    $e2eArgs += @('--error-body-fragment', $ErrorBodyFragment)
}
if ($SkipErrorResponse) { $e2eArgs += '--skip-error' }

Write-Host 'Running the production executable against a live HTTP service...'
& $python.Source @launcherArgs @e2eArgs
$e2eExitCode = $LASTEXITCODE
if ($e2eExitCode -ne 0) {
    throw "Production end-to-end test failed with exit code $e2eExitCode."
}

$coreArgs = @($coreScript, '--exe', $exePath)
$coreArgs += @('--live-base-url', $LiveBaseUrl)
if (-not $SkipOpenApiImport) { $coreArgs += @('--openapi-url', $OpenApiUrl) }
Write-Host 'Running production catalog, editor, and OpenAPI workflows...'
& $python.Source @launcherArgs @coreArgs
if ($LASTEXITCODE -ne 0) { throw "Production core workflow test failed with exit code $LASTEXITCODE." }

Write-Host 'Capturing and checking production UI styling...'
$visualUrl = $LiveBaseUrl.TrimEnd('/') + '/get'
& $python.Source @launcherArgs $visualScript --exe $exePath --live-url $visualUrl
if ($LASTEXITCODE -ne 0) { throw "Production visual test failed with exit code $LASTEXITCODE." }
Write-Host 'All release, live HTTP, production workflow, and visual tests passed.'
