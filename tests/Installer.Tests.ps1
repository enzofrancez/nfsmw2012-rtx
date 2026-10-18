$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$fixture=Join-Path $env:TEMP ('nfsmw-installer-test-'+[guid]::NewGuid())
New-Item -ItemType Directory -Path $fixture -Force|Out-Null
function Assert($Value,[string]$Message){if(-not $Value){throw $Message}}
# Authored minimal PE metadata fixture; no game code or runtime is loaded.
$pe=New-Object byte[] 512;$pe[0]=0x4d;$pe[1]=0x5a;$pe[0x3c]=0x80;$pe[0x80]=0x50;$pe[0x81]=0x45;$pe[0x84]=0x4c;$pe[0x85]=1
[IO.File]::WriteAllBytes((Join-Path $fixture 'NFS13.exe'),$pe)
$fakeRuntime=Join-Path $fixture 'dxgi.dll'
Add-Type -TypeDefinition @'
using System.Reflection;
[assembly: AssemblyProduct("ReShade installer-test fixture")]
[assembly: AssemblyFileVersion("6.8.0.0")]
public class InstallerFixture { }
'@ -OutputAssembly $fakeRuntime
$ini=Join-Path $fixture 'ReShade.ini'
@('[GENERAL]','PresetPath=original.ini','KeepThis=unchanged','','[INPUT]','KeyOverlay=35,0,0,0')|Set-Content -LiteralPath $ini
$originalIni=(Get-FileHash -LiteralPath $ini).Hash
$originalExe=(Get-FileHash -LiteralPath (Join-Path $fixture 'NFS13.exe')).Hash
$originalRuntime=(Get-FileHash -LiteralPath $fakeRuntime).Hash
$statePath=Join-Path $root '.nfsmw-last-install.json';$stateExists=Test-Path -LiteralPath $statePath;$savedState=if($stateExists){[IO.File]::ReadAllBytes($statePath)}else{$null}
try{
 & (Join-Path $root 'Install.ps1') -GamePath $fixture -SceneTools
 Assert (Test-Path (Join-Path $fixture 'NFSMW_RTX.addon32')) 'Addon missing'
 Assert (Test-Path (Join-Path $fixture 'NFSMW_SceneTools.addon32')) 'Optional scene tools missing'
 Assert ((Get-Content (Join-Path $fixture 'NFSMW_RTX.request') -Raw) -match '^on:') 'RTX not enabled'
 Assert ((Get-Content $ini -Raw) -match 'KeepThis=unchanged') 'Unrelated setting lost'
 Assert ((Get-FileHash (Join-Path $fixture 'NFS13.exe')).Hash -eq $originalExe) 'Game executable modified'
 Assert ((Get-FileHash $fakeRuntime).Hash -eq $originalRuntime) 'ReShade runtime modified'
 $state=Get-Content $statePath -Raw|ConvertFrom-Json
 & (Join-Path $root 'Control-RTX.ps1') -GamePath $fixture -Acao Desativar
 Assert ((Get-Content (Join-Path $fixture 'NFSMW_RTX.request') -Raw) -match '^off:') 'Off command failed'
 & (Join-Path $root 'Uninstall.ps1') -GamePath $fixture -Backup $state.backup
 Assert (-not (Test-Path (Join-Path $fixture 'NFSMW_RTX.addon32'))) 'New addon not removed'
 Assert (-not (Test-Path (Join-Path $fixture 'NFSMW_SceneTools.addon32'))) 'Scene tools not removed'
 Assert ((Get-FileHash $ini).Hash -eq $originalIni) 'Original INI not restored byte-exactly'
 Assert ((Get-FileHash $fakeRuntime).Hash -eq $originalRuntime) 'Runtime changed during uninstall'
 # Existing addon survives install/restore, and a locked file causes zero mutation.
 $addon=Join-Path $fixture 'NFSMW_RTX.addon32';[IO.File]::WriteAllText($addon,'previous addon fixture')
 $oldAddon=(Get-FileHash $addon).Hash
 $lock=[IO.File]::Open($addon,'Open','ReadWrite','None');$rejected=$false
 try{& (Join-Path $root 'Install.ps1') -GamePath $fixture}catch{$rejected=$true}finally{$lock.Dispose()}
 Assert $rejected 'Locked addon was accepted'
 Assert ((Get-FileHash $ini).Hash -eq $originalIni) 'Failed preflight changed config'
 & (Join-Path $root 'Install.ps1') -GamePath $fixture
 $state=Get-Content $statePath -Raw|ConvertFrom-Json
 & (Join-Path $root 'Uninstall.ps1') -GamePath $fixture -Backup $state.backup
 Assert ((Get-FileHash $addon).Hash -eq $oldAddon) 'Prior addon not restored'
 $escaped=$false
 try{Safe-GameTarget $fixture '..\outside.txt'|Out-Null}catch{$escaped=$true}
 Assert $escaped 'Path traversal accepted'
 Write-Host 'PASS: clean install, exact rollback, optional tools, controls, locked-file rejection, prior addon preservation, traversal rejection. Synthetic fixture only.'
}finally{
 if($stateExists){[IO.File]::WriteAllBytes($statePath,$savedState)}elseif(Test-Path -LiteralPath $statePath){Remove-Item -LiteralPath $statePath}
 # Keep the isolated temporary fixture for diagnostics; never delete a computed tree.
 Write-Host "Fixture preservada em: $fixture"
}
