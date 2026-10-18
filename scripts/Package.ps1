param([string]$Version='0.19.0')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$payload=Join-Path $root 'dist\payload'
if(-not (Test-Path (Join-Path $payload 'SHA256.json'))){throw 'Execute scripts\Build.ps1 primeiro.'}
$artifacts=Join-Path $root 'artifacts';New-Item -ItemType Directory -Path $artifacts -Force|Out-Null
$stage=Join-Path $root ('dist\package-'+[guid]::NewGuid())
New-Item -ItemType Directory -Path $stage -Force|Out-Null
foreach($name in @('Install.ps1','Uninstall.ps1','Control-RTX.ps1','Instalar.cmd','Desinstalar.cmd','Controle-RTX.cmd','README.md','LICENSE','THIRD_PARTY_NOTICES.md')){Copy-Item -LiteralPath (Join-Path $root $name) -Destination $stage}
New-Item -ItemType Directory -Path (Join-Path $stage 'scripts') -Force|Out-Null
Copy-Item -LiteralPath (Join-Path $root 'scripts\Common.ps1'),(Join-Path $root 'scripts\Start-Install.ps1') -Destination (Join-Path $stage 'scripts')
Copy-Item -LiteralPath $payload -Destination (Join-Path $stage 'payload') -Recurse
Copy-Item -LiteralPath (Join-Path $root 'docs') -Destination (Join-Path $stage 'docs') -Recurse
New-Item -ItemType Directory -Path (Join-Path $stage 'licenses') -Force|Out-Null
Copy-Item -LiteralPath (Join-Path $root 'third_party\reshade\LICENSE-ReShade.txt'),(Join-Path $root 'third_party\imgui\LICENSE-ImGui.txt') -Destination (Join-Path $stage 'licenses')
$zip=Join-Path $artifacts "NFSMW2012-RTX-v$Version.zip"
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
Get-FileHash -LiteralPath $zip|ForEach-Object {"$($_.Hash.ToLower())  $([IO.Path]::GetFileName($zip))"}|Set-Content -LiteralPath ($zip+'.sha256') -Encoding ASCII
Write-Host "Pacote: $zip"
