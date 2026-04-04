# Last Rich Presence Companion Extension (MVP)

This is an optional Chrome/Edge extension that detects media from:

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
- Twitch (media pages)

It includes a small popup UI where users can:

- enable/disable extension hints
- toggle site sources for all supported services
- see a compact connection status at the top of the popup

It sends refined source hints to the desktop app over Chromium native messaging so the desktop app can use higher-accuracy labels.

## Transport

The extension opens a native-messaging connection to:

- `com.lastprojects.lastrichpresence`

The desktop app registers that host per-user for both Chrome and Edge on app launch. The native host forwards the existing browser-hint JSON payload into the running app over the internal named pipe `\\.\pipe\LastRichPresence.BrowserHints`.

Important behavior notes:

- The desktop app must already be running.
- The native host does not auto-launch or restore the desktop app.
- If the app is not running, the popup shows an offline or disconnected status.

## Install (unpacked)

1. Open `chrome://extensions` (or `edge://extensions`).
2. Enable **Developer mode**.
3. Click **Load unpacked**.
4. Select the `browser-extension` folder.
5. Click the extension icon to open the companion popup.
6. Launch the desktop app once so the browser native-host registration exists for your current install path.
7. Keep the committed extension `key` stable unless you also update the desktop app allow-list, because the native host is registered for this fixed extension ID.

## Payload shape

Hint message:

```json
{
  "kind": "hint",
  "source": "lrp-browser-extension",
  "timestamp": 1730000000000,
  "data": {
    "service": "Spotify",
    "mediaKind": "music",
    "title": "Track title",
    "artist": "Artist",
    "album": "",
    "isPlaying": true,
    "positionSeconds": 61,
    "durationSeconds": 314,
    "pageHost": "open.spotify.com",
    "pageVisible": true,
    "tabId": 123,
    "confidence": 97,
    "rule": "dom:spotify-player"
  }
}
```

Clear message:

```json
{
  "kind": "clear",
  "source": "lrp-browser-extension",
  "timestamp": 1730000000000
}
```

## Notes

- The desktop app keeps working without this extension.
- This extension is an optional precision layer for browser-source classification.
- The payload shape is unchanged from the previous bridge; only the transport changed.
