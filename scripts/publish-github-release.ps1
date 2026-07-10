param(
    [string]$Tag = "v$((Get-Content -Raw (Join-Path $PSScriptRoot '..\VERSION')).Trim())",
    [switch]$Draft
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$installerZip = Join-Path $repoRoot "release\packages\obs-preview-plugin.windows-x64-installer.zip"
$portableZip = Join-Path $repoRoot "release\packages\obs-preview-plugin.windows-x64-portable.zip"
$notesPath = Join-Path $repoRoot "release\release-notes.md"

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw "GitHub CLI (gh) is required. Install it with: winget install --id GitHub.cli --exact"
}

if (-not (Test-Path $installerZip) -or -not (Test-Path $portableZip)) {
    throw "Release zip files are missing. Run scripts/package-windows.ps1 first."
}

& (Join-Path $PSScriptRoot "generate-release-notes.ps1") -Tag $Tag -OutputPath $notesPath
$args = @("release", "create", $Tag, $installerZip, $portableZip, "--title", $Tag, "--notes-file", $notesPath)
if ($Draft) {
    $args += "--draft"
}

gh @args
