Set-Location $PSScriptRoot

if (-not $env:TEMP) { $env:TEMP = [System.IO.Path]::GetTempPath().TrimEnd('\') }
if (-not $env:TMP) { $env:TMP = $env:TEMP }

$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio 11.0\VC\vcvarsall.bat"
if (-not (Test-Path $vcvars)) {
    if ($env:VS110COMNTOOLS -and (Test-Path "$env:VS110COMNTOOLS\..\..\VC\vcvarsall.bat")) {
        $vcvars = (Resolve-Path "$env:VS110COMNTOOLS\..\..\VC\vcvarsall.bat").Path
    }
}

if (-not (Test-Path $vcvars)) {
    Write-Host "Error: Visual Studio 2012 vcvarsall.bat not found!" -ForegroundColor Red
    exit 1
}

Write-Host "=== Compiling CATPart批量赋色.exe ===" -ForegroundColor Cyan
$cmd = "call `"$vcvars`" amd64 && " + `
       'rc.exe /nologo /fo app.res app.rc && ' + `
       'cl /nologo /EHsc /O2 /c ColorTool.cpp /FoColorTool.obj && ' + `
       'cl /nologo /EHsc /O2 /c pipeline.cpp /Fopipeline.obj && ' + `
       'link /nologo ColorTool.obj pipeline.obj app.res user32.lib gdi32.lib comdlg32.lib comctl32.lib shell32.lib ole32.lib advapi32.lib /SUBSYSTEM:WINDOWS /OUT:"..\..\CATPart批量赋色.exe"'

cmd /c $cmd
if ($LASTEXITCODE -eq 0) {
    Write-Host "=== Build Succeeded! ===" -ForegroundColor Green
} else {
    Write-Host "=== Build Failed with code $LASTEXITCODE ===" -ForegroundColor Red
    exit $LASTEXITCODE
}
