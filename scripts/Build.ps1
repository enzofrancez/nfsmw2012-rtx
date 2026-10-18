param([string]$MsvcRoot,[string]$SdkRoot,[string]$SdkVersion)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(-not $MsvcRoot){
 $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
 $vs=if(Test-Path -LiteralPath $vswhere){& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath}else{$null}
 if($vs){$candidates=@(Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Tools\MSVC') -Directory)}
 else{
  $candidates=@(foreach($base in @(${env:ProgramFiles(x86)},$env:ProgramFiles)){Get-ChildItem -Path (Join-Path $base 'Microsoft Visual Studio\*\*\VC\Tools\MSVC\*') -Directory -ErrorAction SilentlyContinue})
 }
 $compilerRoot=$candidates|Where-Object {Test-Path (Join-Path $_.FullName 'bin\Hostx64\x86\cl.exe')}|Sort-Object {[version]$_.Name} -Descending|Select-Object -First 1
 if(-not $compilerRoot){throw 'Instale Visual Studio Build Tools com Desktop development with C++.'}
 $MsvcRoot=$compilerRoot.FullName
}
if(-not $SdkRoot){$SdkRoot=(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10}
if(-not $SdkVersion){
 $sdk=Get-ChildItem -LiteralPath (Join-Path $SdkRoot 'Include') -Directory|Where-Object {(Test-Path (Join-Path $_.FullName 'ucrt')) -and (Test-Path (Join-Path $SdkRoot ('bin\'+$_.Name+'\x64\dxc.exe')))}|Sort-Object {[version]$_.Name} -Descending|Select-Object -First 1
 if(-not $sdk){throw 'Windows SDK com DXC nao encontrado.'};$SdkVersion=$sdk.Name
}
$out=Join-Path $root 'dist\payload';$build=Join-Path $root 'dist\build'
New-Item -ItemType Directory -Path $out,$build -Force|Out-Null
$savedInclude=$env:INCLUDE;$savedLib=$env:LIB
$env:INCLUDE="$MsvcRoot\include;$SdkRoot\Include\$SdkVersion\ucrt;$SdkRoot\Include\$SdkVersion\shared;$SdkRoot\Include\$SdkVersion\um;$SdkRoot\Include\$SdkVersion\winrt;$root\third_party\reshade;$root\third_party\imgui"
Push-Location $build
try{
 $dxc=Join-Path $SdkRoot "bin\$SdkVersion\x64\dxc.exe"
 foreach($entry in @(@('main','rtx_trace.cso'),@('ConvertGeometry','rtx_convert.cso'))){
  & $dxc -T cs_6_5 -E $entry[0] -O3 -Fo (Join-Path $out $entry[1]) (Join-Path $root 'src\rtx\rtx_trace.hlsl')
  if($LASTEXITCODE -ne 0){throw 'Compilacao HLSL falhou.'}
 }
 foreach($arch in @('x86','x64')){
  $env:LIB="$MsvcRoot\lib\$arch;$SdkRoot\Lib\$SdkVersion\ucrt\$arch;$SdkRoot\Lib\$SdkVersion\um\$arch"
  $cl=Join-Path $MsvcRoot "bin\Hostx64\$arch\cl.exe"
  if($arch -eq 'x86'){
   & $cl /nologo /EHsc /std:c++17 /O2 /MT /W4 /D_CRT_SECURE_NO_WARNINGS /LD (Join-Path $root 'src\rtx\rtx_addon.cpp') /link "/OUT:$out\NFSMW_RTX.addon32" d3d11.lib d3dcompiler.lib user32.lib d3d12.lib dxgi.lib
   if($LASTEXITCODE -ne 0){throw 'Addon RTX x86 falhou.'}
   & $cl /nologo /EHsc /std:c++17 /O2 /MT /W4 /LD (Join-Path $root 'src\scene-tools\scene_tools.cpp') /link "/OUT:$out\NFSMW_SceneTools.addon32" user32.lib
  }else{
   & $cl /nologo /EHsc /std:c++17 /O2 /MT /W4 /D_CRT_SECURE_NO_WARNINGS (Join-Path $root 'src\rtx\rtx_helper.cpp') "/Fe:$out\NFSMW_RTX_GPU_Helper.exe" /link d3d12.lib dxgi.lib d3dcompiler.lib
  }
  if($LASTEXITCODE -ne 0){throw "Compilacao $arch falhou."}
 }
}finally{Pop-Location;$env:INCLUDE=$savedInclude;$env:LIB=$savedLib}
Copy-Item -LiteralPath (Join-Path $root 'shaders\NFSMW_RTX.fx') -Destination $out
Copy-Item -LiteralPath (Join-Path $root 'third_party\reshade\ReShade.fxh') -Destination (Join-Path $out 'NFSMW_ReShade.fxh')
Copy-Item -LiteralPath (Join-Path $root 'presets\NFSMW2012-RTX.ini') -Destination $out
$hashes=[ordered]@{};Get-ChildItem -LiteralPath $out -File|Where-Object {$_.Name -ne 'SHA256.json'}|Sort-Object Name|ForEach-Object {$hashes[$_.Name]=(Get-FileHash -LiteralPath $_.FullName).Hash}
$hashes|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $out 'SHA256.json') -Encoding UTF8
Write-Host "Build concluido: $out"
