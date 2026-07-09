param(
    [string]$Label = "baseline",
    [int]$DurationSeconds = 120,
    [double]$IntervalSeconds = 1.0,
    [string]$ProcessName = "obs64",
    [string]$PreviewUrl = "http://127.0.0.1:9181/preview.mjpg",
    [string]$HealthUrl = "http://127.0.0.1:9181/health",
    [int]$Clients = 0,
    [string]$OutDir = "measurements"
)

$ErrorActionPreference = "Stop"

function Get-TargetProcess {
    param([string]$Name)

    $processes = @(Get-Process -Name $Name -ErrorAction SilentlyContinue)
    if ($processes.Count -eq 0) {
        throw "Process '$Name' was not found. Start OBS first, or pass -ProcessName."
    }

    if ($processes.Count -eq 1) {
        return $processes[0]
    }

    return $processes | Sort-Object StartTime -Descending | Select-Object -First 1
}

function Get-Percentile {
    param(
        [double[]]$Values,
        [double]$Percentile
    )

    if (-not $Values -or $Values.Count -eq 0) {
        return 0
    }

    $sorted = @($Values | Sort-Object)
    $index = [Math]::Ceiling(($Percentile / 100.0) * $sorted.Count) - 1
    $index = [Math]::Max(0, [Math]::Min($sorted.Count - 1, $index))
    return [Math]::Round($sorted[$index], 3)
}

function Get-Stats {
    param(
        [object[]]$Samples,
        [string]$Property
    )

    $values = @($Samples | ForEach-Object { [double]$_.$Property })
    $measure = $values | Measure-Object -Average -Maximum -Minimum
    return [ordered]@{
        avg = [Math]::Round($measure.Average, 3)
        p95 = Get-Percentile -Values $values -Percentile 95
        max = [Math]::Round($measure.Maximum, 3)
        min = [Math]::Round($measure.Minimum, 3)
    }
}

function Start-MjpegClient {
    param(
        [string]$Url,
        [int]$Duration
    )

    Start-Job -ScriptBlock {
        param($JobUrl, $JobDuration)

        $buffer = New-Object byte[] 65536
        $bytes = 0L
        $errorMessage = $null
        $timer = [Diagnostics.Stopwatch]::StartNew()

        try {
            $request = [Net.HttpWebRequest]::Create($JobUrl)
            $request.Timeout = 10000
            $request.ReadWriteTimeout = 10000
            $request.UserAgent = "obs-lan-preview-measure"

            $response = $request.GetResponse()
            $stream = $response.GetResponseStream()

            while ($timer.Elapsed.TotalSeconds -lt $JobDuration) {
                $read = $stream.Read($buffer, 0, $buffer.Length)
                if ($read -le 0) {
                    break
                }
                $bytes += $read
            }

            $stream.Dispose()
            $response.Dispose()
        } catch {
            $errorMessage = $_.Exception.Message
        }

        [pscustomobject]@{
            bytes = $bytes
            seconds = [Math]::Max(0.001, $timer.Elapsed.TotalSeconds)
            error = $errorMessage
        }
    } -ArgumentList $Url, $Duration
}

function Get-HealthSnapshot {
    param([string]$Url)

    if ([string]::IsNullOrWhiteSpace($Url)) {
        return $null
    }

    try {
        return Invoke-RestMethod -Uri $Url -TimeoutSec 3
    } catch {
        return [pscustomobject]@{
            error = $_.Exception.Message
        }
    }
}

function Get-HealthDelta {
    param(
        [object]$Before,
        [object]$After
    )

    if (-not $Before -or -not $After) {
        return $null
    }

    if ($Before.PSObject.Properties.Name -contains "error" -or $After.PSObject.Properties.Name -contains "error") {
        return $null
    }

    $fields = @(
        "submittedFrames",
        "encodedFrames",
        "droppedFrames",
        "rawBuffersAllocated",
        "rawBuffersReused"
    )

    $delta = [ordered]@{}
    foreach ($field in $fields) {
        if ($Before.PSObject.Properties.Name -contains $field -and $After.PSObject.Properties.Name -contains $field) {
            $delta[$field] = [int64]$After.$field - [int64]$Before.$field
        }
    }

    return $delta
}

if ($DurationSeconds -lt 5) {
    throw "-DurationSeconds must be at least 5."
}

if ($IntervalSeconds -le 0) {
    throw "-IntervalSeconds must be greater than 0."
}

if ($Clients -lt 0) {
    throw "-Clients cannot be negative."
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$outputRoot = Join-Path $repoRoot $OutDir
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

$safeLabel = ($Label -replace "[^A-Za-z0-9_.-]", "-").Trim("-")
if ([string]::IsNullOrWhiteSpace($safeLabel)) {
    $safeLabel = "measurement"
}

$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$csvPath = Join-Path $outputRoot "$timestamp-$safeLabel-samples.csv"
$summaryPath = Join-Path $outputRoot "$timestamp-$safeLabel-summary.json"

$target = Get-TargetProcess -Name $ProcessName
$logicalProcessors = [Environment]::ProcessorCount
$clientJobs = @()
$healthBefore = Get-HealthSnapshot -Url $HealthUrl

Write-Host "Measuring process $($target.ProcessName) pid=$($target.Id) for $DurationSeconds seconds."
if ($Clients -gt 0) {
    Write-Host "Starting $Clients MJPEG client(s): $PreviewUrl"
    for ($i = 0; $i -lt $Clients; $i++) {
        $clientJobs += Start-MjpegClient -Url $PreviewUrl -Duration $DurationSeconds
    }
}

$samples = New-Object System.Collections.Generic.List[object]
$sampleIntervalMs = [Math]::Max(100, [int]($IntervalSeconds * 1000))
$runTimer = [Diagnostics.Stopwatch]::StartNew()

$previousProcess = Get-Process -Id $target.Id
$previousTime = Get-Date
$previousCpuSeconds = $previousProcess.TotalProcessorTime.TotalSeconds

while ($runTimer.Elapsed.TotalSeconds -lt $DurationSeconds) {
    Start-Sleep -Milliseconds $sampleIntervalMs

    $now = Get-Date
    $current = Get-Process -Id $target.Id -ErrorAction Stop
    $elapsedSeconds = [Math]::Max(0.001, ($now - $previousTime).TotalSeconds)
    $cpuDeltaSeconds = $current.TotalProcessorTime.TotalSeconds - $previousCpuSeconds
    $cpuOneCorePct = ($cpuDeltaSeconds / $elapsedSeconds) * 100.0
    $cpuMachinePct = $cpuOneCorePct / $logicalProcessors

    $samples.Add([pscustomobject]@{
        timestamp = $now.ToString("o")
        elapsed_s = [Math]::Round($runTimer.Elapsed.TotalSeconds, 3)
        cpu_one_core_pct = [Math]::Round($cpuOneCorePct, 3)
        cpu_machine_pct = [Math]::Round($cpuMachinePct, 3)
        working_set_mb = [Math]::Round($current.WorkingSet64 / 1MB, 3)
        private_memory_mb = [Math]::Round($current.PrivateMemorySize64 / 1MB, 3)
        threads = $current.Threads.Count
        handles = $current.HandleCount
    })

    $previousTime = $now
    $previousCpuSeconds = $current.TotalProcessorTime.TotalSeconds
}

$clientResults = @()
if ($clientJobs.Count -gt 0) {
    Wait-Job -Job $clientJobs | Out-Null
    $clientResults = @($clientJobs | Receive-Job)
    Remove-Job -Job $clientJobs -Force
}

$healthAfter = Get-HealthSnapshot -Url $HealthUrl

$samples | Export-Csv -NoTypeInformation -Encoding UTF8 -Path $csvPath

$totalClientBytes = 0L
foreach ($result in $clientResults) {
    $totalClientBytes += [int64]$result.bytes
}

$summary = [ordered]@{
    label = $Label
    process_name = $target.ProcessName
    pid = $target.Id
    duration_seconds = $DurationSeconds
    interval_seconds = $IntervalSeconds
    logical_processors = $logicalProcessors
    clients = $Clients
    preview_url = $PreviewUrl
    health_url = $HealthUrl
    samples = $samples.Count
    cpu_one_core_pct = Get-Stats -Samples $samples -Property "cpu_one_core_pct"
    cpu_machine_pct = Get-Stats -Samples $samples -Property "cpu_machine_pct"
    working_set_mb = Get-Stats -Samples $samples -Property "working_set_mb"
    private_memory_mb = Get-Stats -Samples $samples -Property "private_memory_mb"
    threads = Get-Stats -Samples $samples -Property "threads"
    handles = Get-Stats -Samples $samples -Property "handles"
    client_total_mb = [Math]::Round($totalClientBytes / 1MB, 3)
    client_total_mbps = [Math]::Round((($totalClientBytes * 8.0) / [Math]::Max(1, $DurationSeconds)) / 1000000.0, 3)
    client_results = $clientResults
    health_before = $healthBefore
    health_after = $healthAfter
    health_delta = Get-HealthDelta -Before $healthBefore -After $healthAfter
    csv_path = $csvPath
}

$summary | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 -Path $summaryPath

Write-Host ""
Write-Host "Summary:" -ForegroundColor Green
Write-Host "  CPU avg: $($summary.cpu_machine_pct.avg)% of machine, $($summary.cpu_one_core_pct.avg)% of one core"
Write-Host "  CPU p95: $($summary.cpu_machine_pct.p95)% of machine, $($summary.cpu_one_core_pct.p95)% of one core"
Write-Host "  Working set avg: $($summary.working_set_mb.avg) MB"
Write-Host "  Private memory avg: $($summary.private_memory_mb.avg) MB"
if ($Clients -gt 0) {
    Write-Host "  Client receive rate: $($summary.client_total_mbps) Mbps total"
}
if ($summary.health_delta) {
    Write-Host "  Encoded frames delta: $($summary.health_delta.encodedFrames)"
    Write-Host "  Dropped frames delta: $($summary.health_delta.droppedFrames)"
}
Write-Host "  Samples: $csvPath"
Write-Host "  Summary: $summaryPath"
