$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("obs-lan-preview-release-notes-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $tempRoot | Out-Null

try {
    Push-Location $tempRoot
    git init -q
    git config user.email test@example.invalid
    git config user.name "Release Notes Test"
    "initial" | Set-Content initial.txt
    git add initial.txt
    git commit -qm "Initial commit"
    git tag v0.1.0
    "feature" | Set-Content feature.txt
    git add feature.txt
    git commit -qm "feat: add linked change"
    "release" | Set-Content release.txt
    git add release.txt
    git commit -qm "Release v0.1.1"
    git tag v0.1.1
    git remote add origin https://github.com/example/preview.git

    $output = Join-Path $tempRoot release-notes.md
    & (Join-Path $repoRoot "scripts\generate-release-notes.ps1") -Tag v0.1.1 -Repository example/preview -OutputPath $output
    $notes = Get-Content -Raw $output
    if ($notes -notmatch '\[feat: add linked change\]\(https://github\.com/example/preview/commit/[0-9a-f]+\)') {
        throw "Expected linked feature commit was not generated."
    }
    if ($notes -match 'Release v0\.1\.1') {
        throw "Release commit was not excluded."
    }
    Write-Host "Release notes test passed." -ForegroundColor Green
} finally {
    Pop-Location -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $tempRoot -Force -Recurse -ErrorAction SilentlyContinue
}
