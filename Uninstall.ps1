param([string]$GamePath,[string]$Backup)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'scripts\Common.ps1')
if(-not $GamePath -and (Test-Path (Join-Path $PSScriptRoot '.nfsmw-last-install.json'))){$GamePath=(Get-Content (Join-Path $PSScriptRoot '.nfsmw-last-install.json') -Raw|ConvertFrom-Json).game}
$game=Resolve-GameFolder $GamePath
Assert-GameClosed $game
if(-not $Backup){
 $backups=Join-Path $game '.nfsmw-rtx\backups'
 $latest=Get-ChildItem -LiteralPath $backups -Directory|Where-Object {Test-Path (Join-Path $_.FullName 'manifest.json')}|Sort-Object Name -Descending|Select-Object -First 1
 if(-not $latest){throw 'Nenhum backup encontrado.'};$Backup=$latest.FullName
}
Restore-Manifest $game ([IO.Path]::GetFullPath($Backup))
Write-Host 'Instalacao anterior restaurada. O backup foi preservado.'
