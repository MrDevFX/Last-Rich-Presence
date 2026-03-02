# Last Rich Presence Installer Guide

This guide documents the release flow we validated for generating an `.exe` installer with Inno Setup.

## Files

- Inno script: `installer/LastRichPresence.iss`
- Output installer: `dist/LastRichPresence-Setup-x64.exe`

## Goal

Build an installer that works on other PCs without requiring users to preinstall Windows App Runtime.

## Important build mode

Use **unpackaged + self-contained** build:

- `WindowsPackageType=None`
- `AppxPackage=false`
- `WindowsAppSDKSelfContained=true`

If you build framework-dependent by mistake, the installer becomes very small (~6 MB) and may not work on clean machines.

## Prerequisites

- Visual Studio/MSBuild installed
- Inno Setup 6 installed (default path used here):
  - `C:\Program Files (x86)\Inno Setup 6\ISCC.exe`

## Release steps (recommended)

Run from repo root:

```powershell
msbuild "Last Rich Presence.sln" -t:Clean `
  -p:Configuration=Release `
  -p:Platform=x64 `
  -p:WindowsPackageType=None `
  -p:AppxPackage=false `
  -p:WindowsAppSDKSelfContained=true
```

```powershell
msbuild "Last Rich Presence.sln" -t:Build `
  -p:Configuration=Release `
  -p:Platform=x64 `
  -p:WindowsPackageType=None `
  -p:AppxPackage=false `
  -p:WindowsAppSDKSelfContained=true
```

Then compile installer:

```powershell
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" "installer\LastRichPresence.iss"
```

## Output

Installer is generated at:

`dist\LastRichPresence-Setup-x64.exe`

Expected size (self-contained): around `30-35 MB` (observed ~33 MB).

## Quick smoke test (optional)

Silent install:

```powershell
.\dist\LastRichPresence-Setup-x64.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-
```

Installed app location:

`%LOCALAPPDATA%\Programs\Last Rich Presence\Last_Rich_Presence.exe`

## Troubleshooting

1. Installer is too small (~5-6 MB)
   - Cause: framework-dependent build was packaged.
   - Fix: run `Clean` + `Build` again with `WindowsAppSDKSelfContained=true`.

2. App crashes right after launch after changing build mode
   - Cause: stale mixed artifacts from previous build mode.
   - Fix: run the full `Clean` step first, then rebuild.

3. Inno compile works but app missing files
   - Ensure `x64\Release\Last Rich Presence` exists and contains runtime DLLs.
   - Rebuild using the exact commands above.

4. "Launch on Windows Startup" toggle does not work (Inno/unpackaged install)
   - Unpackaged apps cannot rely on `StartupTask` (MSIX-only behavior).
   - Current app code uses registry fallback: `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\LastRichPresence`.
   - Verify with:

```powershell
Get-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name LastRichPresence
```

5. Setup says files are in use (`Last_Rich_Presence.exe` or runtime DLLs like `CoreMessagingXP.dll`)
   - Cause: app/runtime helper is still running (often minimized to tray or auto-started).
   - Current `.iss` proactively kills both:
     - `Last_Rich_Presence.exe`
     - `RestartAgent.exe`
   - Setup uses:
     - `CloseApplications=yes` (built-in close-app handling for locked files)
     - `RestartApplications=no`
   - Uninstall also runs forced termination:

```powershell
taskkill /F /T /IM Last_Rich_Presence.exe
taskkill /F /T /IM RestartAgent.exe
```

6. Installed app shows generic icon (desktop shortcut / Task Manager / Control Panel)
   - Ensure icon is embedded into the exe via project resource file:
     - `app_icon.rc` with `IDI_APP_ICON ICON "Assets\\logo.ico"`
     - included in project as `ResourceCompile`
   - Installer shortcut/uninstall icon should point to `Assets\logo.ico` in `.iss`.
   - If updated icon still does not appear immediately, Windows icon cache may be stale; sign out/in once.

## Notes about current `.iss`

- Installs per-user by default (`PrivilegesRequired=lowest`).
- Copies entire build output recursively.
- Excludes debug/dev artifacts (`*.pdb`, `*.lib`, `*.exp`, etc.).
- Supports optional desktop icon.
- Shows license agreement page using:
  - `installer\LICENSE.txt` (copied from `C:\Users\Dev\Documents\GitHub\Last-Rich-Presence\LICENSE`)
- Publisher branding in installer metadata:
  - `AppPublisher=Last Projects`
  - `AppPublisherURL=https://lastprojects.com/`
  - `AppSupportURL=https://lastprojects.com/`
  - `AppUpdatesURL=https://lastprojects.com/`
