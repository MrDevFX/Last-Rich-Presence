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

function Test-BuildOutputLocked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [Parameter(Mandatory = $true)]
        [string]$Configuration,

        [Parameter(Mandatory = $true)]
        [string]$Platform
    )

    $candidateExe = Join-Path $RepoRoot "$Platform\$Configuration\Last Rich Presence\Last_Rich_Presence.exe"
    if (-not (Test-Path $candidateExe)) {
        return
    }

    $resolvedCandidate = (Resolve-Path $candidateExe).Path
    $runningInstances = @(
        Get-Process -Name Last_Rich_Presence -ErrorAction SilentlyContinue |
            Where-Object {
                try {
                    $_.Path -and ([System.StringComparer]::OrdinalIgnoreCase.Equals($_.Path, $resolvedCandidate))
                }
                catch {
                    $false
                }
            }
    )

    if ($runningInstances.Count -eq 0) {
        return
    }

    $details = $runningInstances | ForEach-Object {
        "$($_.ProcessName) (PID $($_.Id)) -> $($_.Path)"
    }

    throw "Build output is locked by a running app instance:`n$($details -join [Environment]::NewLine)`nStop the running app and rerun verify.ps1."
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

Test-BuildOutputLocked -RepoRoot $repoRoot -Configuration $Configuration -Platform $Platform

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

    $jsFiles = @(Get-ChildItem -Path $extensionDir -Recurse -Filter *.js -File | Sort-Object FullName)
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

Invoke-Step -Name "Native tests" -Action {
    $testProjects = @(Get-ChildItem -Path $repoRoot -Recurse -Include *Test*.vcxproj, *Tests*.vcxproj -File)
    $ctestFiles = @(Get-ChildItem -Path $repoRoot -Recurse -Filter CTestTestfile.cmake -File)

    if ($testProjects.Count -eq 0 -and $ctestFiles.Count -eq 0) {
        Write-Host "No native test projects were found. Build + JS checks are used as smoke verification."
        return
    }

    foreach ($project in $testProjects) {
        $projectName = [System.IO.Path]::GetFileNameWithoutExtension($project.Name)
        $candidateExe = Join-Path $project.DirectoryName "bin\$Platform\$Configuration\$projectName.exe"
        if (-not (Test-Path $candidateExe)) {
            $candidateExe = Get-ChildItem -Path $project.DirectoryName -Recurse -Filter "$projectName.exe" -File |
                Sort-Object LastWriteTime -Descending |
                Select-Object -First 1 -ExpandProperty FullName
        }

        if (-not $candidateExe -or -not (Test-Path $candidateExe)) {
            throw "Could not locate built test executable for $($project.FullName)"
        }

        Write-Host "Running $candidateExe"
        & $candidateExe
        if ($LASTEXITCODE -ne 0) {
            throw "Native test run failed: $candidateExe"
        }
    }

    foreach ($ctest in $ctestFiles) {
        Write-Host "Found CTest metadata at $($ctest.FullName), but no CTest runner is configured in verify.ps1."
    }
}

Write-Host ""
Write-Host "Verification complete."
