param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("patch", "minor", "major")]
    [string]$Part,

    [switch]$NoCommit,
    [switch]$NoTag
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$versionPath = Join-Path $repoRoot "VERSION"
$cmakePath = Join-Path $repoRoot "CMakeLists.txt"
$installerPath = Join-Path $repoRoot "installer\obs-lan-preview.iss"

if (-not (Test-Path $versionPath)) {
    throw "VERSION file not found."
}

$current = (Get-Content -Raw $versionPath).Trim()
if ($current -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
    throw "VERSION must be SemVer MAJOR.MINOR.PATCH. Current value: $current"
}

$major = [int]$Matches[1]
$minor = [int]$Matches[2]
$patch = [int]$Matches[3]

switch ($Part) {
    "patch" { $patch += 1 }
    "minor" { $minor += 1; $patch = 0 }
    "major" { $major += 1; $minor = 0; $patch = 0 }
}

$next = "$major.$minor.$patch"
Set-Content -Path $versionPath -Value $next

$cmake = Get-Content -Raw $cmakePath
$cmake = $cmake -replace 'project\(obs-lan-preview VERSION \d+\.\d+\.\d+ LANGUAGES CXX\)', "project(obs-lan-preview VERSION $next LANGUAGES CXX)"
Set-Content -Path $cmakePath -Value $cmake

$installer = Get-Content -Raw $installerPath
$installer = $installer -replace '#define PluginVersion "\d+\.\d+\.\d+"', "#define PluginVersion `"$next`""
Set-Content -Path $installerPath -Value $installer

Write-Host "Version bumped: $current -> $next" -ForegroundColor Green

if (-not $NoCommit) {
    git add VERSION CMakeLists.txt installer/obs-lan-preview.iss
    git commit -m "Release v$next"
}

if (-not $NoTag) {
    git tag "v$next"
}

Write-Host "Next version: $next"
