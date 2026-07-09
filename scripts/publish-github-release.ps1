param(
    [string]$Tag = "v$((Get-Content -Raw (Join-Path $PSScriptRoot '..\VERSION')).Trim())",
    [switch]$Draft
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$installerZip = Join-Path $repoRoot "release\packages\obs-preview-plugin.windows-x64-installer.zip"
$portableZip = Join-Path $repoRoot "release\packages\obs-preview-plugin.windows-x64-portable.zip"

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw "GitHub CLI (gh) is required. Install it with: winget install --id GitHub.cli --exact"
}

if (-not (Test-Path $installerZip) -or -not (Test-Path $portableZip)) {
    throw "Release zip files are missing. Run scripts/package-windows.ps1 first."
}

$args = @("release", "create", $Tag, $installerZip, $portableZip, "--title", $Tag, "--notes", "Windows x64 release assets.")
if ($Draft) {
    $args += "--draft"
}

gh @args
