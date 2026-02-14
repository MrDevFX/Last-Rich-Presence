const ENDPOINTS = [
  "http://127.0.0.1:32145/v1/browser-hint",
  "http://localhost:32145/v1/browser-hint"
];

const STALE_AFTER_MS = 30000;
const HEARTBEAT_MS = 1500;
const RESEND_AFTER_MS = 2000;
const CLEAR_DEBOUNCE_MS = 2500;
const AUTH_TOKEN_TTL_MS = 10 * 60 * 1000;

const DEFAULT_SETTINGS = {
  enabled: true,
  siteEnabled: {
    youtube: true,
    youtube_music: true,
    spotify: true,
    soundcloud: true,
    apple_music: true,
    amazon_music: true,
    deezer: true,
    tidal: true,
    jiosaavn: true,
    gaana: true,
    wynk: true,
    bandcamp: true,
    mixcloud: true,
    twitch: true
  }
};

const SITE_KEYS = Object.keys(DEFAULT_SETTINGS.siteEnabled);
const SUPPORTED_HOST_PATTERN = /(^|\.)(youtube\.com|youtu\.be|open\.spotify\.com|soundcloud\.com|music\.apple\.com|music\.amazon\.com|music\.amazon\.in|deezer\.com|tidal\.com|jiosaavn\.com|saavn\.com|gaana\.com|wynk\.in|bandcamp\.com|mixcloud\.com|twitch\.tv)$/i;

const latestByTab = new Map();
const clearTimersByTab = new Map();
const authTokenCache = new Map();

let settings = JSON.parse(JSON.stringify(DEFAULT_SETTINGS));
let lastSignature = "";
let lastSentAt = 0;

const runtimeStatus = {
  lastPostAt: 0,
  lastPostOk: false,
  lastError: "",
  lastHintService: "",
  activeHints: 0
};

function normalizeSettings(input) {
  const merged = JSON.parse(JSON.stringify(DEFAULT_SETTINGS));

  if (input && typeof input === "object") {
    if (typeof input.enabled === "boolean") {
      merged.enabled = input.enabled;
    }

    if (input.siteEnabled && typeof input.siteEnabled === "object") {
      for (const key of SITE_KEYS) {
        if (typeof input.siteEnabled[key] === "boolean") {
          merged.siteEnabled[key] = input.siteEnabled[key];
        }
      }

      // Backward compatibility with older popup keys.
      if (typeof input.siteEnabled.youtubeMusic === "boolean") {
        merged.siteEnabled.youtube_music = input.siteEnabled.youtubeMusic;
      }
    }
  }

  return merged;
}

async function loadSettings() {
  const stored = await chrome.storage.sync.get(["lrpSettings"]);
  settings = normalizeSettings(stored.lrpSettings);
}

async function saveSettings() {
  await chrome.storage.sync.set({ lrpSettings: settings });
}

function serviceKeyFromHint(hint) {
  const explicit = (hint.siteKey || "").toLowerCase();
  if (explicit === "youtube") return "youtube";
  if (explicit === "youtube_music") return "youtube_music";
  if (explicit === "spotify") return "spotify";
  if (explicit === "soundcloud") return "soundcloud";
  if (explicit === "apple_music") return "apple_music";
  if (explicit === "amazon_music") return "amazon_music";
  if (explicit === "deezer") return "deezer";
  if (explicit === "tidal") return "tidal";
  if (explicit === "jiosaavn") return "jiosaavn";
  if (explicit === "gaana") return "gaana";
  if (explicit === "wynk") return "wynk";
  if (explicit === "bandcamp") return "bandcamp";
  if (explicit === "mixcloud") return "mixcloud";
  if (explicit === "twitch") return "twitch";

  const service = (hint.service || "").toLowerCase();
  if (service.includes("youtube music")) return "youtube_music";
  if (service.includes("youtube")) return "youtube";
  if (service.includes("spotify")) return "spotify";
  if (service.includes("soundcloud")) return "soundcloud";
  if (service.includes("apple music")) return "apple_music";
  if (service.includes("amazon music")) return "amazon_music";
  if (service.includes("deezer")) return "deezer";
  if (service.includes("tidal")) return "tidal";
  if (service.includes("jiosaavn")) return "jiosaavn";
  if (service.includes("gaana")) return "gaana";
  if (service.includes("wynk")) return "wynk";
  if (service.includes("bandcamp")) return "bandcamp";
  if (service.includes("mixcloud")) return "mixcloud";
  if (service.includes("twitch")) return "twitch";
  return "";
}

function isHintAllowed(hint) {
  if (!settings.enabled) return false;

  const key = serviceKeyFromHint(hint);
  if (!key) return true;

  const value = settings.siteEnabled ? settings.siteEnabled[key] : undefined;
  if (typeof value === "boolean") return value;
  return true;
}

function pruneStaleHints() {
  const now = Date.now();
  for (const [tabId, hint] of latestByTab.entries()) {
    if (!hint || now - (hint.timestamp || 0) > STALE_AFTER_MS) {
      latestByTab.delete(tabId);
    }
  }
  runtimeStatus.activeHints = latestByTab.size;
}

function cancelPendingClear(tabId) {
  const timer = clearTimersByTab.get(tabId);
  if (timer) {
    clearTimeout(timer);
    clearTimersByTab.delete(tabId);
  }
}

function scoreHint(hint) {
  const ageMs = Math.max(0, Date.now() - (hint.timestamp || 0));
  let score = 0;

  if (hint.isPlaying) score += 100;
  if (hint.pageVisible) score += 20;
  if (typeof hint.confidence === "number") {
    score += Math.max(0, Math.min(30, Math.floor(hint.confidence / 4)));
  }
  if (hint.siteKey) score += 5;
  score += Math.max(0, 20 - Math.floor(ageMs / 500));

  return score;
}

function chooseBestHint() {
  pruneStaleHints();

  let best = null;
  let bestScore = -1;

  for (const hint of latestByTab.values()) {
    const score = scoreHint(hint);
    if (score > bestScore) {
      bestScore = score;
      best = hint;
    }
  }

  return best;
}

async function postToEndpoint(payload) {
  for (const endpoint of ENDPOINTS) {
    const tokenEndpoint = endpoint.replace("/v1/browser-hint", "/v1/browser-hint/token");

    async function fetchToken(forceRefresh) {
      const cached = authTokenCache.get(endpoint);
      const now = Date.now();
      if (!forceRefresh && cached && cached.token && now - cached.fetchedAt < AUTH_TOKEN_TTL_MS) {
        return cached.token;
      }

      try {
        const tokenResponse = await fetch(tokenEndpoint, {
          method: "GET",
          headers: { "x-lrp-extension": "1" },
          cache: "no-store"
        });

        if (!tokenResponse.ok) {
          return "";
        }

        const tokenPayload = await tokenResponse.json();
        const token = typeof tokenPayload.token === "string" ? tokenPayload.token : "";
        if (!token) return "";

        authTokenCache.set(endpoint, { token, fetchedAt: now });
        return token;
      } catch {
        return "";
      }
    }

    try {
      let token = await fetchToken(false);
      if (!token) continue;

      const response = await fetch(endpoint, {
        method: "POST",
        headers: {
          "content-type": "application/json",
          "x-lrp-extension": "1",
          "x-lrp-token": token
        },
        body: JSON.stringify(payload),
        keepalive: true
      });

      if (response.status === 401) {
        token = await fetchToken(true);
        if (!token) continue;

        const retry = await fetch(endpoint, {
          method: "POST",
          headers: {
            "content-type": "application/json",
            "x-lrp-extension": "1",
            "x-lrp-token": token
          },
          body: JSON.stringify(payload),
          keepalive: true
        });

        if (retry.ok) {
          runtimeStatus.lastPostAt = Date.now();
          runtimeStatus.lastPostOk = true;
          runtimeStatus.lastError = "";
          return true;
        }

        if (retry.status === 401) {
          authTokenCache.delete(endpoint);
        }
      }

      if (response.ok) {
        runtimeStatus.lastPostAt = Date.now();
        runtimeStatus.lastPostOk = true;
        runtimeStatus.lastError = "";
        return true;
      }
    } catch (error) {
      runtimeStatus.lastPostAt = Date.now();
      runtimeStatus.lastPostOk = false;
      runtimeStatus.lastError = String(error || "post-failed");
    }
  }

  return false;
}

function buildOutboundPayload() {
  if (!settings.enabled) {
    return {
      kind: "clear",
      source: "lrp-browser-extension",
      timestamp: Date.now()
    };
  }

  const best = chooseBestHint();
  if (!best) {
    runtimeStatus.lastHintService = "";
    return {
      kind: "clear",
      source: "lrp-browser-extension",
      timestamp: Date.now()
    };
  }

  runtimeStatus.lastHintService = best.service || "";

  return {
    schemaVersion: 1,
    kind: "hint",
    source: "lrp-browser-extension",
    timestamp: Date.now(),
    data: {
      service: best.service,
      siteKey: best.siteKey,
      mediaKind: best.mediaKind,
      title: best.title,
      artist: best.artist,
      album: best.album,
      isPlaying: best.isPlaying,
      positionSeconds: best.positionSeconds,
      durationSeconds: best.durationSeconds,
      pageHost: best.pageHost,
      pageVisible: best.pageVisible,
      tabId: best.tabId,
      sequence: best.sequence,
      confidence: best.confidence,
      rule: best.rule
    }
  };
}

async function forwardHint(force = false) {
  const payload = buildOutboundPayload();
  const signature = JSON.stringify(payload);
  const now = Date.now();

  if (!force && signature === lastSignature && now - lastSentAt < RESEND_AFTER_MS) {
    return;
  }

  await postToEndpoint(payload);
  lastSignature = signature;
  lastSentAt = now;
}

function getPopupState() {
  pruneStaleHints();
  return {
    settings,
    runtimeStatus: {
      ...runtimeStatus,
      endpoint: ENDPOINTS[0]
    }
  };
}

function isSupportedUrl(urlValue) {
  try {
    const parsed = new URL(urlValue);
    if (parsed.protocol !== "http:" && parsed.protocol !== "https:") {
      return false;
    }
    return SUPPORTED_HOST_PATTERN.test(parsed.hostname.toLowerCase());
  } catch {
    return false;
  }
}

async function ensureContentScriptForTab(tabId, urlValue) {
  if (!tabId || !isSupportedUrl(urlValue)) return;

  try {
    await chrome.scripting.executeScript({
      target: { tabId },
      files: ["content.js"]
    });
  } catch {
    // Ignore tabs where script injection is not allowed.
  }
}

async function injectIntoOpenTabs() {
  try {
    const tabs = await chrome.tabs.query({});
    for (const tab of tabs) {
      if (!tab || typeof tab.id !== "number") continue;
      await ensureContentScriptForTab(tab.id, tab.url || "");
    }
  } catch {
    // Ignore startup query failures.
  }
}

chrome.runtime.onInstalled.addListener(async () => {
  await loadSettings();
  await saveSettings();
  await injectIntoOpenTabs();
});

chrome.runtime.onStartup.addListener(() => {
  void injectIntoOpenTabs();
});

chrome.tabs.onUpdated.addListener((tabId, changeInfo, tab) => {
  if (changeInfo.status === "complete") {
    void ensureContentScriptForTab(tabId, (tab && tab.url) || "");
  }
});

void loadSettings();

chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
  if (message && message.type === "lrp-popup-get-state") {
    sendResponse(getPopupState());
    return;
  }

  if (message && message.type === "lrp-popup-set-settings") {
    settings = normalizeSettings(message.settings);
    void saveSettings().then(async () => {
      pruneStaleHints();

      if (!settings.enabled) {
        latestByTab.clear();
        runtimeStatus.activeHints = 0;
      } else {
        for (const [tabId, hint] of latestByTab.entries()) {
          if (!isHintAllowed(hint)) {
            latestByTab.delete(tabId);
          }
        }
      }

      await forwardHint(true);
      sendResponse(getPopupState());
    });
    return true;
  }

  const tabId = sender.tab && sender.tab.id;
  if (!tabId) return;

  if (message && message.type === "lrp-media-state" && message.payload) {
    cancelPendingClear(tabId);

    if (!settings.enabled || !isHintAllowed(message.payload)) {
      latestByTab.delete(tabId);
      runtimeStatus.activeHints = latestByTab.size;
      void forwardHint(true);
      return;
    }

    latestByTab.set(tabId, {
      ...message.payload,
      tabId,
      timestamp: Date.now()
    });
    runtimeStatus.activeHints = latestByTab.size;
    void forwardHint(false);
    return;
  }

  if (message && message.type === "lrp-media-state-clear") {
    cancelPendingClear(tabId);
    const timer = setTimeout(() => {
      clearTimersByTab.delete(tabId);
      latestByTab.delete(tabId);
      runtimeStatus.activeHints = latestByTab.size;
      void forwardHint(true);
    }, CLEAR_DEBOUNCE_MS);

    clearTimersByTab.set(tabId, timer);
  }
});

chrome.tabs.onRemoved.addListener((tabId) => {
  cancelPendingClear(tabId);
  latestByTab.delete(tabId);
  runtimeStatus.activeHints = latestByTab.size;
  void forwardHint(true);
});

setInterval(() => {
  void forwardHint(false);
}, HEARTBEAT_MS);
