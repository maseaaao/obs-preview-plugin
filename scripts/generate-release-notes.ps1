param(
    [string]$Tag = "v$((Get-Content -Raw (Join-Path $PSScriptRoot '..\VERSION')).Trim())",
    [string]$Repository,
    [string]$OutputPath = (Join-Path $PSScriptRoot "..\release-notes.md")
)

$ErrorActionPreference = "Stop"

function Invoke-GitText {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)

    $text = & git @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "git $($Arguments -join ' ') failed."
    }
    return @($text)
}

function Escape-MarkdownLinkText {
    param([string]$Text)

    return $Text.Replace('\', '\\').Replace('[', '\[').Replace(']', '\]')
}

if (-not $Repository) {
    $remote = (Invoke-GitText remote get-url origin | Select-Object -First 1).Trim()
    if ($remote -match 'github\.com[:/]([^/]+/[^/.]+)(?:\.git)?$') {
        $Repository = $Matches[1]
    } else {
        throw "Unable to infer the GitHub repository. Pass -Repository owner/name."
    }
}

Invoke-GitText rev-parse --verify "$Tag^{commit}" | Out-Null
$tags = Invoke-GitText tag --merged $Tag --sort=-version:refname
$previousTag = $tags | Where-Object { $_ -and $_ -ne $Tag } | Select-Object -First 1
$range = if ($previousTag) { "$previousTag..$Tag" } else { $Tag }

$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add("## Changes")
$commits = Invoke-GitText log $range --no-merges --format=%H%x09%s
foreach ($commit in $commits) {
    $parts = $commit -split "`t", 2
    if ($parts.Count -ne 2 -or $parts[1] -match '^Release v\d+\.\d+\.\d+$') {
        continue
    }
    $lines.Add("- [$((Escape-MarkdownLinkText $parts[1]))](https://github.com/$Repository/commit/$($parts[0]))")
}

if ($lines.Count -eq 1) {
    $lines.Add("- No non-merge commits since the previous release.")
}

$directory = Split-Path -Parent $OutputPath
if ($directory) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}
[System.IO.File]::WriteAllLines((Resolve-Path -LiteralPath $directory).Path + [IO.Path]::DirectorySeparatorChar + (Split-Path -Leaf $OutputPath), $lines)
Write-Host "Release notes written to $OutputPath for $range" -ForegroundColor Green
