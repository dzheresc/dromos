# Windows wrapper for the 7-series builds on the Dell (Vivado 2017.4 there has 7-series only).
#   .\scripts\vivado.ps1 k7 build.tcl
param([Parameter(Mandatory)][string]$Board, [Parameter(Mandatory)][string]$Script,
      [Parameter(ValueFromRemainingArguments)][string[]]$Rest)
$ErrorActionPreference = 'Stop'
$vivado = if ($env:VIVADO_BAT) { $env:VIVADO_BAT } else { 'C:\Xilinx\Vivado\2017.4\bin\vivado.bat' }
$dir = Join-Path (Split-Path $PSScriptRoot) "boards\$Board"
Push-Location $dir
try {
  $args = @('-mode', 'batch', '-nojournal', '-notrace', '-source', $Script)
  if ($Rest) { $args += '-tclargs'; $args += $Rest }
  & $vivado @args
  if ($LASTEXITCODE) { exit $LASTEXITCODE }
} finally { Pop-Location }
