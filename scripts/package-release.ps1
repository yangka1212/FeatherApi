[CmdletBinding()]
param(
    [string]$ExePath = (Join-Path $PSScriptRoot '../build/release/FeatherApi.exe'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../build/packages'),
    [string]$ReleaseTag,
    [switch]$RequireSignature
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$sourceExe = Get-Item -LiteralPath $ExePath
$metadata = $sourceExe.VersionInfo
if ($metadata.ProductName -ne 'FeatherApi' -or
    [string]::IsNullOrWhiteSpace($metadata.CompanyName) -or
    [string]::IsNullOrWhiteSpace($metadata.LegalCopyright) -or
    [string]::IsNullOrWhiteSpace($metadata.ProductVersion) -or
    $metadata.FileVersion -ne $metadata.ProductVersion -or $metadata.IsDebug) {
    throw 'Release EXE must have matching product/file versions, FeatherApi product metadata, and no debug flag.'
}

$version = [version]$metadata.ProductVersion
$numericVersion = '{0}.{1}.{2}' -f $version.Major, $version.Minor, $version.Build
if ($version.Revision -gt 0) { $numericVersion += '.' + $version.Revision }
$expectedTag = 'v' + $numericVersion
if ($ReleaseTag -and $ReleaseTag -cne $expectedTag) {
    throw "Release tag $ReleaseTag does not match EXE version $expectedTag. Update resources.rc before tagging."
}
$ReleaseTag = $expectedTag

$signature = Get-AuthenticodeSignature -LiteralPath $sourceExe.FullName
if ($RequireSignature) {
    if ($signature.Status -ne 'Valid' -or -not $signature.SignerCertificate -or
        -not $signature.TimeStamperCertificate) {
        throw "A valid, timestamped Authenticode signature is required; status: $($signature.Status)."
    }
}

$license = Join-Path $projectRoot 'LICENSE'
if (-not (Test-Path -LiteralPath $license -PathType Leaf)) { throw 'LICENSE is missing.' }
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null
$baseName = "FeatherApi-$ReleaseTag-win-x64"
if (-not $RequireSignature) { $baseName += '-unsigned' }
$publishedExe = Join-Path $output ($baseName + '.exe')
Copy-Item -LiteralPath $sourceExe.FullName -Destination $publishedExe -Force

# Create an exact two-file ZIP; no workspace data or stale build files enter it.
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipPath = Join-Path $output ($baseName + '.zip')
$stream = [IO.File]::Open($zipPath, [IO.FileMode]::Create)
try {
    $zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $true)
    try {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $publishedExe, 'FeatherApi.exe') | Out-Null
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $license, 'LICENSE') | Out-Null
    } finally { $zip.Dispose() }
} finally { $stream.Dispose() }

$checksumPath = Join-Path $output ($baseName + '-SHA256SUMS.txt')
$checksums = foreach ($artifact in @($publishedExe, $zipPath)) {
    $hash = Get-FileHash -LiteralPath $artifact -Algorithm SHA256
    '{0}  {1}' -f $hash.Hash.ToLowerInvariant(), [IO.Path]::GetFileName($artifact)
}
$checksums | Set-Content -LiteralPath $checksumPath -Encoding ASCII
Write-Host "Packaged $ReleaseTag; Authenticode status: $($signature.Status)"
Write-Host "Artifacts: $output"
