param([ValidateSet('Ativar','Desativar','Estado')][string]$Acao,[string]$GamePath)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'scripts\Common.ps1')
if(-not $GamePath -and (Test-Path (Join-Path $PSScriptRoot '.nfsmw-last-install.json'))){$GamePath=(Get-Content (Join-Path $PSScriptRoot '.nfsmw-last-install.json') -Raw|ConvertFrom-Json).game}
$game=Resolve-GameFolder $GamePath
if(-not $Acao){$choice=Read-Host '1 = ativar RTX; 2 = desativar; 3 = mostrar log';$Acao=switch($choice){'1'{'Ativar'}'2'{'Desativar'}default{'Estado'}}}
if($Acao -eq 'Estado'){
 $log=Join-Path $game 'NFSMW_RTX.log'
 if(Test-Path -LiteralPath $log){Get-Content -LiteralPath $log -Tail 18}else{Write-Host 'O jogo ainda nao gerou o log.'}
}else{
 $command=if($Acao -eq 'Ativar'){'on'}else{'off'}
 Set-Content -LiteralPath (Join-Path $game 'NFSMW_RTX.request') -Value ($command+':'+([guid]::NewGuid())) -Encoding ASCII
 Write-Host 'Comando enviado. A tecnica RTX tambem precisa estar marcada no ReShade.'
 Write-Host 'Se a protecao ja registrou uma falha, reinicie o jogo antes de reativar.'
}
