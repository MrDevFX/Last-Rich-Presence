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
- see live forwarding status

It sends refined source hints to a local endpoint so the desktop app can use higher-accuracy labels.

## Transport (Option 2)

This extension posts JSON to:

- `http://127.0.0.1:32145/v1/browser-hint`
- fallback: `http://localhost:32145/v1/browser-hint`

Before posting hints, the extension requests a session token from:

- `GET /v1/browser-hint/token`

and sends it with `x-lrp-token` header on hint updates.

## Install (unpacked)

1. Open `chrome://extensions` (or `edge://extensions`).
2. Enable **Developer mode**.
3. Click **Load unpacked**.
4. Select the `browser-extension` folder.
5. Click the extension icon to open the companion popup.

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
