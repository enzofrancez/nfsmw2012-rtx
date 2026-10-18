Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
function Resolve-GameFolder([string]$GamePath) {
 if(-not $GamePath){
  Add-Type -AssemblyName System.Windows.Forms
  $picker=New-Object System.Windows.Forms.OpenFileDialog
  $picker.Title='Selecione NFS13.exe (Most Wanted 2012)'
  $picker.Filter='Most Wanted 2012 (NFS13.exe)|NFS13.exe'
  if($picker.ShowDialog() -ne 'OK'){throw 'Operacao cancelada.'}
  $GamePath=$picker.FileName
 }
 if([IO.Path]::GetExtension($GamePath) -eq '.exe'){$GamePath=Split-Path -Parent $GamePath}
 $folder=[IO.Path]::GetFullPath((Resolve-Path -LiteralPath $GamePath).Path)
 $exe=Join-Path $folder 'NFS13.exe'
 if(-not (Test-Path -LiteralPath $exe -PathType Leaf)){throw 'Selecione a pasta que contem NFS13.exe, do Most Wanted 2012.'}
 $stream=[IO.File]::OpenRead($exe)
 try{
  $reader=New-Object IO.BinaryReader($stream)
  if($reader.ReadUInt16() -ne 0x5a4d){throw 'NFS13.exe nao e um executavel Windows valido.'}
  $stream.Position=0x3c;$offset=$reader.ReadUInt32();$stream.Position=$offset
  if($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x14c){throw 'Este pacote requer NFS13.exe de 32 bits.'}
 }finally{$stream.Dispose()}
 return $folder
}
function Safe-GameTarget([string]$Game,[string]$Relative) {
 $root=[IO.Path]::GetFullPath($Game).TrimEnd('\')+'\'
 $target=[IO.Path]::GetFullPath((Join-Path $root $Relative))
 if(-not $target.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)){throw 'Caminho fora da pasta do jogo.'}
 return $target
}
function Assert-GameClosed([string]$Game) {
 foreach($relative in @('NFS13.exe','NFSMW_RTX.addon32','NFSMW_RTX_GPU_Helper.exe','NFSMW_SceneTools.addon32')){
  $path=Safe-GameTarget $Game $relative
  if(Test-Path -LiteralPath $path){
   try{$handle=[IO.File]::Open($path,'Open','ReadWrite','None');$handle.Dispose()}
   catch{throw "Feche o jogo e qualquer auxiliar do mod antes de continuar. Arquivo bloqueado: $relative"}
  }
 }
}
function Get-IniValue([string]$Path,[string]$Section,[string]$Key) {
 if(-not (Test-Path -LiteralPath $Path)){return ''}
 $active=$false
 foreach($line in [IO.File]::ReadAllLines($Path)){
  if($line -match '^\s*\[([^]]+)\]'){$active=$Matches[1] -eq $Section;continue}
  if($active -and $line -match ('^\s*'+[regex]::Escape($Key)+'\s*=(.*)$')){return $Matches[1]}
 }
 return ''
}
function Set-IniValue([string]$Path,[string]$Section,[string]$Key,[string]$Value) {
 $lines=New-Object 'Collections.Generic.List[string]'
 if(Test-Path -LiteralPath $Path){$lines.AddRange([IO.File]::ReadAllLines($Path))}
 $begin=-1;$end=$lines.Count
 for($i=0;$i -lt $lines.Count;$i++){
  if($lines[$i] -match '^\s*\[([^]]+)\]'){
   if($begin -ge 0){$end=$i;break}
   if($Matches[1] -eq $Section){$begin=$i}
  }
 }
 if($begin -lt 0){$lines.Add('');$lines.Add("[$Section]");$lines.Add("$Key=$Value")}
 else{
  $found=$false
  for($i=$end-1;$i -gt $begin;$i--){if($lines[$i] -match ('^\s*'+[regex]::Escape($Key)+'\s*=')){$lines.RemoveAt($i)}}
  $lines.Insert($begin+1,"$Key=$Value")
 }
 [IO.File]::WriteAllLines($Path,$lines,(New-Object Text.UTF8Encoding($false)))
}
function Assert-ReShade([string]$Game) {
 foreach($name in @('dxgi.dll','d3d11.dll')){
  $path=Join-Path $Game $name
  if(Test-Path -LiteralPath $path){
   $info=[Diagnostics.FileVersionInfo]::GetVersionInfo($path)
   if(($info.ProductName+' '+$info.FileDescription) -match 'ReShade'){
    if($info.FileMajorPart -lt 6 -or ($info.FileMajorPart -eq 6 -and $info.FileMinorPart -lt 8)){throw 'Instale ReShade 6.8.0 ou mais recente, com full add-on support.'}
    return
   }
  }
 }
 throw 'Instale primeiro o ReShade com full add-on support para DirectX 10/11/12. Instrucoes no README.'
}
function Restore-Manifest([string]$Game,[string]$Backup) {
 $manifest=Get-Content -LiteralPath (Join-Path $Backup 'manifest.json') -Raw|ConvertFrom-Json
 $managed=@('NFSMW_RTX.addon32','NFSMW_RTX_GPU_Helper.exe','rtx_trace.cso','rtx_convert.cso','NFSMW2012-RTX.ini','NFSMW_SceneTools.addon32','reshade-shaders\Shaders\NFSMW_RTX.fx','reshade-shaders\Shaders\NFSMW_ReShade.fxh','ReShade.ini','NFSMW_RTX.request')
 if([IO.Path]::GetFullPath($manifest.game) -ne [IO.Path]::GetFullPath($Game)){throw 'O backup pertence a outra pasta do jogo.'}
 foreach($entry in $manifest.files){
  if($entry.path -notin $managed){throw 'O manifesto contem um arquivo que nao pertence ao mod.'}
  $target=Safe-GameTarget $Game $entry.path
  if($entry.existed){
   $source=Safe-GameTarget $Backup $entry.path
   if((Get-FileHash -LiteralPath $source).Hash -ne $entry.originalHash){throw "Backup alterado: $($entry.path)"}
  }
 }
 foreach($entry in $manifest.files){
  $target=Safe-GameTarget $Game $entry.path
  if($entry.existed){Copy-Item -LiteralPath (Safe-GameTarget $Backup $entry.path) -Destination $target -Force}
  elseif(Test-Path -LiteralPath $target){Remove-Item -LiteralPath $target -Force}
 }
}
