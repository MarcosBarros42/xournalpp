# fork: build do xournalpp + deploy do exe para build\dist\bin
#
# Uso (de qualquer lugar): powershell -File scripts\build-dist.ps1
# Um commit/tipo novo de build: ninja xournalpp; copia o exe pro diretorio
# que a instancia do dia a dia executa (build\dist\bin).

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build"
$builtExe = Join-Path $buildDir "xournalpp.exe"
$deployExe = Join-Path $buildDir "dist\bin\xournalpp.exe"

# Toolchain (c++, ninja e DLLs de runtime) vive no msys64
$env:PATH = "C:\msys64\ucrt64\bin;C:\msys64\bin;$env:PATH"

Push-Location $buildDir
try {
    ninja xournalpp
    if ($LASTEXITCODE -ne 0) { throw "build falhou (exit $LASTEXITCODE)" }
}
finally {
    Pop-Location
}

if (Get-Process xournalpp -ErrorAction SilentlyContinue) {
    throw "Xournal++ esta aberto; feche antes de atualizar o dist (instancia antiga roda o binario velho)"
}

Copy-Item -LiteralPath $builtExe -Destination $deployExe -Force
Write-Host "Deploy ok: $deployExe"
