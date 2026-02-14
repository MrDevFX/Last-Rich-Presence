(function () {
  if (window.__lrpCompanionInjected) {
    return;
  }
  window.__lrpCompanionInjected = true;

  const host = window.location.hostname.toLowerCase();

  function detectSite() {
    if (host === "music.youtube.com") {
      return { key: "youtube_music", service: "YouTube Music", mediaKind: "music" };
    }
    if (host === "youtube.com" || host === "www.youtube.com" || host === "m.youtube.com" || host === "youtu.be") {
      return { key: "youtube", service: "YouTube", mediaKind: "video" };
    }
    if (host === "open.spotify.com") {
      return { key: "spotify", service: "Spotify", mediaKind: "music" };
    }
    if (host === "soundcloud.com" || host.endsWith(".soundcloud.com")) {
      return { key: "soundcloud", service: "SoundCloud", mediaKind: "music" };
    }
    if (host === "music.apple.com" || host.endsWith(".music.apple.com")) {
      return { key: "apple_music", service: "Apple Music", mediaKind: "music" };
    }
    if (host === "music.amazon.com" || host === "music.amazon.in" || host.endsWith(".music.amazon.com") || host.endsWith(".music.amazon.in")) {
      return { key: "amazon_music", service: "Amazon Music", mediaKind: "music" };
    }
    if (host === "www.deezer.com" || host === "deezer.com" || host.endsWith(".deezer.com")) {
      return { key: "deezer", service: "Deezer", mediaKind: "music" };
    }
    if (host === "listen.tidal.com" || host === "tidal.com" || host.endsWith(".tidal.com")) {
      return { key: "tidal", service: "TIDAL", mediaKind: "music" };
    }
    if (
      host === "www.jiosaavn.com" ||
      host === "jiosaavn.com" ||
      host.endsWith(".jiosaavn.com") ||
      host === "saavn.com" ||
      host.endsWith(".saavn.com")
    ) {
      return { key: "jiosaavn", service: "JioSaavn", mediaKind: "music" };
    }
    if (host === "gaana.com" || host === "www.gaana.com" || host.endsWith(".gaana.com")) {
      return { key: "gaana", service: "Gaana", mediaKind: "music" };
    }
    if (host === "wynk.in" || host.endsWith(".wynk.in")) {
      return { key: "wynk", service: "Wynk Music", mediaKind: "music" };
    }
    if (host.endsWith(".bandcamp.com") || host === "bandcamp.com" || host === "www.bandcamp.com") {
      return { key: "bandcamp", service: "Bandcamp", mediaKind: "music" };
    }
    if (host === "mixcloud.com" || host === "www.mixcloud.com" || host.endsWith(".mixcloud.com")) {
      return { key: "mixcloud", service: "Mixcloud", mediaKind: "music" };
    }
    if (host === "twitch.tv" || host.endsWith(".twitch.tv")) {
      return { key: "twitch", service: "Twitch", mediaKind: "video" };
    }
    return null;
  }

  const site = detectSite();
  if (!site) return;

  function text(selector) {
    const node = document.querySelector(selector);
    return node ? (node.textContent || "").trim() : "";
  }

  function firstText(selectors) {
    for (const selector of selectors) {
      const value = text(selector);
      if (value) return value;
    }
    return "";
  }

  function metaContent(selector) {
    const node = document.querySelector(selector);
    if (!node) return "";
    const value = node.getAttribute("content");
    return value ? value.trim() : "";
  }

  function firstMeta(selectors) {
    for (const selector of selectors) {
      const value = metaContent(selector);
      if (value) return value;
    }
    return "";
  }

  function parseClock(raw) {
    const value = (raw || "").trim();
    if (!value) return 0;

    const parts = value.split(":").map((x) => parseInt(x, 10));
    if (parts.some((x) => Number.isNaN(x))) return 0;

    if (parts.length === 2) return parts[0] * 60 + parts[1];
    if (parts.length === 3) return parts[0] * 3600 + parts[1] * 60 + parts[2];
    return 0;
  }

  function parseClockRange(raw) {
    const value = (raw || "").trim();
    if (!value || !value.includes("/")) return null;

    const parts = value.split("/");
    if (parts.length < 2) return null;

    const positionSeconds = parseClock(parts[0]);
    const durationSeconds = parseClock(parts[1]);
    if (durationSeconds <= 0) return null;

    return {
      positionSeconds,
      durationSeconds
    };
  }

  function findMediaBySelectors(selectors) {
    for (const selector of selectors) {
      const node = document.querySelector(selector);
      if (node instanceof HTMLMediaElement) {
        return node;
      }
    }
    return null;
  }

  function scoreMediaElement(media) {
    let score = 0;

    if (!media.paused && !media.ended) score += 220;
    if (Number.isFinite(media.duration) && media.duration > 0) score += 90;
    if (Number.isFinite(media.currentTime) && media.currentTime > 0) score += 40;
    if (media.readyState >= 2) score += 25;
    if (media.currentSrc) score += 20;

    const className = String(media.className || "");
    if (/main-video|video-stream/i.test(className)) score += 120;

    if (media.closest("#movie_player")) score += 150;
    if (media.closest("ytmusic-player")) score += 60;

    return score;
  }

  function getMediaElement() {
    if (site.key === "youtube_music") {
      const preferred = findMediaBySelectors([
        "#movie_player video.video-stream.html5-main-video",
        "#movie_player audio",
        "video.video-stream.html5-main-video",
        "audio.video-stream.html5-main-video"
      ]);
      if (preferred) return preferred;
    }

    if (site.key === "youtube") {
      const preferred = findMediaBySelectors([
        "#movie_player video.video-stream.html5-main-video",
        "#movie_player audio",
        "video.video-stream.html5-main-video"
      ]);
      if (preferred) return preferred;
    }

    const candidates = Array.from(document.querySelectorAll("video, audio"))
      .filter((node) => node instanceof HTMLMediaElement);

    if (candidates.length === 0) return null;
    if (candidates.length === 1) return candidates[0];

    candidates.sort((left, right) => scoreMediaElement(right) - scoreMediaElement(left));
    return candidates[0];
  }

  function getMediaSessionMetadata() {
    try {
      const session = navigator.mediaSession;
      const metadata = session && session.metadata;
      if (!metadata) return null;

      return {
        title: (metadata.title || "").trim(),
        artist: (metadata.artist || "").trim(),
        album: (metadata.album || "").trim(),
        playbackState: (session.playbackState || "").trim().toLowerCase()
      };
    } catch {
      return null;
    }
  }

  function inferIsPlaying(media, mediaSessionPlaybackState) {
    if (mediaSessionPlaybackState === "playing") {
      return true;
    }

    if (media) {
      try {
        if (media.remote && media.remote.state === "connected" && !media.ended) {
          return true;
        }
      } catch {
        // Ignore Remote Playback access errors.
      }

      return !media.paused && !media.ended;
    }

    if (mediaSessionPlaybackState) {
      return mediaSessionPlaybackState === "playing";
    }
    return false;
  }

  function cleanDocTitle(value, serviceName) {
    let out = (value || "").trim();
    if (!out) return "";

    out = out.replace(/\s*[-|]\s*YouTube\s*$/i, "");
    out = out.replace(/\s*[-|]\s*YouTube Music\s*$/i, "");
    out = out.replace(/\s*[-|]\s*Spotify\s*$/i, "");
    out = out.replace(/\s*[-|]\s*SoundCloud\s*$/i, "");

    if (serviceName) {
      const escaped = serviceName.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
      out = out.replace(new RegExp("\\s*[-|]\\s*" + escaped + "\\s*$", "i"), "");
    }

    return out.trim();
  }

  function baseState(title, artist, album, isPlaying, positionSeconds, durationSeconds, confidence, rule) {
    if (!title) return null;
    return {
      siteKey: site.key,
      service: site.service,
      mediaKind: site.mediaKind,
      title,
      artist: artist || "",
      album: album || "",
      isPlaying: !!isPlaying,
      positionSeconds: Number.isFinite(positionSeconds) ? positionSeconds : 0,
      durationSeconds: Number.isFinite(durationSeconds) ? durationSeconds : 0,
      confidence,
      rule
    };
  }

  function getYouTubeState() {
    const media = getMediaElement();
    const sessionMeta = getMediaSessionMetadata();

    const title =
      firstText([
        "h1.ytd-watch-metadata yt-formatted-string",
        "h1.title yt-formatted-string",
        "h1.title",
        ".ytp-title-link"
      ]) ||
      firstMeta(["meta[property='og:title']", "meta[name='title']"]) ||
      cleanDocTitle(document.title, "YouTube");

    const artist =
      firstText([
        "#upload-info #channel-name a",
        "#channel-name a",
        "ytd-channel-name a",
        "#owner #channel-name a"
      ]) ||
      firstMeta(["meta[itemprop='author']", "meta[name='author']"]) ||
      (sessionMeta ? sessionMeta.artist : "");

    const domPos = parseClock(firstText([".ytp-time-current"]));
    const domDur = parseClock(firstText([".ytp-time-duration"]));

    const mediaPos = media && Number.isFinite(media.currentTime) ? media.currentTime : 0;
    const mediaDur = media && Number.isFinite(media.duration) ? media.duration : 0;

    const positionSeconds = domPos > 0 ? domPos : mediaPos;
    const durationSeconds = domDur > 0 ? domDur : mediaDur;
    const isPlaying = inferIsPlaying(media, sessionMeta ? sessionMeta.playbackState : "");

    return baseState(title, artist, "", isPlaying, positionSeconds, durationSeconds, 96, "dom:youtube");
  }

  function getYouTubeMusicState() {
    const media = getMediaElement();
    const sessionMeta = getMediaSessionMetadata();

    const title =
      firstText([
        "ytmusic-player-bar .title",
        "ytmusic-player-bar .title a",
        "ytmusic-player-bar .middle-controls .title"
      ]) ||
      firstMeta(["meta[property='og:title']"]) ||
      cleanDocTitle(document.title, "YouTube Music");

    const byline =
      firstText([
        "ytmusic-player-bar .byline",
        "ytmusic-player-bar .subtitle",
        "ytmusic-player-bar .middle-controls .byline"
      ]) ||
      firstMeta(["meta[name='author']"]) ||
      (sessionMeta ? sessionMeta.artist : "");

    let artist = byline;
    let album = "";
    if (byline.includes("\u2022")) {
      const parts = byline.split("\u2022").map((x) => x.trim()).filter(Boolean);
      artist = parts[0] || "";
      album = parts.slice(1).join(" - ");
    }

    const clockRange = parseClockRange(firstText([
      "ytmusic-player-bar .time-info",
      "ytmusic-player-bar #progress-bar .time-info",
      "ytmusic-player-bar [class*='time-info']"
    ]));

    const mediaPos = media && Number.isFinite(media.currentTime) ? media.currentTime : 0;
    const mediaDur = media && Number.isFinite(media.duration) ? media.duration : 0;

    const mediaTimelineValid =
      Number.isFinite(mediaPos) &&
      Number.isFinite(mediaDur) &&
      mediaDur > 0 &&
      mediaPos >= 0 &&
      mediaPos <= mediaDur + 5;

    const clockTimelineValid =
      !!clockRange &&
      clockRange.durationSeconds > 0 &&
      clockRange.positionSeconds >= 0 &&
      clockRange.positionSeconds <= clockRange.durationSeconds + 5;

    let positionSeconds = 0;
    let durationSeconds = 0;
    let timelineConfidence = 95;
    let timelineRule = "dom:youtube-music";

    if (clockTimelineValid) {
      positionSeconds = clockRange.positionSeconds;
      durationSeconds = clockRange.durationSeconds;
      timelineConfidence = 99;
      timelineRule = "dom:youtube-music:clock";
    } else if (mediaTimelineValid) {
      positionSeconds = Math.floor(mediaPos);
      durationSeconds = Math.floor(mediaDur);
      timelineConfidence = 93;
      timelineRule = "dom:youtube-music:media";
    }
    const isPlaying = inferIsPlaying(media, sessionMeta ? sessionMeta.playbackState : "");

    return baseState(title, artist, album, isPlaying, positionSeconds, durationSeconds, timelineConfidence, timelineRule);
  }

  function getSpotifyState() {
    const media = getMediaElement();
    const sessionMeta = getMediaSessionMetadata();

    const title =
      firstText([
        "[data-testid='context-item-info-title']",
        "a[data-testid='context-item-link']",
        "[data-testid='now-playing-widget'] a"
      ]) ||
      (sessionMeta ? sessionMeta.title : "") ||
      cleanDocTitle(document.title, "Spotify");

    const artistNodes = document.querySelectorAll("[data-testid='context-item-info-subtitles'] a");
    let artist = "";
    if (artistNodes.length > 0) {
      artist = Array.from(artistNodes)
        .map((n) => (n.textContent || "").trim())
        .filter(Boolean)
        .join(", ");
    } else {
      artist = text("[data-testid='context-item-info-subtitles']") || (sessionMeta ? sessionMeta.artist : "");
    }

    const playPauseButton = document.querySelector("[data-testid='control-button-playpause']");
    const ariaLabel = ((playPauseButton && playPauseButton.getAttribute("aria-label")) || "").toLowerCase();
    let isPlaying = ariaLabel.includes("pause") || ariaLabel.includes("pausar") || ariaLabel.includes("anhalten");
    if (!isPlaying) {
      isPlaying = inferIsPlaying(media, sessionMeta ? sessionMeta.playbackState : "");
    }

    const positionSeconds = parseClock(text("[data-testid='playback-position']"));
    const durationSeconds = parseClock(text("[data-testid='playback-duration']"));

    return baseState(title, artist, "", isPlaying, positionSeconds, durationSeconds, 97, "dom:spotify");
  }

  function getSoundCloudState() {
    const media = getMediaElement();
    const sessionMeta = getMediaSessionMetadata();

    const title =
      firstText([
        ".playbackSoundBadge__titleLink",
        ".playbackSoundBadge__titleContextContainer a",
        ".playbackSoundBadge__title",
        ".playbackSoundBadge span"
      ]) ||
      (sessionMeta ? sessionMeta.title : "") ||
      firstMeta(["meta[property='og:title']"]) ||
      cleanDocTitle(document.title, "SoundCloud");

    const artist =
      firstText([
        ".playbackSoundBadge__lightLink",
        ".playbackSoundBadge__titleContextContainer .sc-link-light"
      ]) ||
      (sessionMeta ? sessionMeta.artist : "") ||
      firstMeta(["meta[name='author']"]);

    const isPlaying = inferIsPlaying(media, sessionMeta ? sessionMeta.playbackState : "");
    const positionSeconds = media && Number.isFinite(media.currentTime) ? media.currentTime : 0;
    const durationSeconds = media && Number.isFinite(media.duration) ? media.duration : 0;

    return baseState(title, artist, "", isPlaying, positionSeconds, durationSeconds, 96, "dom:soundcloud");
  }

  function getJioSaavnState() {
    const media = getMediaElement();
    const sessionMeta = getMediaSessionMetadata();

    const title =
      firstText([
        ".c-screen .song-name",
        ".player-song-name",
        ".player [class*='songName']",
        ".s-player [class*='song']"
      ]) ||
      (sessionMeta ? sessionMeta.title : "") ||
      firstMeta(["meta[property='og:title']"]) ||
      cleanDocTitle(document.title, "JioSaavn");

    const artist =
      firstText([
        ".c-screen .song-artist",
        ".player-artist-name",
        ".player [class*='artist']"
      ]) ||
      (sessionMeta ? sessionMeta.artist : "") ||
      firstMeta(["meta[name='author']"]);

    const positionSeconds = media && Number.isFinite(media.currentTime) ? media.currentTime : 0;
    const durationSeconds = media && Number.isFinite(media.duration) ? media.duration : 0;
    const isPlaying = inferIsPlaying(media, sessionMeta ? sessionMeta.playbackState : "");

    return baseState(title, artist, "", isPlaying, positionSeconds, durationSeconds, 97, "dom:jiosaavn");
  }

  function getTwitchState() {
    const media = getMediaElement();
    const sessionMeta = getMediaSessionMetadata();

    const title =
      firstText([
        "[data-a-target='stream-title']",
        "h2[data-a-target='stream-title']",
        "h1[data-a-target='stream-title']"
      ]) ||
      (sessionMeta ? sessionMeta.title : "") ||
      firstMeta(["meta[property='og:title']"]) ||
      cleanDocTitle(document.title, "Twitch");

    const artist =
      firstText([
        "[data-a-target='stream-game-link']",
        "a[data-a-target='stream-game-link']",
        "a[data-a-target='user-display-name']"
      ]) ||
      (sessionMeta ? sessionMeta.artist : "") ||
      firstMeta(["meta[name='author']"]);

    const positionSeconds = media && Number.isFinite(media.currentTime) ? media.currentTime : 0;
    const durationSeconds = media && Number.isFinite(media.duration) ? media.duration : 0;
    const isPlaying = inferIsPlaying(media, sessionMeta ? sessionMeta.playbackState : "");

    return baseState(title, artist, "", isPlaying, positionSeconds, durationSeconds, 95, "dom:twitch");
  }

  function getGenericSiteState() {
    const media = getMediaElement();
    const sessionMeta = getMediaSessionMetadata();
    if (!media && !sessionMeta) return null;

    const title =
      firstMeta([
        "meta[property='og:title']",
        "meta[name='twitter:title']",
        "meta[name='title']"
      ]) ||
      (sessionMeta ? sessionMeta.title : "") ||
      cleanDocTitle(document.title, site.service);

    const artist =
      firstMeta([
        "meta[name='author']",
        "meta[property='music:musician']",
        "meta[property='og:site_name']"
      ]) ||
      (sessionMeta ? sessionMeta.artist : "");

    const isPlaying = inferIsPlaying(media, sessionMeta ? sessionMeta.playbackState : "");
    const positionSeconds = media && Number.isFinite(media.currentTime) ? media.currentTime : 0;
    const durationSeconds = media && Number.isFinite(media.duration) ? media.duration : 0;

    return baseState(title, artist, "", isPlaying, positionSeconds, durationSeconds, 90, "generic:site-media");
  }

  function getState() {
    if (site.key === "youtube") return getYouTubeState();
    if (site.key === "youtube_music") return getYouTubeMusicState();
    if (site.key === "spotify") return getSpotifyState();
    if (site.key === "soundcloud") return getSoundCloudState();
    if (site.key === "jiosaavn") return getJioSaavnState();
    if (site.key === "twitch") return getTwitchState();
    return getGenericSiteState();
  }

  let lastSignature = "";
  let lastSentAt = 0;
  let hadState = false;
  let lastGoodPayload = null;
  let lastGoodAt = 0;
  let sequence = 0;
  const KEEPALIVE_MS = 3000;
  const CLEAR_GRACE_MS = 7000;

  function safeSendMessage(message) {
    try {
      chrome.runtime.sendMessage(message, () => {
        // Ignore context invalidation / worker sleep races.
        void chrome.runtime.lastError;
      });
    } catch {
      // Ignore context invalidation after extension reload/uninstall.
    }
  }

  function emit(force) {
    const now = Date.now();
    const state = getState();
    if (!state) {
      if (!hadState || !lastGoodPayload) {
        return;
      }

      if (now - lastGoodAt < CLEAR_GRACE_MS) {
        if (!force && now - lastSentAt < KEEPALIVE_MS) {
          return;
        }

        const fallbackPayload = {
          ...lastGoodPayload,
          pageHost: window.location.host,
          pageVisible: document.visibilityState === "visible",
          sequence: ++sequence
        };

        lastSentAt = now;
        safeSendMessage({ type: "lrp-media-state", payload: fallbackPayload });
        return;
      }

      if (hadState) {
        hadState = false;
        lastSignature = "";
        lastSentAt = 0;
        lastGoodPayload = null;
        lastGoodAt = 0;
        safeSendMessage({ type: "lrp-media-state-clear" });
      }
      return;
    }

    const payload = {
      ...state,
      pageHost: window.location.host,
      pageVisible: document.visibilityState === "visible",
      sequence: ++sequence
    };

    const signature = JSON.stringify({
      service: payload.service,
      title: payload.title,
      artist: payload.artist,
      isPlaying: payload.isPlaying,
      positionSeconds: Math.floor(payload.positionSeconds || 0),
      durationSeconds: Math.floor(payload.durationSeconds || 0)
    });

    if (!force && signature === lastSignature && now - lastSentAt < KEEPALIVE_MS) return;

    hadState = true;
    lastGoodPayload = payload;
    lastGoodAt = now;
    lastSignature = signature;
    lastSentAt = now;
    safeSendMessage({ type: "lrp-media-state", payload });
  }

  function attachMediaListeners() {
    const media = getMediaElement();
    if (!media || media.dataset.lrpBound === "1") return;

    media.dataset.lrpBound = "1";

    ["play", "pause", "ended", "loadedmetadata", "durationchange", "seeking", "seeked"].forEach((evt) => {
      media.addEventListener(evt, () => emit(true));
    });

    media.addEventListener("timeupdate", () => emit(false));
  }

  const observer = new MutationObserver(() => {
    attachMediaListeners();
    emit(false);
  });

  observer.observe(document.documentElement, {
    childList: true,
    subtree: true,
    attributes: true,
    characterData: false
  });

  document.addEventListener("visibilitychange", () => emit(true));

  window.addEventListener("beforeunload", () => {
    safeSendMessage({ type: "lrp-media-state-clear" });
  });

  attachMediaListeners();
  emit(true);
  setInterval(() => emit(false), 1000);
})();
