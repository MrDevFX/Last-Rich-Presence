<div align="center">
  <img src="Assets/Wide310x150Logo.scale-200.png" alt="Last Rich Presence" width="420" />

  <h1>Last Rich Presence</h1>
  <p><strong>Windows app for accurate, polished Discord Rich Presence across media, creative, and productivity workflows.</strong></p>

  <p>
    <img src="https://img.shields.io/badge/platform-Windows%2010%2F11-0078D4?style=for-the-badge&logo=windows&logoColor=white" alt="Windows 10/11" />
    <img src="https://img.shields.io/badge/UI-WinUI%203-0A5A9C?style=for-the-badge" alt="WinUI 3" />
    <img src="https://img.shields.io/badge/language-C%2B%2B20-00599C?style=for-the-badge&logo=cplusplus&logoColor=white" alt="C++20" />
    <img src="https://img.shields.io/badge/extension-Chromium%20MV3-3C873A?style=for-the-badge&logo=googlechrome&logoColor=white" alt="Chromium MV3" />
  </p>
</div>

## What It Does

Last Rich Presence keeps Discord aligned with what you are doing on Windows across three activity lanes:

- Media playback via Windows media sessions, with optional browser companion hints for better web timeline accuracy.
- Creativity activity via Adobe-family desktop app detection.
- Productivity activity via supported desktop productivity apps, including Microsoft Office apps and Codex.

The app is single-instance, supports tray-first workflows, can start minimized to tray, and persists in-app settings including the global Rich Presence master toggle.

## Interface Preview

<p align="center">
  <img src="Assets/v2.0.0-preview.png" alt="Last Rich Presence Interface" width="980" />
</p>

## Highlights

- Accurate media presence with GSMTC detection, browser-hint timeline arbitration, and source heuristics.
- Separate Creativity and Productivity Discord pipelines with dedicated app IDs, allowing them to coexist with media activity as separate Discord cards.
- Tray integration for show/hide, global Rich Presence enable or disable, and exit.
- Persisted settings for startup behavior, privacy controls, per-app filters, theme, activity type overrides, and diagnostics export/import.
- Optional Chromium MV3 companion extension for supported web players.

## Activity Lanes

### Media

- Detects active Windows media sessions.
- Builds Discord presence with title, artist, album, playback state, and timeline.
- Supports album art, source display, paused-state handling, idle-card behavior, and activity type override.
- Adds browser-specific privacy controls, blocked app/site terms, and optional browser album-art suppression.

### Creativity

- Detects supported Adobe-family desktop apps from foreground and visible windows.
- Detection modes: `ForegroundPreferredVisibleFallback`, `ForegroundOnly`, `VisibleWindowOnly`.
- Privacy modes: `Normal`, `AppOnly`, `Private`.
- Supports project-name display, window-title display, idle behavior, media-vs-creativity priority, per-app filters, and activity type override.

### Productivity

- Detects supported desktop productivity apps from foreground and visible windows.
- Detection modes: `ForegroundPreferredVisibleFallback`, `ForegroundOnly`, `VisibleWindowOnly`.
- Supports project or file-name display, per-app filters, and activity type override.

## Supported Desktop Apps

- Creativity: Adobe Photoshop, Illustrator, Premiere Pro, After Effects, InDesign, Audition, Media Encoder, Lightroom, Lightroom Classic, InCopy, Dreamweaver, Animate, XD, Bridge, Character Animator, Fresco, Dimension, Substance 3D Painter, Substance 3D Designer, Substance 3D Sampler, Substance 3D Stager, Substance 3D Modeler, Acrobat.
- Productivity: Word, Excel, PowerPoint, OneNote, Access, Publisher, Visio, Project, Codex.

## Supported Web Sources

The optional browser extension currently provides high-confidence hints for:

- YouTube
- YouTube Music
- Spotify Web
- SoundCloud
- Apple Music (web)
- Amazon Music (web)
- Deezer
- TIDAL
- JioSaavn
- Gaana
- Wynk Music
- Bandcamp
- Mixcloud
- Twitch media pages

## Requirements

### Runtime

- Windows 10/11
- Discord desktop client

### Build

- Visual Studio 2026 with Desktop development for C++ installed
- MSVC toolset `v145`
- Windows SDK / Build Tools required by the solution
- Node.js on `PATH` for browser-extension syntax checks in verification
- Inno Setup 6 if you want to build the unpackaged installer

## Build and Verify

Open [Last Rich Presence.sln](Last%20Rich%20Presence.sln) in Visual Studio and build one of these configurations:

| Configuration | Purpose |
| --- | --- |
| `Debug` / `Release` | Standard development builds |
| `Release-Inno` | Unpackaged, self-contained build for Inno installer releases |
| `Release-MSIX` | Packaged MSIX build path |

Quick verification:

```powershell
.\scripts\verify.ps1 -Configuration Debug -Platform x64
```

Also supported:

```powershell
.\scripts\verify.ps1 -Configuration Release -Platform x64
.\scripts\verify.ps1 -Configuration Release-Inno -Platform x64
.\scripts\verify.ps1 -Configuration Release-MSIX -Platform x64
```

The verification script builds the solution, runs browser-extension JavaScript syntax checks, and executes the native guard tests when the test binary is available.

## Browser Extension Setup

1. Open `chrome://extensions` or `edge://extensions`.
2. Enable **Developer mode**.
3. Click **Load unpacked**.
4. Select the `browser-extension/` folder.

The extension communicates with the desktop app through a local token-protected bridge on `127.0.0.1` / `localhost`.

## Release Packaging

For installer-focused release steps, use the dedicated guide:

- [installer/INSTALLER_GUIDE.md](installer/INSTALLER_GUIDE.md)

Typical release outputs:

- Inno installer: `dist\LastRichPresence-Setup-x64.exe`
- MSIX build artifacts: produced from `Release-MSIX`

## Privacy and Network Notes

- The optional browser extension sends local playback hints to the desktop app over `localhost`.
- Media album-art fallback may issue outbound requests to `itunes.apple.com`.
- If direct album art is unavailable and a thumbnail is available, the app can upload image bytes to Imgur for a fallback asset URL.
- If you do not want browser album-art behavior, use the in-app browser privacy and album-art settings.

## Testing and Contributor Notes

- Automated coverage is currently limited to the native guard-test project in [tests/RefactorGuardTests](tests/RefactorGuardTests).
- There is no broad UI or end-to-end automation yet.
- Current builds may emit duplicate `WindowsAppRuntimeAutoInitializer` warnings; these are known build warnings.
- Curated fallback app logos are bundled in `Assets\CreativeLogos` and `Assets\ProductiveLogos`.

## Project Structure

```text
Last Rich Presence/
|- src/
|  |- core/                 # Detection, presence building, settings, Discord IPC
|  |- ui/                   # WinUI shell, pages, settings, tray, animations
|- browser-extension/       # Optional MV3 companion extension
|- installer/               # Inno Setup script and installer guide
|- scripts/                 # Build and verification scripts
|- tests/                   # Native guard tests
|- Assets/                  # App branding and curated fallback logos
|- Last Rich Presence.sln   # Visual Studio solution
```

## Troubleshooting

- If build fails with `LNK1201` or `LNK1104`, close any running app instance and rebuild.
- If Rich Presence does not update, ensure Discord desktop is running.
- If extension changes do not appear, reload the unpacked extension.
- If startup settings do not apply for unpackaged installs, verify `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\LastRichPresence`.
- If repeated launches appear to do nothing, the app may already be running in the tray; this is expected because the app redirects secondary launches to the existing instance.
