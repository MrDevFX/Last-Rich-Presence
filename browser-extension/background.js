const HOST_NAME = "com.lastprojects.lastrichpresence";
const STALE_AFTER_MS = 30000;
const HEARTBEAT_MS = 1500;
const RESEND_AFTER_MS = 2000;
const CLEAR_DEBOUNCE_MS = 2500;
const RECONNECT_BASE_MS = 1000;
const RECONNECT_MAX_MS = 15000;
const NATIVE_RESPONSE_TIMEOUT_MS = 5000;
const RECONNECT_ALARM_NAME = "lrp-native-reconnect";

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

let settings = JSON.parse(JSON.stringify(DEFAULT_SETTINGS));
let settingsReady = null;
let lastSignature = "";
let lastSentAt = 0;
let nativePort = null;
let nativePortConnected = false;
let nativeConnectPromise = null;
let reconnectTimerId = null;
let reconnectDelayMs = RECONNECT_BASE_MS;
let pendingNativeResponse = null;
let nativeSendChain = Promise.resolve(true);

const runtimeStatus = {
  nativeHostName: HOST_NAME,
  registrationState: "unknown",
  connectionState: "disconnected",
  lastSendResult: "never",
  lastSendOk: false,
  lastSendAt: 0,
  lastError: "",
  lastHintService: "",
  activeHints: 0
};

function cloneDefaultSettings() {
  return JSON.parse(JSON.stringify(DEFAULT_SETTINGS));
}

function normalizeSettings(input) {
  const merged = cloneDefaultSettings();

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

function ensureSettingsLoaded() {
  if (!settingsReady) {
    settingsReady = loadSettings().catch(() => {
      settings = cloneDefaultSettings();
    });
  }

  return settingsReady;
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

function buildOutboundPayload() {
  if (!settings.enabled) {
    runtimeStatus.lastHintService = "";
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

function normalizeError(errorValue) {
  const rawValue =
    errorValue && typeof errorValue === "object" && typeof errorValue.message === "string"
      ? errorValue.message
      : errorValue;
  const text = String(rawValue || "").trim();
  if (!text) {
    return "";
  }

  const lower = text.toLowerCase();
  if (
    lower.includes("native messaging host not found") ||
    lower.includes("specified native messaging host not found") ||
    lower.includes("host not found")
  ) {
    return "native-host-not-found";
  }

  if (lower.includes("access denied")) {
    return "blocked";
  }

  if (
    lower.includes("forbidden") ||
    lower.includes("not allowed") ||
    lower.includes("allowed_origins") ||
    lower.includes("allowed origins")
  ) {
    return "blocked";
  }

  if (
    lower.includes("failed to start native messaging host") ||
    lower.includes("native host has exited")
  ) {
    return "native-host-launch-failed";
  }

  if (lower.includes("app-not-running")) {
    return "app-not-running";
  }

  return text;
}

function updateRegistrationState(errorValue) {
  const normalized = normalizeError(errorValue);
  if (normalized === "native-host-not-found") {
    runtimeStatus.registrationState = "not-registered";
    return;
  }

  if (normalized === "blocked") {
    runtimeStatus.registrationState = "blocked";
    return;
  }

  if (nativePortConnected) {
    runtimeStatus.registrationState = "registered";
  }
}

function clearReconnectTimer() {
  if (reconnectTimerId) {
    clearTimeout(reconnectTimerId);
    reconnectTimerId = null;
  }
}

function clearReconnectSchedule() {
  clearReconnectTimer();

  try {
    chrome.alarms.clear(RECONNECT_ALARM_NAME);
  } catch {
    // Ignore alarm cleanup failures in restricted browsers/tests.
  }
}

function scheduleReconnect(delayMs = reconnectDelayMs) {
  if (reconnectTimerId) {
    return;
  }

  const delay = Math.max(0, delayMs);
  reconnectTimerId = setTimeout(() => {
    reconnectTimerId = null;
    void ensureNativePort();
  }, delay);

  try {
    chrome.alarms.create(RECONNECT_ALARM_NAME, {
      when: Date.now() + delay
    });
  } catch {
    // Ignore alarm scheduling failures and keep the in-memory timer path.
  }
}

function resolvePendingNativeResponse(response) {
  const pending = pendingNativeResponse;
  pendingNativeResponse = null;
  if (pending) {
    clearTimeout(pending.timeoutId);
    pending.resolve(response);
  }
}

function setDisconnectedState(errorValue) {
  nativePortConnected = false;
  nativePort = null;
  runtimeStatus.connectionState = "disconnected";

  const normalized = normalizeError(errorValue);
  if (normalized) {
    runtimeStatus.lastError = normalized;
  }

  updateRegistrationState(normalized);
}

function handleNativeHostMessage(message) {
  resolvePendingNativeResponse(message || { ok: false, error: "empty-response" });
}

function handleNativeHostDisconnect() {
  const lastErrorMessage = chrome.runtime.lastError ? chrome.runtime.lastError.message : "native-host-disconnected";
  const normalized = normalizeError(lastErrorMessage) || "native-host-disconnected";
  resolvePendingNativeResponse({ ok: false, error: normalized });
  setDisconnectedState(normalized);
  reconnectDelayMs = Math.min(RECONNECT_MAX_MS, reconnectDelayMs * 2);
  scheduleReconnect();
}

async function connectNativePort() {
  if (nativePortConnected && nativePort) {
    return true;
  }

  if (nativeConnectPromise) {
    return nativeConnectPromise;
  }

  nativeConnectPromise = new Promise((resolve) => {
    clearReconnectSchedule();
    runtimeStatus.connectionState = "connecting";

    try {
      const port = chrome.runtime.connectNative(HOST_NAME);
      nativePort = port;
      nativePortConnected = true;
      runtimeStatus.connectionState = "waiting";
      runtimeStatus.lastError = "";
      runtimeStatus.registrationState = "registered";
      reconnectDelayMs = RECONNECT_BASE_MS;
      clearReconnectSchedule();

      port.onMessage.addListener(handleNativeHostMessage);
      port.onDisconnect.addListener(handleNativeHostDisconnect);
      resolve(true);
    } catch (error) {
      const normalized = normalizeError(error) || "native-host-connect-failed";
      runtimeStatus.lastError = normalized;
      runtimeStatus.lastSendOk = false;
      setDisconnectedState(normalized);
      scheduleReconnect();
      resolve(false);
    } finally {
      nativeConnectPromise = null;
    }
  });

  return nativeConnectPromise;
}

async function ensureNativePort() {
  if (nativePortConnected && nativePort) {
    return true;
  }

  return connectNativePort();
}

async function sendNativePayload(payload) {
  const connected = await ensureNativePort();
  if (!connected || !nativePort) {
    runtimeStatus.lastSendAt = Date.now();
    runtimeStatus.lastSendOk = false;
    runtimeStatus.lastSendResult = runtimeStatus.lastError || "native-host-unavailable";
    if (!runtimeStatus.lastError) {
      runtimeStatus.lastError = "native-host-unavailable";
    }
    return false;
  }

  const responsePromise = new Promise((resolve) => {
    const timeoutId = setTimeout(() => {
      if (!pendingNativeResponse || pendingNativeResponse.timeoutId !== timeoutId) {
        return;
      }

      pendingNativeResponse = null;
      resolve({ ok: false, error: "native-host-timeout" });
    }, NATIVE_RESPONSE_TIMEOUT_MS);

    pendingNativeResponse = { resolve, timeoutId };
  });

  try {
    nativePort.postMessage(payload);
  } catch (error) {
    pendingNativeResponse = null;
    const normalized = normalizeError(error) || "native-host-post-failed";
    runtimeStatus.lastSendAt = Date.now();
    runtimeStatus.lastSendOk = false;
    runtimeStatus.lastSendResult = normalized;
    runtimeStatus.lastError = normalized;
    setDisconnectedState(normalized);
    scheduleReconnect();
    return false;
  }

  const response = await responsePromise;
  runtimeStatus.lastSendAt = Date.now();

  if (response && response.ok) {
    runtimeStatus.lastSendOk = true;
    runtimeStatus.lastSendResult = "ok";
    runtimeStatus.lastError = "";
    runtimeStatus.connectionState = "connected";
    runtimeStatus.registrationState = "registered";
    reconnectDelayMs = RECONNECT_BASE_MS;
    return true;
  }

  const normalized = normalizeError(response && response.error ? response.error : "native-host-error") || "native-host-error";
  runtimeStatus.lastSendOk = false;
  runtimeStatus.lastSendResult = normalized;
  runtimeStatus.lastError = normalized;
  runtimeStatus.connectionState = normalized === "app-not-running" ? "disconnected" : "waiting";
  updateRegistrationState(normalized);

  if (normalized === "native-host-timeout") {
    try {
      if (nativePort) {
        nativePort.disconnect();
      }
    } catch {
      // Ignore disconnect failures; the reconnect path still runs.
    }

    setDisconnectedState(normalized);
  }

  if (normalized !== "app-not-running") {
    reconnectDelayMs = Math.min(RECONNECT_MAX_MS, reconnectDelayMs * 2);
    scheduleReconnect(reconnectDelayMs);
  }

  return false;
}

function enqueueNativeSend(payload) {
  nativeSendChain = nativeSendChain
    .catch(() => true)
    .then(() => sendNativePayload(payload));

  return nativeSendChain;
}

async function forwardHint(force = false) {
  const payload = buildOutboundPayload();
  const signature = JSON.stringify(payload);
  const now = Date.now();

  if (!force && signature === lastSignature && now - lastSentAt < RESEND_AFTER_MS) {
    return;
  }

  await enqueueNativeSend(payload);
  lastSignature = signature;
  lastSentAt = now;
}

function getPopupState() {
  pruneStaleHints();

  if (!nativePortConnected && !nativeConnectPromise) {
    scheduleReconnect(0);
  }

  return {
    settings,
    runtimeStatus: {
      ...runtimeStatus
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

chrome.runtime.onInstalled.addListener(() => {
  void ensureSettingsLoaded().then(async () => {
    await saveSettings();
    await injectIntoOpenTabs();
    scheduleReconnect(0);
  });
});

chrome.runtime.onStartup.addListener(() => {
  void ensureSettingsLoaded().then(async () => {
    await injectIntoOpenTabs();
    scheduleReconnect(0);
  });
});

chrome.alarms.onAlarm.addListener((alarm) => {
  if (!alarm || alarm.name !== RECONNECT_ALARM_NAME) {
    return;
  }

  void ensureNativePort().then((connected) => {
    if (connected) {
      void forwardHint(true);
    }
  });
});

chrome.tabs.onUpdated.addListener((tabId, changeInfo, tab) => {
  if (changeInfo.status === "complete") {
    void ensureContentScriptForTab(tabId, (tab && tab.url) || "");
  }
});

chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
  void ensureSettingsLoaded().then(async () => {
    if (message && message.type === "lrp-popup-get-state") {
      sendResponse(getPopupState());
      return;
    }

    if (message && message.type === "lrp-popup-set-settings") {
      settings = normalizeSettings(message.settings);
      await saveSettings();
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
      return;
    }

    const tabId = sender.tab && sender.tab.id;
    if (!tabId) {
      sendResponse(null);
      return;
    }

    if (message && message.type === "lrp-media-state" && message.payload) {
      cancelPendingClear(tabId);

      if (!settings.enabled || !isHintAllowed(message.payload)) {
        latestByTab.delete(tabId);
        runtimeStatus.activeHints = latestByTab.size;
        await forwardHint(true);
        sendResponse(null);
        return;
      }

      latestByTab.set(tabId, {
        ...message.payload,
        tabId,
        timestamp: Date.now()
      });
      runtimeStatus.activeHints = latestByTab.size;
      await forwardHint(false);
      sendResponse(null);
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
      sendResponse(null);
      return;
    }

    sendResponse(null);
  });

  return true;
});

chrome.tabs.onRemoved.addListener((tabId) => {
  cancelPendingClear(tabId);
  latestByTab.delete(tabId);
  runtimeStatus.activeHints = latestByTab.size;
  void forwardHint(true);
});

void ensureSettingsLoaded().then(() => {
  scheduleReconnect(0);
});

setInterval(() => {
  void forwardHint(false);
}, HEARTBEAT_MS);
