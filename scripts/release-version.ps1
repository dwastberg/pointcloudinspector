param(
    [string]$ExpectedVersion = '',
    [string]$VersionFile = ''
)

$ErrorActionPreference = 'Stop'
$source = Get-Content -LiteralPath CMakeLists.txt -Raw
$match = [regex]::Match($source, '(?ms)^\s*project\s*\(\s*pcinspector\b[^)]*?\bVERSION\s+([^\s)]+)')
$version = $match.Groups[1].Value
if ($version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
    throw "Invalid CMake project version: $version"
}
if ($ExpectedVersion -and $ExpectedVersion -ne $version) {
    throw "Expected version $ExpectedVersion, found $version"
}
if ($VersionFile -and (Get-Content -LiteralPath $VersionFile -Raw).Trim() -ne $version) {
    throw "Configured version differs from $version"
}
Write-Output $version
