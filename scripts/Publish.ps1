param([string]$Name='nfsmw2012-rtx',[string]$Version='0.19.0')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
& gh auth status
if($LASTEXITCODE -ne 0){throw 'Conclua gh auth login antes de publicar. Nao passe senha ou token neste script.'}
$account=(& gh api user --jq '[.login, (.id|tostring)] | @tsv') -split "`t"
if($LASTEXITCODE -ne 0 -or $account.Count -ne 2){throw 'Nao foi possivel confirmar a conta GitHub.'}
$owner=$account[0];$id=$account[1]
$readme=Join-Path $root 'README.md'
$text=[IO.File]::ReadAllText($readme).Replace('https://github.com/OWNER/nfsmw2012-rtx/',"https://github.com/$owner/$Name/")
[IO.File]::WriteAllText($readme,$text,(New-Object Text.UTF8Encoding($false)))
& (Join-Path $PSScriptRoot 'Package.ps1') -Version $Version
if(-not (Test-Path (Join-Path $root '.git'))){& git init -b main $root;if($LASTEXITCODE -ne 0){throw 'git init falhou.'}}
& git -C $root config user.name $owner
& git -C $root config user.email "$id+$owner@users.noreply.github.com"
& git -C $root add --all
if($LASTEXITCODE -ne 0){throw 'git add falhou.'}
& git -C $root diff --cached --quiet
if($LASTEXITCODE -eq 1){& git -C $root commit -m "Release experimental v${Version}: DXR v19, installer and documentation";if($LASTEXITCODE -ne 0){throw 'Commit falhou.'}}
$remote=$null
$remotes=@(& git -C $root remote)
if($LASTEXITCODE -ne 0){throw 'Nao foi possivel consultar os remotes.'}
if($remotes -contains 'origin'){$remote=& git -C $root remote get-url origin;if($LASTEXITCODE -ne 0){throw 'Nao foi possivel consultar origin.'}}
if(-not $remote){
 & gh repo create "$owner/$Name" --public --description 'Experimental hardware DXR mod for Need for Speed Most Wanted 2012, with easy Windows installation and optional scene tools.' --source $root --remote origin --push
}else{
 if($remote -notmatch ('github\.com[:/]'+[regex]::Escape("$owner/$Name")+'(?:\.git)?$')){throw 'O remote existente pertence a outro repositorio.'}
 & git -C $root push -u origin main
}
if($LASTEXITCODE -ne 0){throw 'Publicacao do repositorio falhou.'}
$notes=Join-Path $root 'docs\RELEASE-v0.19.0.md'
$zip=Join-Path $root "artifacts\NFSMW2012-RTX-v$Version.zip"
& gh release create "v$Version" $zip ($zip+'.sha256') --repo "$owner/$Name" --target main --title "v$Version - experimental preview" --prerelease --notes-file $notes
if($LASTEXITCODE -ne 0){throw 'Upload da release falhou; repositorio preservado.'}
Write-Host "Repositorio: https://github.com/$owner/$Name"
Write-Host "Release: https://github.com/$owner/$Name/releases/tag/v$Version"
