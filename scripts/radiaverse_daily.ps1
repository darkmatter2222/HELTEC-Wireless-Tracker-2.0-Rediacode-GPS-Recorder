<#
.SYNOPSIS
    Daily Radiaverse session sync -- uploads new MongoDB sessions to Radiaverse.

.DESCRIPTION
    Runs 'scripts/radiaverse_sync.py --sessions' via the repo .venv Python interpreter.
    Designed to be registered with Windows Task Scheduler so any session the device
    syncs to MongoDB is automatically mirrored to Radiaverse.

    Run once manually first to confirm the JWT token is cached:
        .\.venv\Scripts\python.exe scripts\radiaverse_sync.py --login

.USAGE
    Manual run:           .\scripts\radiaverse_daily.ps1
    Dry-run preview:      .\scripts\radiaverse_daily.ps1 -DryRun
    Register daily task:  .\scripts\radiaverse_daily.ps1 -Register
    Remove daily task:    .\scripts\radiaverse_daily.ps1 -Unregister
    Status check:         .\scripts\radiaverse_daily.ps1 -Status

.PARAMETER DryRun
    Pass --dry-run to the sync script (no actual uploads).

.PARAMETER Register
    Register a Windows Task Scheduler task named 'RadiaverseSessionSync' that
    runs this script daily at -Hour (default 06:00).

.PARAMETER Unregister
    Remove the 'RadiaverseSessionSync' scheduled task.

.PARAMETER Status
    Run 'radiaverse_sync.py --status' to show the upload summary table.

.PARAMETER Hour
    Hour of day (0-23) for the scheduled task trigger. Default: 6 (06:00 AM).
#>

param(
    [switch]$DryRun,
    [switch]$Register,
    [switch]$Unregister,
    [switch]$Status,
    [int]$Hour = 6
)

$ErrorActionPreference = "Stop"

$REPO      = Split-Path -Parent $PSScriptRoot
$PYTHON    = Join-Path $REPO ".venv\Scripts\python.exe"
$SCRIPT    = Join-Path $REPO "scripts\radiaverse_sync.py"
$LOG_FILE  = Join-Path $REPO "scripts\radiaverse_daily.log"
$TASK_NAME = "RadiaverseSessionSync"

# Verify venv exists
if (-not (Test-Path $PYTHON)) {
    Write-Error "Python venv not found at $PYTHON`nCreate it with: python -m venv .venv && .venv\Scripts\pip install pymongo requests websockets"
    exit 1
}

# ── Unregister ------------------------------------------------------------------
if ($Unregister) {
    Unregister-ScheduledTask -TaskName $TASK_NAME -Confirm:$false
    Write-Host "Task '$TASK_NAME' removed."
    exit 0
}

# ── Register with Task Scheduler ------------------------------------------------
if ($Register) {
    $psExe   = (Get-Command powershell.exe).Source
    $args_   = "-NonInteractive -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    $action  = New-ScheduledTaskAction -Execute $psExe -Argument $args_ -WorkingDirectory $REPO
    $trigger = New-ScheduledTaskTrigger -Daily -At "${Hour}:00"
    $settings = New-ScheduledTaskSettingsSet `
        -RunOnlyIfNetworkAvailable `
        -StartWhenAvailable `
        -ExecutionTimeLimit (New-TimeSpan -Hours 2)
    $principal = New-ScheduledTaskPrincipal `
        -UserId $env:USERNAME `
        -LogonType Interactive
    Register-ScheduledTask `
        -TaskName $TASK_NAME `
        -Action $action `
        -Trigger $trigger `
        -Settings $settings `
        -Principal $principal `
        -Description "Uploads new GPS sessions from MongoDB to Radiaverse daily" `
        -Force | Out-Null
    Write-Host "Task '$TASK_NAME' registered -- runs daily at ${Hour}:00."
    Write-Host "To verify: Get-ScheduledTask -TaskName '$TASK_NAME'"
    exit 0
}

# ── Status only -----------------------------------------------------------------
if ($Status) {
    & $PYTHON $SCRIPT --status
    exit $LASTEXITCODE
}

# ── Run the sync ----------------------------------------------------------------
$timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
"" | Add-Content -Path $LOG_FILE
"--- $timestamp ---" | Add-Content -Path $LOG_FILE

$syncArgs = @("$SCRIPT", "--sessions")
if ($DryRun) { $syncArgs += "--dry-run" }

& $PYTHON @syncArgs 2>&1 | Tee-Object -Append -FilePath $LOG_FILE
exit $LASTEXITCODE
