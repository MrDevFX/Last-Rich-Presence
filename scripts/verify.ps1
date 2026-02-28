[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "Release-Inno", "Release-MSIX")]
    [string]$Configuration = "Debug",

    [ValidateSet("x64", "x86", "ARM64")]
    [string]$Platform = "x64"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Find-MSBuild {
    $candidates = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\17\Enterprise\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\17\Professional\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\17\Community\MSBuild\Current\Bin\MSBuild.exe"
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return $candidate
        }
    }

    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vsWhere) {
        $found = & $vsWhere -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1
        if ($found) {
            return $found
        }
    }

    throw "MSBuild.exe was not found. Install Visual Studio Build Tools or Visual Studio with MSBuild."
}

function Invoke-Step {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [scriptblock]$Action
    )

    Write-Host ""
    Write-Host "=== $Name ==="
    & $Action
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$solutionPath = Join-Path $repoRoot "Last Rich Presence.sln"

if (-not (Test-Path $solutionPath)) {
    throw "Solution file not found: $solutionPath"
}

$msbuildPath = Find-MSBuild

Write-Host "Repo: $repoRoot"
Write-Host "MSBuild: $msbuildPath"
Write-Host "Configuration: $Configuration"
Write-Host "Platform: $Platform"

Invoke-Step -Name "Build solution" -Action {
    & $msbuildPath $solutionPath -t:Build "-p:Configuration=$Configuration" "-p:Platform=$Platform" -m
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE"
    }
}

Invoke-Step -Name "Extension JS syntax checks" -Action {
    $extensionDir = Join-Path $repoRoot "browser-extension"
    if (-not (Test-Path $extensionDir)) {
        Write-Host "browser-extension folder not found, skipping JS checks."
        return
    }

    $node = Get-Command node -ErrorAction SilentlyContinue
    if (-not $node) {
        throw "Node.js is required for JS checks but 'node' was not found on PATH."
    }

    $jsFiles = @(Get-ChildItem -Path $extensionDir -Filter *.js -File | Sort-Object Name)
    if ($jsFiles.Count -eq 0) {
        Write-Host "No JS files found, skipping JS checks."
        return
    }

    foreach ($jsFile in $jsFiles) {
        Write-Host "Checking $($jsFile.Name)"
        & node --check $jsFile.FullName
        if ($LASTEXITCODE -ne 0) {
            throw "JavaScript syntax check failed: $($jsFile.FullName)"
        }
    }
}

Invoke-Step -Name "Test discovery" -Action {
    $testProjects = @(Get-ChildItem -Path $repoRoot -Recurse -Include *Test*.vcxproj, *Tests*.vcxproj -File)
    $ctestFiles = @(Get-ChildItem -Path $repoRoot -Recurse -Filter CTestTestfile.cmake -File)

    if ($testProjects.Count -eq 0 -and $ctestFiles.Count -eq 0) {
        Write-Host "No native test projects were found. Build + JS checks are used as smoke verification."
        return
    }

    Write-Host "Found test artifacts:"
    foreach ($project in $testProjects) {
        Write-Host " - $($project.FullName)"
    }
    foreach ($ctest in $ctestFiles) {
        Write-Host " - $($ctest.FullName)"
    }

    throw "Test artifacts exist but this script has no runner configured for them yet."
}

Write-Host ""
Write-Host "Verification complete."
