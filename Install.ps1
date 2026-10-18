param([string]$GamePath,[switch]$SceneTools)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'scripts\Common.ps1')
$game=Resolve-GameFolder $GamePath
Assert-GameClosed $game
Assert-ReShade $game
$payload=Join-Path $PSScriptRoot 'payload'
if(-not (Test-Path -LiteralPath $payload)){$payload=Join-Path $PSScriptRoot 'dist\payload'}
if(-not (Test-Path -LiteralPath (Join-Path $payload 'SHA256.json'))){throw 'Baixe o ZIP pronto na pagina Releases. O ZIP Source code exige compilacao.'}
$hashes=Get-Content -LiteralPath (Join-Path $payload 'SHA256.json') -Raw|ConvertFrom-Json
foreach($property in $hashes.PSObject.Properties){
 $source=Safe-GameTarget $payload $property.Name
 if((Get-FileHash -LiteralPath $source).Hash -ne $property.Value){throw "Arquivo do pacote alterado ou incompleto: $($property.Name)"}
}
$ini=Join-Path $game 'ReShade.ini'
$addonPath=Get-IniValue $ini 'ADDON' 'AddonPath'
if($addonPath -and $addonPath -notin @('.','.\',$game,$game+'\')){throw 'AddonPath personalizado no ReShade.ini. Este instalador requer AddonPath=.\; ele nao altera essa configuracao automaticamente.'}
$targets=[ordered]@{
 'NFSMW_RTX.addon32'='NFSMW_RTX.addon32'
 'NFSMW_RTX_GPU_Helper.exe'='NFSMW_RTX_GPU_Helper.exe'
 'rtx_trace.cso'='rtx_trace.cso'
 'rtx_convert.cso'='rtx_convert.cso'
 'NFSMW2012-RTX.ini'='NFSMW2012-RTX.ini'
 'reshade-shaders\Shaders\NFSMW_RTX.fx'='NFSMW_RTX.fx'
 'reshade-shaders\Shaders\NFSMW_ReShade.fxh'='NFSMW_ReShade.fxh'
}
if($SceneTools){$targets['NFSMW_SceneTools.addon32']='NFSMW_SceneTools.addon32'}
$relativePaths=@($targets.Keys)+@('ReShade.ini','NFSMW_RTX.request')
$backup=Join-Path $game ('.nfsmw-rtx\backups\'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $backup -Force|Out-Null
$entries=foreach($relative in $relativePaths){
 $target=Safe-GameTarget $game $relative;$exists=Test-Path -LiteralPath $target -PathType Leaf;$hash=''
 if($exists){$dest=Safe-GameTarget $backup $relative;New-Item -ItemType Directory -Path (Split-Path $dest) -Force|Out-Null;Copy-Item -LiteralPath $target -Destination $dest;$hash=(Get-FileHash -LiteralPath $dest).Hash}
 [pscustomobject]@{path=$relative;existed=$exists;originalHash=$hash}
}
@{schema=1;game=$game;version='v19';files=@($entries)}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $backup 'manifest.json') -Encoding UTF8
try{
 foreach($relative in $targets.Keys){
  $dest=Safe-GameTarget $game $relative;New-Item -ItemType Directory -Path (Split-Path $dest) -Force|Out-Null
  Copy-Item -LiteralPath (Join-Path $payload $targets[$relative]) -Destination $dest -Force
 }
 Set-IniValue $ini 'ADDON' 'AddonPath' '.\'
 Set-IniValue $ini 'GENERAL' 'PresetPath' '.\NFSMW2012-RTX.ini'
 Set-IniValue $ini 'GENERAL' 'StartupPresetPath' '.\NFSMW2012-RTX.ini'
 $paths=Get-IniValue $ini 'GENERAL' 'EffectSearchPaths'
 if(-not $paths){$paths='.\reshade-shaders\Shaders\**'}
 elseif($paths -notmatch 'reshade-shaders[\\/]Shaders'){$paths+=',.\reshade-shaders\Shaders\**'}
 Set-IniValue $ini 'GENERAL' 'EffectSearchPaths' $paths
 Set-IniValue $ini 'INPUT' 'KeyOverlay' '36,0,0,0'
 Set-IniValue $ini 'INPUT' 'InputProcessing' '2'
 # "on" enables capture without resetting the user's saved strength sliders.
 Set-Content -LiteralPath (Join-Path $game 'NFSMW_RTX.request') -Value ('on:'+([guid]::NewGuid())) -Encoding ASCII
 foreach($relative in $targets.Keys){if((Get-FileHash -LiteralPath (Safe-GameTarget $game $relative)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $payload $targets[$relative])).Hash){throw "Falha ao conferir $relative"}}
 @{game=$game;backup=$backup}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $PSScriptRoot '.nfsmw-last-install.json') -Encoding UTF8
 Write-Host "Instalado. Backup: $backup"
 Write-Host 'Abra o jogo OFFLINE. Home abre o ReShade; F7 alterna os efeitos RTX.'
}catch{
 $failure=$_
 try{Restore-Manifest $game $backup}catch{Write-Warning "Restauracao automatica falhou; backup preservado em $backup"}
 throw $failure
}
