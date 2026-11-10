# perf_sample.ps1 - writes one CSV row of system / process load every -IntervalSec seconds.
# Started by tools/perf_test.py; stops when -StopFile exists or after -MaxSeconds (0 = no limit).
#
# The counters are the usual Processor / Memory / PhysicalDisk / GPU Engine / Process ones, read through the
# Win32_PerfFormattedData_* WMI classes instead of Get-Counter: Get-Counter needs the counter NAMES in the
# language of Windows (they are different on a Russian system), the WMI classes are the same everywhere.
#
# GPU columns: the sum of all "engtype_<name>" instances, capped at 100. Approximate: the engines of one
# type on several GPUs / processes are added together.
# *_cpu columns: % of the whole CPU (the per-core value of a process divided by the number of logical cores).
param(
    [double]$IntervalSec = 2,
    [Parameter(Mandatory = $true)][string]$OutFile,
    [Parameter(Mandatory = $true)][string]$StopFile,
    [double]$MaxSeconds = 0
)

$ErrorActionPreference = 'SilentlyContinue'
$inv = [System.Globalization.CultureInfo]::InvariantCulture
function F([double]$v) { return $v.ToString('0.##', $inv) }

$os = Get-CimInstance Win32_OperatingSystem
$totalMb = [double]$os.TotalVisibleMemorySize / 1024
$cores = [int](Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
if ($cores -lt 1) { $cores = 1 }

$header = 'elapsed_s,cpu_total,mem_used_mb,disk_read_mb,disk_write_mb,gpu_3d,gpu_decode,gpu_encode,gpu_copy,homrec_cpu,ffmpeg_cpu,app_cpu,proc_mem_mb'
Set-Content -Path $OutFile -Value $header -Encoding ASCII

function GpuSum($engines, [string]$type) {
    $sum = 0.0
    foreach ($e in $engines) { if ($e.Name -like "*engtype_$type") { $sum += [double]$e.UtilizationPercentage } }
    return [Math]::Min(100.0, $sum)
}

$start = Get-Date
while ($true) {
    $tickStart = Get-Date
    $elapsed = ($tickStart - $start).TotalSeconds
    if ((Test-Path $StopFile) -or ($MaxSeconds -gt 0 -and $elapsed -ge $MaxSeconds)) { break }

    $cpu  = Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'"
    $mem  = Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory
    $disk = Get-CimInstance Win32_PerfFormattedData_PerfDisk_PhysicalDisk -Filter "Name='_Total'"
    $gpu  = Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUEngine
    $proc = Get-CimInstance Win32_PerfFormattedData_PerfProc_Process

    $homrec = 0.0; $ffmpeg = 0.0; $app = 0.0; $procMem = 0.0
    foreach ($p in $proc) {
        if ($p.Name -match '^(homrec|ffmpeg|hr)(#\d+)?$') {
            $c = [double]$p.PercentProcessorTime / $cores
            switch ($Matches[1]) { 'homrec' { $homrec += $c } 'ffmpeg' { $ffmpeg += $c } 'hr' { $app += $c } }
            $procMem += [double]$p.WorkingSetPrivate / 1MB
        }
    }

    $row = @(
        (F $elapsed),
        (F ([double]$cpu.PercentProcessorTime)),
        (F ($totalMb - [double]$mem.AvailableMBytes)),
        (F ([double]$disk.DiskReadBytesPersec / 1MB)),
        (F ([double]$disk.DiskWriteBytesPersec / 1MB)),
        (F (GpuSum $gpu '3D')),
        (F (GpuSum $gpu 'VideoDecode')),
        (F (GpuSum $gpu 'VideoEncode')),
        (F (GpuSum $gpu 'Copy')),
        (F $homrec), (F $ffmpeg), (F $app), (F $procMem)
    ) -join ','
    Add-Content -Path $OutFile -Value $row -Encoding ASCII

    # Sleep only what is left of the interval: the queries above take a while themselves.
    $left = $IntervalSec - ((Get-Date) - $tickStart).TotalSeconds
    if ($left -gt 0) { Start-Sleep -Milliseconds ([int]($left * 1000)) }
}
