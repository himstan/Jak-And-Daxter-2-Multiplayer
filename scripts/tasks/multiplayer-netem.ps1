param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("Start", "Stop", "Status", "RunHost")]
  [string]$Action,

  [Parameter(Mandatory = $true)]
  [string]$ExecutablePath,

  [Parameter(Mandatory = $true)]
  [string]$PidPath,

  [Parameter(Mandatory = $true)]
  [string]$LogPath,

  [string]$MetadataPath = "",
  [ValidateSet("lan", "wifi", "4g", "poor-4g", "stress")]
  [string]$Profile = "wifi",

  [string]$Seed = "1",
  [int]$ListenPort = 26212,
  [string]$Target = "127.0.0.1:26210",
  [string]$GameExecutablePath = "",
  [string]$GameArguments = "",
  [int]$ExpectedProcessId = 0,
  [string]$ExpectedOwnerToken = ""
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

function Read-RelayMetadata([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    return $null
  }
  try {
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
  } catch {
    throw "The netem metadata file is invalid: $Path"
  }
}

function Get-MetadataProperty($Metadata, [string]$Name) {
  if ($null -eq $Metadata) {
    return $null
  }
  $property = $Metadata.PSObject.Properties[$Name]
  if ($null -eq $property) {
    return $null
  }
  return $property.Value
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

function Get-ProcessStartTimeTicks($Process) {
  return $Process.StartTime.ToUniversalTime().Ticks.ToString([Globalization.CultureInfo]::InvariantCulture)
}

function Test-RelayMetadataIdentity($Process, $Metadata) {
  if ($null -eq $Metadata) {
    return $false
  }
  $metadataPid = [string](Get-MetadataProperty $Metadata "ProcessId")
  $metadataStartTime = [string](Get-MetadataProperty $Metadata "StartTimeUtcTicks")
  if ([string]::IsNullOrWhiteSpace($metadataPid) -or
      [string]::IsNullOrWhiteSpace($metadataStartTime)) {
    return $false
  }
  if ($metadataPid -ne $Process.Id.ToString([Globalization.CultureInfo]::InvariantCulture)) {
    return $false
  }
  try {
    return $metadataStartTime -eq (Get-ProcessStartTimeTicks $Process)
  } catch {
    return $false
  }
}

function Remove-RelayState {
  if (Test-Path -LiteralPath $resolvedPidPath -PathType Leaf) {
    Remove-Item -LiteralPath $resolvedPidPath -Force
  }
  if (Test-Path -LiteralPath $resolvedMetadataPath -PathType Leaf) {
    Remove-Item -LiteralPath $resolvedMetadataPath -Force
  }
}

function Test-UdpPortAvailable([int]$Port) {
  try {
    $endpoints = @(Get-NetUDPEndpoint -LocalPort $Port -ErrorAction Stop)
    return $endpoints.Count -eq 0
  } catch [System.Management.Automation.CommandNotFoundException] {
    return $true
  } catch {
    Write-Warning "Could not inspect UDP port $Port before starting netem; the relay will report bind errors if it is occupied."
    return $true
  }
}

function Test-RequestedConfiguration($Metadata) {
  $activeProfile = Get-MetadataProperty $Metadata "Profile"
  $activeSeed = Get-MetadataProperty $Metadata "Seed"
  $activePort = Get-MetadataProperty $Metadata "ListenPort"
  $activeTarget = Get-MetadataProperty $Metadata "Target"
  if ($null -eq $activeProfile -or $null -eq $activeSeed -or $null -eq $activePort -or
      $null -eq $activeTarget) {
    throw "multiplayer-netem is already running, but its profile metadata is missing; run task stop-mp-netem before starting it again"
  }
  if ($activeProfile -ine $Profile -or [string]$activeSeed -ne $Seed -or
      [int]$activePort -ne $ListenPort -or $activeTarget -ine $Target) {
    throw "multiplayer-netem is already running with profile $activeProfile, seed $activeSeed, port $activePort, target $activeTarget; requested profile $Profile, seed $Seed, port $ListenPort, target $Target"
  }
}

function Write-RelayMetadata([int]$ProcessId, [string]$OwnerToken, $Process) {
  $metadata = [ordered]@{
    ProcessId = $ProcessId
    StartTimeUtcTicks = Get-ProcessStartTimeTicks $Process
    OwnerToken = if ([string]::IsNullOrWhiteSpace($OwnerToken)) { "manual" } else { $OwnerToken }
    Profile = $Profile
    Seed = $Seed
    ListenPort = $ListenPort
    Target = $Target
  }
  $json = $metadata | ConvertTo-Json -Compress
  [System.IO.File]::WriteAllText($resolvedMetadataPath, $json, [Text.Encoding]::UTF8)
}

function Get-GameArgumentList {
  if ([string]::IsNullOrWhiteSpace($GameArguments)) {
    return @()
  }
  return @($GameArguments -split '\|')
}

function Start-Relay([string]$OwnerToken, [switch]$RequireNew) {
  $existingPid = Read-PidFile $resolvedPidPath
  if ($null -ne $existingPid) {
    $existingProcess = Get-ExpectedProcess $existingPid $resolvedExecutable
    if ($null -ne $existingProcess) {
      $existingMetadata = Read-RelayMetadata $resolvedMetadataPath
      if (-not (Test-RelayMetadataIdentity $existingProcess $existingMetadata)) {
        throw "multiplayer-netem PID $existingPid is live, but its metadata does not match that process; run task stop-mp-netem and inspect the relay before retrying"
      }
      if ($RequireNew) {
        $activeProfile = Get-MetadataProperty $existingMetadata "Profile"
        $activeSeed = Get-MetadataProperty $existingMetadata "Seed"
        throw "multiplayer-netem is already running (PID $existingPid, profile $activeProfile, seed $activeSeed); stop it before starting a session-owned host"
      }
      Test-RequestedConfiguration $existingMetadata
      Write-Host "multiplayer-netem is already running (PID $existingPid, profile $Profile, seed $Seed)."
      return [pscustomobject]@{
        ProcessId = $existingPid
        OwnerToken = [string](Get-MetadataProperty $existingMetadata "OwnerToken")
      }
    }
    Remove-RelayState
  } elseif (Test-Path -LiteralPath $resolvedMetadataPath -PathType Leaf) {
    Remove-Item -LiteralPath $resolvedMetadataPath -Force
  }

  if (-not (Test-Path -LiteralPath $resolvedExecutable -PathType Leaf)) {
    throw "Could not find multiplayer-netem executable: $resolvedExecutable"
  }
  if (-not (Test-UdpPortAvailable $ListenPort)) {
    throw "UDP port $ListenPort is already occupied; stop the existing relay or process using that port before starting netem"
  }

  $arguments = @(
    "--listen-port", $ListenPort.ToString([Globalization.CultureInfo]::InvariantCulture),
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
    Remove-RelayState
    throw "multiplayer-netem exited during startup; inspect $resolvedLogPath"
  }
  try {
    Write-RelayMetadata $process.Id $OwnerToken $startedProcess
  } catch {
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    Remove-RelayState
    throw
  }
  Write-Host "Started multiplayer-netem (PID $($process.Id), profile $Profile, seed $Seed)."
  return [pscustomobject]@{
    ProcessId = $process.Id
    OwnerToken = if ([string]::IsNullOrWhiteSpace($OwnerToken)) { "manual" } else { $OwnerToken }
  }
}

function Stop-Relay([int]$ExpectedId = 0, [string]$ExpectedToken = "") {
  try {
    $processId = Read-PidFile $resolvedPidPath
  } catch {
    Write-Warning $_
    return
  }
  if ($null -eq $processId) {
    Write-Host "multiplayer-netem is not running."
    return
  }
  if ($ExpectedId -gt 0 -and $processId -ne $ExpectedId) {
    Write-Warning "Not stopping multiplayer-netem PID $processId because session PID $ExpectedId is no longer recorded."
    return
  }

  $metadata = $null
  try {
    $metadata = Read-RelayMetadata $resolvedMetadataPath
  } catch {
    Write-Warning $_
    return
  }
  if ($ExpectedToken -and ([string](Get-MetadataProperty $metadata "OwnerToken") -ne $ExpectedToken)) {
    Write-Warning "Not stopping multiplayer-netem PID $processId because its session owner token changed."
    return
  }

  try {
    $process = Get-ExpectedProcess $processId $resolvedExecutable
  } catch {
    Write-Warning $_
    return
  }
  if ($null -eq $process) {
    Remove-RelayState
    Write-Host "Removed stale multiplayer-netem state for PID $processId."
    return
  }
  if ($null -ne $metadata -and -not (Test-RelayMetadataIdentity $process $metadata)) {
    Write-Warning "Not stopping multiplayer-netem PID $processId because its metadata does not match the live process."
    return
  }

  try {
    Stop-Process -Id $processId -Force
    Wait-Process -Id $processId -Timeout 5 -ErrorAction SilentlyContinue
  } catch {
    Write-Warning "Failed to stop multiplayer-netem PID $($processId): $_"
    return
  }

  $remainingProcess = Get-ExpectedProcess $processId $resolvedExecutable
  if ($null -ne $remainingProcess) {
    Write-Warning "multiplayer-netem PID $processId did not exit; leaving PID and metadata files in place."
    return
  }
  Remove-RelayState
  Write-Host "Stopped multiplayer-netem (PID $processId)."
}

$resolvedExecutable = Resolve-AbsolutePath $ExecutablePath
$resolvedPidPath = Resolve-AbsolutePath $PidPath
$resolvedLogPath = Resolve-AbsolutePath $LogPath
if ([string]::IsNullOrWhiteSpace($MetadataPath)) {
  $MetadataPath = [System.IO.Path]::ChangeExtension($PidPath, ".meta.json")
}
$resolvedMetadataPath = Resolve-AbsolutePath $MetadataPath

switch ($Action) {
  "Start" {
    Start-Relay "" | Out-Null
  }
  "Stop" {
    Stop-Relay $ExpectedProcessId $ExpectedOwnerToken
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
    $metadata = Read-RelayMetadata $resolvedMetadataPath
    if (-not (Test-RelayMetadataIdentity $process $metadata)) {
      throw "multiplayer-netem metadata is missing or stale; run task stop-mp-netem"
    }
    $activeProfile = Get-MetadataProperty $metadata "Profile"
    $activeSeed = Get-MetadataProperty $metadata "Seed"
    Write-Host "multiplayer-netem is running (PID $processId, profile $activeProfile, seed $activeSeed). Log: $resolvedLogPath"
  }
  "RunHost" {
    if ([string]::IsNullOrWhiteSpace($GameExecutablePath)) {
      throw "RunHost requires -GameExecutablePath"
    }
    $ownerToken = [Guid]::NewGuid().ToString("N")
    $session = Start-Relay $ownerToken -RequireNew
    $gameExitCode = 1
    try {
      $resolvedGame = Resolve-AbsolutePath $GameExecutablePath
      if (-not (Test-Path -LiteralPath $resolvedGame -PathType Leaf)) {
        throw "Could not find game executable: $resolvedGame"
      }
      Write-Host "Starting session-owned host game (PID will be reported by the game process)."
      & $resolvedGame @(Get-GameArgumentList)
      $gameExitCode = $LASTEXITCODE
    } catch {
      [Console]::Error.WriteLine("multiplayer-netem host session failed: $($_.Exception.Message)")
    } finally {
      Stop-Relay $session.ProcessId $session.OwnerToken
    }
    exit $gameExitCode
  }
}
