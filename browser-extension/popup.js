const SITE_OPTIONS = [
  { key: "youtube", label: "YouTube" },
  { key: "youtube_music", label: "YouTube Music" },
  { key: "spotify", label: "Spotify Web" },
  { key: "soundcloud", label: "SoundCloud" },
  { key: "apple_music", label: "Apple Music" },
  { key: "amazon_music", label: "Amazon Music" },
  { key: "deezer", label: "Deezer" },
  { key: "tidal", label: "TIDAL" },
  { key: "jiosaavn", label: "JioSaavn" },
  { key: "gaana", label: "Gaana" },
  { key: "wynk", label: "Wynk Music" },
  { key: "bandcamp", label: "Bandcamp" },
  { key: "mixcloud", label: "Mixcloud" },
  { key: "twitch", label: "Twitch" }
];

const ui = {
  statusPill: document.getElementById("status-pill"),
  enabled: document.getElementById("enabled"),
  siteRows: document.getElementById("site-rows"),
  metricHint: document.getElementById("metric-hint"),
  metricTabs: document.getElementById("metric-tabs"),
  endpoint: document.getElementById("endpoint"),
  siteCheckboxes: new Map()
};

function createSiteRows() {
  for (const option of SITE_OPTIONS) {
    const row = document.createElement("label");
    row.className = "row small";

    const text = document.createElement("span");
    text.textContent = option.label;

    const checkbox = document.createElement("input");
    checkbox.type = "checkbox";
    checkbox.id = `site-${option.key}`;
    checkbox.dataset.siteKey = option.key;

    row.appendChild(text);
    row.appendChild(checkbox);
    ui.siteRows.appendChild(row);
    ui.siteCheckboxes.set(option.key, checkbox);

    checkbox.addEventListener("change", persist);
  }
}

function send(type, payload) {
  return new Promise((resolve) => {
    chrome.runtime.sendMessage({ type, ...payload }, (response) => {
      resolve(response || null);
    });
  });
}

function buildSettingsFromUI() {
  const siteEnabled = {};
  for (const option of SITE_OPTIONS) {
    const checkbox = ui.siteCheckboxes.get(option.key);
    siteEnabled[option.key] = !!(checkbox && checkbox.checked);
  }

  return {
    enabled: !!ui.enabled.checked,
    siteEnabled
  };
}

function setControlsEnabled(enabled) {
  for (const checkbox of ui.siteCheckboxes.values()) {
    checkbox.disabled = !enabled;
  }
}

function renderStatus(runtimeStatus) {
  const now = Date.now();
  const fresh = runtimeStatus.lastPostOk && now - runtimeStatus.lastPostAt < 10000;

  if (fresh) {
    ui.statusPill.textContent = "Connected";
    ui.statusPill.classList.remove("waiting");
    ui.statusPill.classList.add("connected");
  } else {
    ui.statusPill.textContent = "Waiting";
    ui.statusPill.classList.remove("connected");
    ui.statusPill.classList.add("waiting");
  }

  ui.metricHint.textContent = runtimeStatus.lastHintService || "None";
  ui.metricTabs.textContent = String(runtimeStatus.activeHints || 0);
  ui.endpoint.textContent = runtimeStatus.endpoint || "";
}

function render(state) {
  if (!state) return;

  const settings = state.settings || {};
  const siteEnabled = settings.siteEnabled || {};

  ui.enabled.checked = settings.enabled !== false;

  for (const option of SITE_OPTIONS) {
    const checkbox = ui.siteCheckboxes.get(option.key);
    if (!checkbox) continue;
    checkbox.checked = siteEnabled[option.key] !== false;
  }

  setControlsEnabled(ui.enabled.checked);
  renderStatus(state.runtimeStatus || {});
}

async function refresh() {
  const state = await send("lrp-popup-get-state");
  render(state);
}

async function persist() {
  setControlsEnabled(ui.enabled.checked);
  const state = await send("lrp-popup-set-settings", {
    settings: buildSettingsFromUI()
  });
  render(state);
}

ui.enabled.addEventListener("change", persist);

createSiteRows();
void refresh();
setInterval(() => {
  void refresh();
}, 2000);
