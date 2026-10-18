$ErrorActionPreference='Stop'
try{
 $tools=(Read-Host 'Incluir ferramentas opcionais de horario e freecam? [s/N]') -match '^[sSyY]$'
 & (Join-Path (Split-Path $PSScriptRoot -Parent) 'Install.ps1') -SceneTools:$tools
}catch{Write-Host $_ -ForegroundColor Red;exit 1}
