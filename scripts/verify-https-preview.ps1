param(
    [Parameter(Mandatory = $true)]
    [string]$PreviewUrl,

    [Parameter(Mandatory = $true)]
    [string]$CaCertificate
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $CaCertificate)) {
    throw "CA certificate was not found: $CaCertificate"
}

$uri = [Uri]$PreviewUrl
if ($uri.Scheme -ne "https") {
    throw "PreviewUrl must use HTTPS."
}

$healthUrl = "https://$($uri.Host):$($uri.Port)/health"
Write-Host "Checking trusted TLS connection to $healthUrl" -ForegroundColor Cyan
$response = & curl.exe --fail --silent --show-error --cacert $CaCertificate $healthUrl
if ($LASTEXITCODE -ne 0) {
    throw "TLS validation or /health request failed. Confirm the exported CA and URL are from the running plugin."
}

$health = $response | ConvertFrom-Json
if (-not $health.running) {
    throw "The HTTPS endpoint responded, but preview is not running."
}

Write-Host "HTTPS certificate and health endpoint verified." -ForegroundColor Green
Write-Host $response
