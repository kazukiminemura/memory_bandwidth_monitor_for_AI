param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$MSBuildArgs
)

$ErrorActionPreference = "Stop"

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe was not found. Install Visual Studio or Build Tools first."
}

$installPath = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
if (-not $installPath) {
    throw "No Visual Studio instance with MSBuild component was found."
}

$candidates = @(
    (Join-Path $installPath "MSBuild\Current\Bin\amd64\MSBuild.exe"),
    (Join-Path $installPath "MSBuild\Current\Bin\MSBuild.exe")
)

$msbuildExe = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $msbuildExe) {
    throw "MSBuild.exe was not found under: $installPath"
}

# Add MSBuild location for the current shell session.
$env:Path = "$(Split-Path $msbuildExe -Parent);$env:Path"

if ($MSBuildArgs.Count -eq 0) {
    & $msbuildExe -version
    exit $LASTEXITCODE
}

& $msbuildExe @MSBuildArgs
exit $LASTEXITCODE