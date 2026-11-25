# Windows half of the portable-mode isolation test (B-408): what a session left outside its portable folder.
#   powershell -File tools\portable_isolation_snapshot.ps1 before <work dir>     records the marker, the registry and the firewall
#   powershell -File tools\portable_isolation_snapshot.ps1 after  <work dir>     lists what changed (exit 1 if anything did)
# <work dir> holds run\app (the portable copy), run\home, run\appdata, run\local, run\temp (the sandbox the session ran in:
# USERPROFILE, APPDATA, LOCALAPPDATA and TEMP were pointed there, only for the game's process). "after" lists
#   - every file the sandbox holds (nothing should be there),
#   - files in the real profile's places created or changed since "before" that carry the game's names or look like caches,
#     crash dumps or logs (Documents\bots*, %LOCALAPPDATA%\bots-recomp, %TEMP% skullburglar-update-*, driver shader caches),
#   - new HKCU\Software keys, Start menu entries, and Windows Firewall rules naming the portable copy,
# minus tools\portable_isolation_allow.txt entries for Windows ("Windows <regex>  # reason"). The sandbox is the proof for what
# the game does with the environment; the real-profile list is for what no environment variable can move (the registry's
# shell folders: Documents, Start menu).
param([Parameter(Mandatory)][string]$Mode, [Parameter(Mandatory)][string]$Work)
$ErrorActionPreference = 'SilentlyContinue'
$state = Join-Path $Work 'snapshot'
New-Item -ItemType Directory -Force $state | Out-Null
$docs = [Environment]::GetFolderPath('MyDocuments')
$start = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
function Reg-Keys { (Get-ChildItem 'HKCU:\Software' | % { $_.PSChildName } | Sort-Object) -join "`n" }
function Fw-Rules { (Get-NetFirewallApplicationFilter | ? { $_.Program -like (Join-Path $Work 'run\app\*') } | % { $_.Program } | Sort-Object -Unique) -join "`n" }
if ($Mode -eq 'before') {
  (Get-Date).ToString('o') | Set-Content (Join-Path $state 'marker.txt')
  Reg-Keys | Set-Content (Join-Path $state 'reg.txt')
  Fw-Rules | Set-Content (Join-Path $state 'fw.txt')
  Get-ChildItem $start -Recurse -File | % { $_.FullName } | Sort-Object | Set-Content (Join-Path $state 'start.txt')
  exit 0
}
$marker = [datetime]::Parse((Get-Content (Join-Path $state 'marker.txt')))
$out = New-Object System.Collections.ArrayList
Get-ChildItem (Join-Path $Work 'run') -Recurse -File | ? { $_.FullName -notlike (Join-Path $Work 'run\app\*') } |
  % { [void]$out.Add("SANDBOX $($_.FullName)") }
$watch = @(
  (Join-Path $docs 'bots'), (Join-Path $docs 'bots-save-backups'), (Join-Path $docs 'bots-crashes'), (Join-Path $docs 'bots-p2'),
  (Join-Path $env:LOCALAPPDATA 'bots-recomp'), (Join-Path $env:LOCALAPPDATA 'NVIDIA'), (Join-Path $env:LOCALAPPDATA 'AMD'),
  (Join-Path $env:LOCALAPPDATA 'D3DSCache'), (Join-Path $env:LOCALAPPDATA 'Intel'), $env:TEMP, 'C:\Windows\Temp'
)
foreach ($w in $watch) {
  if (-not (Test-Path $w)) { continue }
  Get-ChildItem $w -Recurse -File | ? { $_.LastWriteTime -ge $marker -or $_.CreationTime -ge $marker } |
    ? { $_.FullName -match 'bots|skullburglar|crash|\.dmp$|DXCache|GLCache|VkCache|D3DSCache' -or $w -like '*\bots*' } |
    % { [void]$out.Add("REAL $($_.FullName)") }
}
$reg0 = (Get-Content (Join-Path $state 'reg.txt')); $reg1 = (Reg-Keys) -split "`n"
$reg1 | ? { $reg0 -notcontains $_ } | % { [void]$out.Add("REGISTRY HKCU\Software\$_") }
Get-ChildItem $start -Recurse -File | ? { $_.FullName -match 'bots|skullburglar' -and $_.LastWriteTime -ge $marker } | % { [void]$out.Add("REAL $($_.FullName)") }
$fw0 = (Get-Content (Join-Path $state 'fw.txt')); (Fw-Rules) -split "`n" | ? { $_ -and $fw0 -notcontains $_ } | % { [void]$out.Add("FIREWALL $_") }
$allow = @()
$allowFile = Join-Path $PSScriptRoot 'portable_isolation_allow.txt'
if (Test-Path $allowFile) { $allow = Get-Content $allowFile | ? { $_ -match '^Windows ' } }
$bad = 0
foreach ($line in ($out | Sort-Object -Unique)) {
  $path = $line.Substring($line.IndexOf(' ') + 1)
  $hit = $allow | ? { $rx = ($_ -replace '^Windows\s+', '') -replace '\s\s#.*$', ''; $path -match $rx } | Select-Object -First 1
  if ($hit) { Write-Output "allowed: $line   [$(($hit -split '  #',2)[1].Trim())]" } else { Write-Output "LEAK: $line"; $bad++ }
}
Write-Output ("== leaks outside the portable folder: $bad ==")
if ($bad -eq 0) { Write-Output 'PASS: nothing written outside the portable folder'; exit 0 } else { Write-Output "FAIL: $bad"; exit 1 }
