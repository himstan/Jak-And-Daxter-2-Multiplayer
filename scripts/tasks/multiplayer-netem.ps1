param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("Start", "Stop", "Status")]
  [string]$Action,

  [Parameter(Mandatory = $true)]
  [string]$ExecutablePath,

  [Parameter(Mandatory = $true)]
  [string]$PidPath,

  [Parameter(Mandatory = $true)]
  [string]$LogPath,

  [ValidateSet("lan", "wifi", "4g", "poor-4g", "stress")]
  [string]$Profile = "wifi",

  [string]$Seed = "1",
  [int]$ListenPort = 26212,
  [string]$Target = "127.0.0.1:26210"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-AbsolutePath([string]$Path) {
  if ([System.IO.Path]::IsPathRooted($Path)) {
    return [System.IO.Path]::GetFullPath($Path)
  }
  return [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Path))
}

function Read-PidFile([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    return $null
  }
  $contents = (Get-Content -LiteralPath $Path -Raw).Trim()
  $processId = 0
  if (-not [int]::TryParse($contents, [Globalization.NumberStyles]::Integer,
      [Globalization.CultureInfo]::InvariantCulture, [ref]$processId) -or $processId -le 0) {
    throw "The netem PID file is invalid: $Path"
  }
  return $processId
}

function Get-ExpectedProcess([int]$ProcessId, [string]$ExpectedPath) {
  try {
    $process = Get-Process -Id $ProcessId -ErrorAction Stop
    $processPath = $process.Path
    if ([string]::IsNullOrWhiteSpace($processPath)) {
      throw "Could not read the executable path for PID $ProcessId"
    }
    if ([string]::Compare((Resolve-AbsolutePath $processPath), $ExpectedPath, $true) -ne 0) {
      throw "PID $ProcessId belongs to $processPath, not $ExpectedPath"
    }
    return $process
  } catch [System.Management.Automation.ItemNotFoundException] {
    return $null
  } catch [Microsoft.PowerShell.Commands.ProcessCommandException] {
    return $null
  }
}

$resolvedExecutable = Resolve-AbsolutePath $ExecutablePath
$resolvedPidPath = Resolve-AbsolutePath $PidPath
$resolvedLogPath = Resolve-AbsolutePath $LogPath

switch ($Action) {
  "Start" {
    $existingPid = Read-PidFile $resolvedPidPath
    if ($null -ne $existingPid) {
      $existingProcess = Get-ExpectedProcess $existingPid $resolvedExecutable
      if ($null -ne $existingProcess) {
        Write-Host "multiplayer-netem is already running (PID $existingPid)."
        exit 0
      }
      Remove-Item -LiteralPath $resolvedPidPath -Force
    }

    if (-not (Test-Path -LiteralPath $resolvedExecutable -PathType Leaf)) {
      throw "Could not find multiplayer-netem executable: $resolvedExecutable"
    }

    $arguments = @(
      "--listen-port", $ListenPort.ToString(),
      "--target", $Target,
      "--profile", $Profile,
      "--seed", $Seed,
      "--log-file", $resolvedLogPath
    )
    $process = Start-Process -FilePath $resolvedExecutable -ArgumentList $arguments `
      -WorkingDirectory (Get-Location).Path -WindowStyle Hidden -PassThru
    [System.IO.File]::WriteAllText($resolvedPidPath, $process.Id.ToString(), [Text.Encoding]::ASCII)

    Start-Sleep -Milliseconds 150
    $startedProcess = Get-ExpectedProcess $process.Id $resolvedExecutable
    if ($null -eq $startedProcess) {
      if (Test-Path -LiteralPath $resolvedPidPath -PathType Leaf) {
        Remove-Item -LiteralPath $resolvedPidPath -Force
      }
      throw "multiplayer-netem exited during startup; inspect $resolvedLogPath"
    }
    Write-Host "Started multiplayer-netem (PID $($process.Id), profile $Profile)."
  }
  "Stop" {
    $processId = Read-PidFile $resolvedPidPath
    if ($null -eq $processId) {
      Write-Host "multiplayer-netem is not running."
      exit 0
    }
    $process = Get-ExpectedProcess $processId $resolvedExecutable
    if ($null -ne $process) {
      Stop-Process -Id $processId -Force
      Wait-Process -Id $processId -Timeout 5 -ErrorAction SilentlyContinue
      Write-Host "Stopped multiplayer-netem (PID $processId)."
    }
    Remove-Item -LiteralPath $resolvedPidPath -Force
  }
  "Status" {
    $processId = Read-PidFile $resolvedPidPath
    if ($null -eq $processId) {
      throw "multiplayer-netem is not running; run task start-mp-netem first"
    }
    $process = Get-ExpectedProcess $processId $resolvedExecutable
    if ($null -eq $process) {
      throw "multiplayer-netem PID file is stale; run task stop-mp-netem"
    }
    Write-Host "multiplayer-netem is running (PID $processId). Log: $resolvedLogPath"
  }
}
