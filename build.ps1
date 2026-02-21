# Build TalkMe client (Release x64). Uses MSBuild from Visual Studio via vswhere if not on PATH.
$ErrorActionPreference = "Stop"
$projectRoot = $PSScriptRoot
$vcxproj = Join-Path $projectRoot "TalkMe.vcxproj"

# Find MSBuild: prefer PATH, then vswhere
$msbuild = $null
if (Get-Command msbuild -ErrorAction SilentlyContinue) {
    $msbuild = (Get-Command msbuild).Source
} else {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" 2>$null
        if ($found) { $msbuild = $found.Trim() }
    }
}
if (-not $msbuild -or -not (Test-Path $msbuild)) {
    Write-Error "MSBuild not found. Install Visual Studio with C++ workload or add MSBuild to PATH."
    exit 1
}

Push-Location $projectRoot
try {
    & $msbuild $vcxproj /p:Configuration=Release /p:Platform=x64 /t:Build /v:minimal
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $exe = Join-Path $projectRoot "x64\Release\TalkMe.exe"
    if (Test-Path $exe) {
        Write-Host "Built: $exe"
    }
} finally {
    Pop-Location
}
