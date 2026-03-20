# Last Rich Presence Installer Guide

This guide covers the release flow for generating the unpackaged `.exe` installer with Inno Setup.

## Files

- Inno script: `installer/LastRichPresence.iss`
- Output installer: `dist/LastRichPresence-Setup-x64.exe`

## Goal

Build a self-contained installer that runs on clean machines without requiring the user to preinstall the Windows App Runtime.

## Recommended Build Profile

Use the unpackaged, self-contained profile:

- Configuration: `Release-Inno`
- Platform: `x64`

This is the preferred release path for the Inno installer.

## Prerequisites

- Visual Studio / MSBuild
- Inno Setup 6
- A successful `Release-Inno|x64` build

Default Inno compiler path:

```powershell
C:\Program Files (x86)\Inno Setup 6\ISCC.exe
```

## Release Steps

Build the self-contained app output:

```powershell
msbuild "Last Rich Presence.sln" -t:Build -p:Configuration=Release-Inno -p:Platform=x64 -m
```

Compile the installer:

```powershell
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" "installer\LastRichPresence.iss"
```

## Output

Installer output:

```text
dist\LastRichPresence-Setup-x64.exe
```

## Optional Smoke Test

Silent install:

```powershell
.\dist\LastRichPresence-Setup-x64.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-
```

Installed app location:

```text
%LOCALAPPDATA%\Programs\Last Rich Presence\Last_Rich_Presence.exe
```

## Troubleshooting

### Installer is too small

If the installer is unexpectedly tiny, you likely built the wrong profile.

- Expected fix: rebuild with `Release-Inno|x64`
- Cause: framework-dependent output was packaged instead of the self-contained release profile

### App crashes or behaves inconsistently after switching build modes

Likely cause: mixed old artifacts from a previous build mode.

Recommended fix:

```powershell
msbuild "Last Rich Presence.sln" -t:Clean -p:Configuration=Release-Inno -p:Platform=x64 -m
msbuild "Last Rich Presence.sln" -t:Build -p:Configuration=Release-Inno -p:Platform=x64 -m
```

### Installer reports files in use

The app may still be running, often minimized to tray.

The current installer script proactively handles locked files and terminates:

- `Last_Rich_Presence.exe`
- `RestartAgent.exe`

### Startup toggle does not work for unpackaged installs

Unpackaged installs rely on the registry fallback:

```text
HKCU\Software\Microsoft\Windows\CurrentVersion\Run\LastRichPresence
```

Verify with:

```powershell
Get-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name LastRichPresence
```

### Installed app icon looks generic

Check these paths:

- App icon resource: `app_icon.rc`
- Source icon: `Assets\logo.ico`
- Installer script: `installer\LastRichPresence.iss`

Windows icon caching can also delay icon refresh.

## Notes About the Current `.iss`

- Per-user install (`PrivilegesRequired=lowest`)
- Recursive copy of release output
- Excludes debug and development artifacts (`*.pdb`, `*.lib`, `*.exp`, etc.)
- Supports optional desktop icon
- Uses `installer\LICENSE.txt` for the license page
