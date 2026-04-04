[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "Release-Inno", "Release-MSIX")]
    [string]$Configuration = "Debug",

    [ValidateSet("x64", "x86", "ARM64")]
    [string]$Platform = "x64",

    [switch]$SkipLaunchSmoke
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

function Find-NuGet {
    $commands = @(
        (Get-Command nuget.exe -ErrorAction SilentlyContinue),
        (Get-Command nuget -ErrorAction SilentlyContinue)
    ) | Where-Object { $_ }

    foreach ($command in $commands) {
        if ($command.Source -and (Test-Path $command.Source)) {
            return $command.Source
        }
    }

    $candidates = @(
        "${env:ProgramFiles}\NuGet\nuget.exe",
        "${env:ProgramFiles(x86)}\NuGet\nuget.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\17\Community\Common7\IDE\CommonExtensions\Microsoft\NuGet\NuGet.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\17\Professional\Common7\IDE\CommonExtensions\Microsoft\NuGet\NuGet.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\17\Enterprise\Common7\IDE\CommonExtensions\Microsoft\NuGet\NuGet.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\NuGet\NuGet.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\NuGet\NuGet.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\NuGet\NuGet.exe"
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return $candidate
        }
    }

    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vsWhere) {
        $found = & $vsWhere -latest -products * -find Common7\IDE\CommonExtensions\Microsoft\NuGet\NuGet.exe | Select-Object -First 1
        if ($found) {
            return $found
        }
    }

    return $null
}

function Find-InnoCompiler {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return $candidate
        }
    }

    return $null
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

function Get-ExpectedAppOutputExePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [Parameter(Mandatory = $true)]
        [string]$Configuration,

        [Parameter(Mandatory = $true)]
        [string]$Platform
    )

    Join-Path $RepoRoot "$Platform\$Configuration\Last Rich Presence\Last_Rich_Presence.exe"
}

function Get-ExpectedAppOutputExe {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [Parameter(Mandatory = $true)]
        [string]$Configuration,

        [Parameter(Mandatory = $true)]
        [string]$Platform
    )

    $expectedExe = Get-ExpectedAppOutputExePath -RepoRoot $RepoRoot -Configuration $Configuration -Platform $Platform
    if (-not (Test-Path $expectedExe)) {
        throw "Expected built app executable was not found: $expectedExe"
    }

    return (Resolve-Path $expectedExe).Path
}

function Get-ExpectedTestExecutable {
    param(
        [Parameter(Mandatory = $true)]
        [System.IO.FileInfo]$Project,

        [Parameter(Mandatory = $true)]
        [string]$Configuration,

        [Parameter(Mandatory = $true)]
        [string]$Platform
    )

    $projectName = [System.IO.Path]::GetFileNameWithoutExtension($Project.Name)
    $expectedExe = Join-Path $Project.DirectoryName "bin\$Platform\$Configuration\$projectName.exe"
    if (-not (Test-Path $expectedExe)) {
        throw "Expected built test executable was not found: $expectedExe"
    }

    return (Resolve-Path $expectedExe).Path
}

function Get-ProcessExecutablePath {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process
    )

    try {
        if ($Process.Path) {
            return $Process.Path
        }
    }
    catch {}

    try {
        if ($Process.MainModule -and $Process.MainModule.FileName) {
            return $Process.MainModule.FileName
        }
    }
    catch {}

    try {
        $pid = $Process.Id
        $cim = Get-CimInstance Win32_Process -Filter "ProcessId = $pid" -ErrorAction Stop
        if ($cim.ExecutablePath) {
            return $cim.ExecutablePath
        }
    }
    catch {}

    return $null
}

function Get-RunningAppInstances {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExePath
    )

    $resolvedExePath = (Resolve-Path $ExePath).Path
    @(
        Get-Process -Name Last_Rich_Presence -ErrorAction SilentlyContinue |
            ForEach-Object {
                $process = $_
                $processPath = Get-ProcessExecutablePath -Process $process
                if (-not $processPath) {
                    return
                }

                if (-not [System.StringComparer]::OrdinalIgnoreCase.Equals($processPath, $resolvedExePath)) {
                    return
                }

                [pscustomobject]@{
                    Id = $process.Id
                    ProcessName = $process.ProcessName
                    MainWindowTitle = $process.MainWindowTitle
                    Path = $processPath
                }
            }
    )
}

function Wait-ForRunningAppInstance {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExePath,

        [int]$TimeoutMs = 15000
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        $instances = @(Get-RunningAppInstances -ExePath $ExePath)
        if ($instances.Count -gt 0) {
            return $instances
        }

        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)

    return @()
}

function Wait-ForMainWindowTitle {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExePath,

        [Parameter(Mandatory = $true)]
        [string]$WindowTitle,

        [int]$TimeoutMs = 15000
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        $matching = @(Get-RunningAppInstances -ExePath $ExePath |
            Where-Object { $_.MainWindowTitle -eq $WindowTitle })
        if ($matching.Count -gt 0) {
            return $matching
        }

        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)

    return @()
}

function Wait-ForStableMainWindowTitle {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExePath,

        [Parameter(Mandatory = $true)]
        [string]$WindowTitle,

        [int]$TimeoutMs = 15000,

        [int]$StableForMs = 2000
    )

    $matching = @(Wait-ForMainWindowTitle -ExePath $ExePath -WindowTitle $WindowTitle -TimeoutMs $TimeoutMs)
    if ($matching.Count -eq 0) {
        return @()
    }

    Start-Sleep -Milliseconds $StableForMs
    return @(Get-RunningAppInstances -ExePath $ExePath |
        Where-Object { $_.MainWindowTitle -eq $WindowTitle })
}

function Wait-ForSingleOriginalAppInstance {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExePath,

        [Parameter(Mandatory = $true)]
        [int]$OriginalPid,

        [int]$TimeoutMs = 10000
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        $instances = @(Get-RunningAppInstances -ExePath $ExePath)
        if ($instances.Count -eq 1 -and $instances[0].Id -eq $OriginalPid) {
            return $true
        }

        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)

    return $false
}

function Stop-RunningAppInstances {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExePath
    )

    foreach ($instance in (Get-RunningAppInstances -ExePath $ExePath)) {
        try {
            Stop-Process -Id $instance.Id -Force -ErrorAction Stop
        }
        catch {
            Write-Warning "Failed to stop smoke-test instance PID $($instance.Id): $($_.Exception.Message)"
        }
    }
}

function Assert-NoRunningAppInstances {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExePath,

        [Parameter(Mandatory = $true)]
        [string]$Context,

        [int]$TimeoutMs = 0
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        $instances = @(Get-RunningAppInstances -ExePath $ExePath)
        if ($instances.Count -eq 0) {
            return
        }

        if ($TimeoutMs -le 0 -or [DateTime]::UtcNow -ge $deadline) {
            break
        }

        Start-Sleep -Milliseconds 250
    } while ($true)

    $details = $instances | ForEach-Object {
        "$($_.ProcessName) (PID $($_.Id)) -> $($_.Path)"
    }

    throw "$Context`n$($details -join [Environment]::NewLine)"
}

function Format-AppInstanceState {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExePath
    )

    $instances = @(Get-RunningAppInstances -ExePath $ExePath)
    if ($instances.Count -eq 0) {
        return "<no matching running process>"
    }

    return ($instances | ForEach-Object {
        $title = if ([string]::IsNullOrWhiteSpace($_.MainWindowTitle)) { "<no window title>" } else { $_.MainWindowTitle }
        "$($_.ProcessName) (PID $($_.Id)) title=$title path=$($_.Path)"
    }) -join [Environment]::NewLine
}

function Test-BuildOutputLocked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExePath
    )

    if (-not (Test-Path $ExePath)) {
        return
    }

    $resolvedCandidate = (Resolve-Path $ExePath).Path
    $runningInstances = @(Get-RunningAppInstances -ExePath $resolvedCandidate)
    if ($runningInstances.Count -eq 0) {
        return
    }

    $details = $runningInstances | ForEach-Object {
        "$($_.ProcessName) (PID $($_.Id)) -> $($_.Path)"
    }

    throw "Build output is locked by a running app instance:`n$($details -join [Environment]::NewLine)`nStop the running app and rerun verify.ps1."
}

function Invoke-UnpackagedLaunchSmoke {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExePath
    )

    Write-Host "Launching $ExePath"

    Stop-RunningAppInstances -ExePath $ExePath
    Assert-NoRunningAppInstances -ExePath $ExePath -Context "Pre-launch cleanup failed: a previous app instance is still running." -TimeoutMs 10000

    try {
        $primary = Start-Process -FilePath $ExePath -WorkingDirectory (Split-Path -Parent $ExePath) -PassThru
        $instances = @(Wait-ForRunningAppInstance -ExePath $ExePath -TimeoutMs 20000)
        if ($instances.Count -eq 0) {
            throw "Launch smoke failed: the app did not remain running.`n$(Format-AppInstanceState -ExePath $ExePath)"
        }

        $windowed = @(Wait-ForStableMainWindowTitle -ExePath $ExePath -WindowTitle "Last Rich Presence" -TimeoutMs 20000)
        if ($windowed.Count -eq 0) {
            Write-Host "Initial launch appears hidden to tray; starting a second instance to request restore."
            Start-Process -FilePath $ExePath -WorkingDirectory (Split-Path -Parent $ExePath) -PassThru | Out-Null

            $windowed = @(Wait-ForStableMainWindowTitle -ExePath $ExePath -WindowTitle "Last Rich Presence" -TimeoutMs 30000)
            if ($windowed.Count -eq 0) {
                throw "Launch smoke failed: no top-level 'Last Rich Presence' window appeared after first or second launch.`n$(Format-AppInstanceState -ExePath $ExePath)"
            }

            if (-not (Wait-ForSingleOriginalAppInstance -ExePath $ExePath -OriginalPid $primary.Id -TimeoutMs 10000)) {
                throw "Launch smoke failed: second launch did not restore the original single app instance."
            }
        }

        Write-Host "Launch smoke confirmed a healthy running process and top-level window."
    }
    finally {
        Stop-RunningAppInstances -ExePath $ExePath
        Assert-NoRunningAppInstances -ExePath $ExePath -Context "Launch smoke cleanup failed: the smoke-test app instance is still running." -TimeoutMs 10000
    }
}

function Get-PackagedManifestInfo {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot
    )

    $manifestPath = Join-Path $RepoRoot "Package.appxmanifest"
    if (-not (Test-Path $manifestPath)) {
        throw "Package.appxmanifest was not found: $manifestPath"
    }

    [xml]$manifest = Get-Content $manifestPath
    $namespace = New-Object System.Xml.XmlNamespaceManager($manifest.NameTable)
    $namespace.AddNamespace("appx", "http://schemas.microsoft.com/appx/manifest/foundation/windows10")

    $identityNode = $manifest.SelectSingleNode("/appx:Package/appx:Identity", $namespace)
    $propertiesNode = $manifest.SelectSingleNode("/appx:Package/appx:Properties", $namespace)
    $applicationNode = $manifest.SelectSingleNode("/appx:Package/appx:Applications/appx:Application", $namespace)

    if (-not $identityNode -or -not $propertiesNode -or -not $applicationNode) {
        throw "Package.appxmanifest is missing identity, properties, or application metadata."
    }

    $displayNameNode = $propertiesNode.SelectSingleNode("appx:DisplayName", $namespace)
    if (-not $displayNameNode) {
        throw "Package.appxmanifest is missing the display name node."
    }

    return [pscustomobject]@{
        IdentityName = $identityNode.GetAttribute("Name")
        IdentityVersion = $identityNode.GetAttribute("Version")
        DisplayName = $displayNameNode.InnerText
        ApplicationId = $applicationNode.GetAttribute("Id")
        Executable = $applicationNode.GetAttribute("Executable")
    }
}

function Get-PackagedArtifactState {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot
    )

    $appPackagesRoot = Join-Path $RepoRoot "AppPackages"
    $state = @{}
    if (-not (Test-Path $appPackagesRoot)) {
        return $state
    }

    Get-ChildItem -Path $appPackagesRoot -Recurse -File -Include *.msix, *.appx, *.msixbundle, *.appxbundle |
        Where-Object { $_.FullName -notlike "*\Dependencies\*" } |
        ForEach-Object {
            $state[$_.FullName] = $_.LastWriteTimeUtc.Ticks
        }

    return $state
}

function Get-PackagedBuildArtifacts {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [Parameter(Mandatory = $true)]
        [pscustomobject]$ManifestInfo,

        [Parameter(Mandatory = $true)]
        [string]$Platform
    )

    $appPackagesRoot = Join-Path $RepoRoot "AppPackages"
    if (-not (Test-Path $appPackagesRoot)) {
        throw "AppPackages output root was not found: $appPackagesRoot"
    }

    $candidateDirectories = @(
        Get-ChildItem -Path $appPackagesRoot -Directory |
            Where-Object { $_.Name -like "$($ManifestInfo.DisplayName)_$($ManifestInfo.IdentityVersion)_${Platform}_*" }
    )
    $displayDirectory = Join-Path $appPackagesRoot $ManifestInfo.DisplayName
    if (Test-Path $displayDirectory) {
        $candidateDirectories += @(
            Get-ChildItem -Path $displayDirectory -Directory |
                Where-Object { $_.Name -like "*_$($ManifestInfo.IdentityVersion)_${Platform}_*" }
        )
    }
    if ($candidateDirectories.Count -eq 0) {
        throw "No packaged output directory matched version $($ManifestInfo.IdentityVersion) for $Platform under $appPackagesRoot"
    }

    $packageDirectory = $candidateDirectories | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    $installScript = @(
        Get-ChildItem -Path $packageDirectory.FullName -Filter Add-AppDevPackage.ps1 -File -Recurse |
            Sort-Object LastWriteTimeUtc -Descending |
            Select-Object -First 1
    )
    $packageFile = @(
        Get-ChildItem -Path $packageDirectory.FullName -Recurse -File |
            Where-Object {
                $_.Extension -in ".msix", ".appx", ".msixbundle", ".appxbundle" -and
                $_.FullName -notlike "*\Dependencies\*"
            } |
            Sort-Object LastWriteTimeUtc -Descending |
            Select-Object -First 1
    )
    $certificateFile = @(
        Get-ChildItem -Path $packageDirectory.FullName -Recurse -File -Include *.cer |
            Sort-Object LastWriteTimeUtc -Descending |
            Select-Object -First 1
    )

    if ($installScript.Count -eq 0 -and $packageFile.Count -eq 0) {
        throw "No Add-AppDevPackage.ps1 or package artifact was found under $($packageDirectory.FullName)"
    }

    $dependenciesRoot = Join-Path $packageDirectory.FullName "Dependencies"
    $dependencyPackageFiles = @()
    if (Test-Path $dependenciesRoot) {
        $dependencyPackageFiles = @(
            Get-ChildItem -Path $dependenciesRoot -Recurse -File |
                Where-Object { $_.Extension -in ".msix", ".appx", ".msixbundle", ".appxbundle" } |
                Sort-Object FullName |
                ForEach-Object { $_.FullName }
        )
    }

    [pscustomobject]@{
        PackageDirectory = $packageDirectory.FullName
        InstallScript = if ($installScript.Count -gt 0) { $installScript[0].FullName } else { $null }
        PackageFile = if ($packageFile.Count -gt 0) { $packageFile[0].FullName } else { $null }
        CertificateFile = if ($certificateFile.Count -gt 0) { $certificateFile[0].FullName } else { $null }
        DependencyPackageFiles = $dependencyPackageFiles
    }
}

function Get-ExpectedInstallerArtifactPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot
    )

    Join-Path $RepoRoot "dist\LastRichPresence-Setup-x64.exe"
}

function Invoke-InnoInstallerBuildValidation {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [string]$CompilerPath
    )

    if (-not $CompilerPath) {
        Write-Warning "Inno Setup 6 was not found on this machine; skipping installer artifact validation."
        return
    }

    $scriptPath = Join-Path $RepoRoot "installer\LastRichPresence.iss"
    if (-not (Test-Path $scriptPath)) {
        throw "Inno Setup script was not found: $scriptPath"
    }

    $artifactPath = Get-ExpectedInstallerArtifactPath -RepoRoot $RepoRoot
    $priorTicks = 0
    if (Test-Path $artifactPath) {
        $priorTicks = (Get-Item $artifactPath).LastWriteTimeUtc.Ticks
    }

    & $CompilerPath $scriptPath
    if ($LASTEXITCODE -ne 0) {
        throw "Inno Setup compiler failed with exit code $LASTEXITCODE"
    }

    if (-not (Test-Path $artifactPath)) {
        throw "Expected installer artifact was not found after Inno compilation: $artifactPath"
    }

    $artifact = Get-Item $artifactPath
    if ($priorTicks -gt 0 -and $artifact.LastWriteTimeUtc.Ticks -lt $priorTicks) {
        throw "Installer artifact validation resolved an older output than the pre-validation artifact: $artifactPath"
    }

    Write-Host "Resolved installer artifact: $($artifact.FullName)"
}

function Get-PackageCertificateThumbprints {
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Artifacts
    )

    if (-not $Artifacts.PackageDirectory -or -not (Test-Path $Artifacts.PackageDirectory)) {
        return @()
    }

    $thumbprints = @()
    $certificateFiles = @(
        Get-ChildItem -Path $Artifacts.PackageDirectory -Recurse -File -Filter *.cer |
            Where-Object { $_.FullName -notlike "*\Dependencies\*" }
    )

    foreach ($certificateFile in $certificateFiles) {
        try {
            $certificate = Get-PfxCertificate $certificateFile.FullName
            if ($certificate -and $certificate.Thumbprint) {
                $thumbprints += $certificate.Thumbprint.ToUpperInvariant()
            }
        }
        catch {
            Write-Warning "Failed to inspect package certificate '$($certificateFile.FullName)': $($_.Exception.Message)"
        }
    }

    return @($thumbprints | Sort-Object -Unique)
}

function Get-CertificateStoreSnapshot {
    param(
        [string[]]$Thumbprints
    )

    if (-not $Thumbprints -or $Thumbprints.Count -eq 0) {
        return @()
    }

    $stores = @(
        "Cert:\CurrentUser\TrustedPeople",
        "Cert:\CurrentUser\Root"
    )
    $snapshot = @()

    foreach ($thumbprint in ($Thumbprints | Sort-Object -Unique)) {
        foreach ($storePath in $stores) {
            $present = @(Get-ChildItem -Path $storePath -ErrorAction SilentlyContinue |
                Where-Object { $_.Thumbprint -eq $thumbprint }).Count -gt 0
            $snapshot += [pscustomobject]@{
                StorePath = $storePath
                Thumbprint = $thumbprint
                Present = $present
            }
        }
    }

    return $snapshot
}

function Remove-CertificatesAddedAfterSnapshot {
    param(
        [object[]]$Snapshot
    )

    if (-not $Snapshot -or $Snapshot.Count -eq 0) {
        return
    }

    foreach ($entry in $Snapshot) {
        if ($entry.Present) {
            continue
        }

        $matches = @(
            Get-ChildItem -Path $entry.StorePath -ErrorAction SilentlyContinue |
                Where-Object { $_.Thumbprint -eq $entry.Thumbprint }
        )
        foreach ($match in $matches) {
            try {
                Remove-Item -LiteralPath $match.PSPath -Force -ErrorAction Stop
                Write-Host "Removed smoke-test certificate $($entry.Thumbprint) from $($entry.StorePath)"
            }
            catch {
                Write-Warning "Failed to remove smoke-test certificate $($entry.Thumbprint) from $($entry.StorePath): $($_.Exception.Message)"
            }
        }
    }
}

function Remove-InstalledPackageIfPresent {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PackageName,

        [string]$ExecutableRelativePath
    )

    $packages = @(Get-AppxPackage -Name $PackageName -ErrorAction SilentlyContinue)
    foreach ($package in $packages) {
        if (-not $ExecutableRelativePath) {
            continue
        }

        $candidateExe = Join-Path $package.InstallLocation $ExecutableRelativePath
        if (-not (Test-Path $candidateExe)) {
            continue
        }

        try {
            Stop-RunningAppInstances -ExePath (Resolve-Path $candidateExe).Path
        }
        catch {}
    }

    foreach ($package in $packages) {
        Write-Host "Removing existing package $($package.PackageFullName)"
        Remove-AppxPackage -Package $package.PackageFullName -ErrorAction Stop
    }

    if ($packages.Count -eq 0) {
        return
    }

    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        if (@(Get-AppxPackage -Name $PackageName -ErrorAction SilentlyContinue).Count -eq 0) {
            return
        }

        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)

    throw "Timed out waiting for package '$PackageName' to uninstall."
}

function Get-InstalledPackageDetails {
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$ManifestInfo
    )

    $package = @(Get-AppxPackage -Name $ManifestInfo.IdentityName -ErrorAction SilentlyContinue |
        Sort-Object Version -Descending |
        Select-Object -First 1)
    if ($package.Count -eq 0) {
        throw "The package '$($ManifestInfo.IdentityName)' is not installed after the install step."
    }

    $installedPackage = $package[0]
    $exePath = Join-Path $installedPackage.InstallLocation $ManifestInfo.Executable
    if (-not (Test-Path $exePath)) {
        throw "Installed package executable was not found: $exePath"
    }

    [pscustomobject]@{
        PackageFullName = $installedPackage.PackageFullName
        PackageFamilyName = $installedPackage.PackageFamilyName
        ExePath = (Resolve-Path $exePath).Path
        AppUserModelId = "$($installedPackage.PackageFamilyName)!$($ManifestInfo.ApplicationId)"
    }
}

function Install-PackagedArtifact {
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Artifacts
    )

    if ($Artifacts.InstallScript) {
        Write-Host "Installing package via $($Artifacts.InstallScript)"
        powershell.exe -ExecutionPolicy Bypass -File $Artifacts.InstallScript -Force
        if ($LASTEXITCODE -ne 0) {
            throw "Add-AppDevPackage.ps1 failed with exit code $LASTEXITCODE"
        }

        return
    }

    Write-Host "Installing package via Add-AppxPackage from $($Artifacts.PackageFile)"
    if ($Artifacts.DependencyPackageFiles -and $Artifacts.DependencyPackageFiles.Count -gt 0) {
        Add-AppxPackage -Path $Artifacts.PackageFile -DependencyPath $Artifacts.DependencyPackageFiles -ForceApplicationShutdown -ErrorAction Stop
        return
    }

    Add-AppxPackage -Path $Artifacts.PackageFile -ForceApplicationShutdown -ErrorAction Stop
}

function Start-PackagedAppByIdentity {
    param(
        [Parameter(Mandatory = $true)]
        [string]$AppUserModelId
    )

    Start-Process explorer.exe -ArgumentList "shell:AppsFolder\$AppUserModelId" | Out-Null
}

function Invoke-PackagedLaunchSmoke {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [Parameter(Mandatory = $true)]
        [string]$Platform
    )

    $manifestInfo = Get-PackagedManifestInfo -RepoRoot $RepoRoot
    $artifacts = Get-PackagedBuildArtifacts -RepoRoot $RepoRoot -ManifestInfo $manifestInfo -Platform $Platform
    $certificateSnapshot = Get-CertificateStoreSnapshot -Thumbprints (Get-PackageCertificateThumbprints -Artifacts $artifacts)

    Remove-InstalledPackageIfPresent -PackageName $manifestInfo.IdentityName -ExecutableRelativePath $manifestInfo.Executable

    $installedPackage = $null
    try {
        Install-PackagedArtifact -Artifacts $artifacts
        $installedPackage = Get-InstalledPackageDetails -ManifestInfo $manifestInfo

        Write-Host "Launching packaged app $($installedPackage.AppUserModelId)"
        Start-PackagedAppByIdentity -AppUserModelId $installedPackage.AppUserModelId

        $instances = @(Wait-ForRunningAppInstance -ExePath $installedPackage.ExePath -TimeoutMs 30000)
        if ($instances.Count -eq 0) {
            throw "Packaged launch smoke failed: the app did not remain running.`n$(Format-AppInstanceState -ExePath $installedPackage.ExePath)"
        }

        $primaryPid = $instances[0].Id
        $windowed = @(Wait-ForStableMainWindowTitle -ExePath $installedPackage.ExePath -WindowTitle "Last Rich Presence" -TimeoutMs 30000)
        if ($windowed.Count -eq 0) {
            Write-Host "Initial packaged launch appears hidden to tray; starting a second activation to request restore."
            Start-PackagedAppByIdentity -AppUserModelId $installedPackage.AppUserModelId

            $windowed = @(Wait-ForStableMainWindowTitle -ExePath $installedPackage.ExePath -WindowTitle "Last Rich Presence" -TimeoutMs 35000)
            if ($windowed.Count -eq 0) {
                throw "Packaged launch smoke failed: no top-level 'Last Rich Presence' window appeared after first or second launch.`n$(Format-AppInstanceState -ExePath $installedPackage.ExePath)"
            }

            if (-not (Wait-ForSingleOriginalAppInstance -ExePath $installedPackage.ExePath -OriginalPid $primaryPid -TimeoutMs 15000)) {
                throw "Packaged launch smoke failed: second activation did not restore the original single app instance."
            }
        }

        Write-Host "Packaged launch smoke confirmed a healthy running process and top-level window."
    }
    finally {
        try {
            if ($installedPackage) {
                Stop-RunningAppInstances -ExePath $installedPackage.ExePath
                Assert-NoRunningAppInstances -ExePath $installedPackage.ExePath -Context "Packaged launch smoke cleanup failed: the smoke-test app instance is still running." -TimeoutMs 10000
            }
        }
        finally {
            try {
                Remove-InstalledPackageIfPresent -PackageName $manifestInfo.IdentityName -ExecutableRelativePath $manifestInfo.Executable
            }
            finally {
                Remove-CertificatesAddedAfterSnapshot -Snapshot $certificateSnapshot
            }
        }
    }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$solutionPath = Join-Path $repoRoot "Last Rich Presence.sln"
if (-not (Test-Path $solutionPath)) {
    throw "Solution file not found: $solutionPath"
}

$msbuildPath = Find-MSBuild
$nugetPath = Find-NuGet
$innoCompilerPath = Find-InnoCompiler

Write-Host "Repo: $repoRoot"
Write-Host "MSBuild: $msbuildPath"
Write-Host "NuGet: $(if ($nugetPath) { $nugetPath } else { '<fallback: MSBuild Restore>' })"
Write-Host "Inno Setup: $(if ($innoCompilerPath) { $innoCompilerPath } else { '<not found>' })"
Write-Host "Configuration: $Configuration"
Write-Host "Platform: $Platform"

$expectedExe = Get-ExpectedAppOutputExePath -RepoRoot $repoRoot -Configuration $Configuration -Platform $Platform
Test-BuildOutputLocked -ExePath $expectedExe

Invoke-Step -Name "Restore packages" -Action {
    if ($nugetPath) {
        & $nugetPath restore $solutionPath
        if ($LASTEXITCODE -ne 0) {
            throw "NuGet restore failed with exit code $LASTEXITCODE"
        }

        return
    }

    Write-Host "NuGet.exe was not found; using MSBuild restore fallback for this machine."
    & $msbuildPath $solutionPath -t:Restore "-p:RestorePackagesConfig=true" "-p:Configuration=$Configuration" "-p:Platform=$Platform" -m
    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild restore fallback failed with exit code $LASTEXITCODE"
    }
}

Invoke-Step -Name "Build solution" -Action {
    if ($Configuration -eq "Release-MSIX") {
        & $msbuildPath $solutionPath -t:Build "-p:Configuration=$Configuration" "-p:Platform=$Platform" "-p:GenerateAppxPackageOnBuild=true" "-p:AppxPackageDir=$repoRoot\\AppPackages\\" -m
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed with exit code $LASTEXITCODE"
        }

        return
    }

    & $msbuildPath $solutionPath -t:Build "-p:Configuration=$Configuration" "-p:Platform=$Platform" -m
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE"
    }
}

Invoke-Step -Name "Release artifact validation" -Action {
    if ($Configuration -eq "Release-Inno") {
        Invoke-InnoInstallerBuildValidation -RepoRoot $repoRoot -CompilerPath $innoCompilerPath
        return
    }

    if ($Configuration -eq "Release-MSIX") {
        $manifestInfo = Get-PackagedManifestInfo -RepoRoot $repoRoot
        $artifacts = Get-PackagedBuildArtifacts -RepoRoot $repoRoot -ManifestInfo $manifestInfo -Platform $Platform
        if ($artifacts.PackageFile) {
            Write-Host "Resolved packaged artifact: $($artifacts.PackageFile)"
        }
        elseif ($artifacts.InstallScript) {
            Write-Host "Resolved packaged install helper: $($artifacts.InstallScript)"
        }
        return
    }

    Write-Host "This configuration does not produce a release installer/package artifact."
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
        $candidateExe = Get-ExpectedTestExecutable -Project $project -Configuration $Configuration -Platform $Platform
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

Invoke-Step -Name "Launch smoke" -Action {
    if ($SkipLaunchSmoke) {
        Write-Host "Launch smoke skipped by request. Use a local interactive run before treating the build as fully release-ready."
        return
    }

    if ($Configuration -eq "Release-MSIX") {
        Invoke-PackagedLaunchSmoke -RepoRoot $repoRoot -Platform $Platform
        return
    }

    $exePath = Get-ExpectedAppOutputExe -RepoRoot $repoRoot -Configuration $Configuration -Platform $Platform
    Invoke-UnpackagedLaunchSmoke -ExePath $exePath
}

Write-Host ""
Write-Host "Verification complete."
