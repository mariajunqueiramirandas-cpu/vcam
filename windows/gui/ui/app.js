// VCam's interface. The native host (vcam-gui.exe) owns the cameras and the
// sources; this page shows their state and sends the user's choices back.
//
// Host -> page: init, status (twice a second), lists, file, thumb, frame
// (pixels in a shared buffer), toast, busy.
// Page -> host: ready, select, previewSize, frameAck, start, stop, refresh,
// pick, saveState, install, saveSettings, resetPlaceholder, openLogs,
// restartService, repair, uninstall.
"use strict";

const host = window.chrome && window.chrome.webview;
const send = (message) => host && host.postMessage(message);
const $ = (id) => document.getElementById(id);

const SOURCES = [
  { kind: "bars", label: "Test bars", icon: "i-bars" },
  { kind: "colour", label: "Colour", icon: "i-colour" },
  { kind: "image", label: "Picture", icon: "i-image" },
  { kind: "video", label: "Video", icon: "i-video" },
  { kind: "webcam", label: "Camera", icon: "i-webcam" },
  { kind: "screen", label: "Screen", icon: "i-screen" },
  { kind: "window", label: "Window", icon: "i-window" },
];
// What the start button says for each source.
const START_LABELS = {
  bars: "Show test bars", colour: "Show the colour", image: "Show the picture", video: "Play the video",
  webcam: "Show the camera", screen: "Share the screen", window: "Share the window",
};
const PRESET_COLOURS = ["#000000", "#ffffff", "#1f6feb", "#0f7b6c", "#7c3aed", "#d9480f", "#00b140"];

const state = {
  init: null,
  camera: 0,
  cameras: [],          // per camera: { source: {...}, options: {...}, running }
  status: [],           // per camera, from the host
  lists: { monitors: [], windows: [], webcams: [] },
  files: {},            // path -> { name, thumb }
  windowFilter: "",
  previewBuffer: null,
  wasLive: [],
};

function defaultCamera() {
  return {
    source: { kind: "bars", color: "#1f6feb", path: "", loop: true, webcam: "", monitor: 1, window: "", cursor: true },
    options: { scale: "fit", mirror: false, flip: false, background: "#000000", always: false },
    running: false,
  };
}

function el(tag, attrs = {}, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(attrs)) {
    if (value === undefined || value === null || value === false) continue;
    if (key === "class") node.className = value;
    else if (key === "text") node.textContent = value;
    else if (key === "style") node.style.cssText = value;  // CSSOM: allowed by the page's CSP
    else if (key.startsWith("on")) node.addEventListener(key.slice(2), value);
    else node.setAttribute(key, value === true ? "" : value);
  }
  for (const child of children.flat()) {
    if (child !== null && child !== undefined && child !== false) {
      node.append(child instanceof Node ? child : document.createTextNode(child));
    }
  }
  return node;
}

function icon(id) {
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  const use = document.createElementNS("http://www.w3.org/2000/svg", "use");
  use.setAttribute("href", "#" + id);
  svg.append(use);
  return svg;
}

const current = () => state.cameras[state.camera];
const cameraName = (i) => state.init.config.names[i] || (i === 0 ? "VCam" : `VCam ${i + 1}`);

function saveState() {
  send({ type: "saveState", state: { camera: state.camera, cameras: state.cameras } });
}

// --- start-up ------------------------------------------------------------

function applyInit(init) {
  state.init = init;
  document.documentElement.classList.toggle("mica", !!init.mica);
  state.accent = init.accent || null;
  applyAccent();
  if (init.lists) state.lists = init.lists;

  if (!init.installed) {
    showWelcome();
    return;
  }
  $("welcome").hidden = true;
  $("app").hidden = false;

  const saved = init.saved || {};
  const count = init.config.cameras;
  state.cameras = [];
  for (let i = 0; i < count; i++) {
    const merged = defaultCamera();
    const s = saved.cameras && saved.cameras[i];
    if (s) {
      Object.assign(merged.source, s.source || {});
      Object.assign(merged.options, s.options || {});
      merged.running = !!s.running;
    }
    state.cameras.push(merged);
  }
  state.camera = Math.min(saved.camera || 0, count - 1);
  state.wasLive = new Array(count).fill(false);
  renderTabs();
  renderCamera();
  send({ type: "select", camera: state.camera });
}

// The system accent colour, in its light-theme or dark-theme shade.
const darkQuery = window.matchMedia("(prefers-color-scheme: dark)");
function applyAccent() {
  const a = state.accent;
  const root = document.documentElement.style;
  if (!a || !a.light) {
    root.removeProperty("--accent");
    root.removeProperty("--accent-text");
    return;
  }
  const dark = darkQuery.matches;
  root.setProperty("--accent", dark ? a.dark : a.light);
  root.setProperty("--accent-text", dark ? "#000000" : "#ffffff");
}
darkQuery.addEventListener("change", applyAccent);

function showWelcome() {
  $("app").hidden = true;
  $("welcome").hidden = false;
  const init = state.init;
  $("welcome-backend").textContent = init.mfSupported
    ? "Every app can use it, including the Windows Camera app."
    : "On Windows 10, desktop apps such as Zoom, Teams, browsers, Discord and OBS can use it. The Windows Camera app can't.";
  $("install").textContent = init.elevated ? "Install" : "Install (asks for administrator permission)";
  $("welcome-note").textContent = "";
  setTimeout(() => $("welcome-name").focus(), 50);
}

// --- tabs ----------------------------------------------------------------

function renderTabs() {
  const nav = $("cams");
  nav.replaceChildren();
  state.cameras.forEach((_, i) => {
    const tab = el("button", {
      class: "cam-tab", role: "tab", "aria-selected": String(i === state.camera),
      onclick: () => selectCamera(i),
    }, el("span", { class: "dot", "data-tally": tallyOf(i).kind }), cameraName(i));
    nav.append(tab);
  });
}

function selectCamera(i) {
  if (i === state.camera) return;
  state.camera = i;
  renderTabs();
  renderCamera();
  send({ type: "select", camera: i });
  saveState();
}

// --- inspector -----------------------------------------------------------

function renderCamera() {
  renderSources();
  renderSourceOptions();
  renderPicture();
  renderGo();
  renderStatus();
}

function renderSources() {
  const cam = current();
  const st = state.status[state.camera];
  const box = $("sources");
  box.replaceChildren();
  for (const s of SOURCES) {
    const checked = cam.source.kind === s.kind;
    box.append(el("button", {
      class: "source", role: "radio", "aria-checked": String(checked), title: s.label,
      onclick: () => changeSource({ kind: s.kind }),
    }, icon(s.icon), s.label, checked && st && st.running ? el("span", { class: "on" }) : null));
  }
}

function changeSource(patch) {
  const cam = current();
  // Lists of screens, windows and cameras change; refresh them when they are needed.
  if (patch.kind && patch.kind !== cam.source.kind && ["screen", "window", "webcam"].includes(patch.kind)) {
    send({ type: "refresh" });
  }
  Object.assign(cam.source, patch);
  renderSources();
  renderSourceOptions();
  restartIfRunning();
}

function changeOptions(patch) {
  Object.assign(current().options, patch);
  renderPicture();
  restartIfRunning();
}

function restartIfRunning() {
  const cam = current();
  if (cam.running) start();
  else saveState();
  renderGo();
}

function fileRow(kind, path, placeholderText, buttonText) {
  const info = path ? state.files[path] : null;
  const name = path ? (info ? info.name : path.split(/[\\/]/).pop()) : placeholderText;
  const thumb = el("canvas", { class: "thumb", width: 64, height: 36 });
  if (info && info.thumb) drawThumb(thumb, info.thumb);
  else drawGlyphThumb(thumb, kind);
  return el("div", { class: "file-row" },
    thumb,
    el("span", { class: "name" + (path ? "" : " empty"), title: path || "" }, name),
    el("button", { class: "quiet-btn", onclick: () => send({ type: "pick", kind, id: state.camera }) }, buttonText));
}

function pointerSwitch() {
  const cam = current();
  const input = el("input", { type: "checkbox", class: "switch" });
  input.checked = cam.source.cursor;
  input.addEventListener("change", () => changeSource({ cursor: input.checked }));
  return el("label", { class: "switch-row" }, el("span", {}, "Show the mouse pointer"), input);
}

function renderSourceOptions() {
  const cam = current();
  const box = $("source-options");
  box.replaceChildren();
  const s = cam.source;
  switch (s.kind) {
    case "bars":
      box.append(el("p", { class: "note" }, "Colour bars with a moving marker and a clock. Use them to check framing, colours and smoothness in the other app."));
      break;
    case "colour": {
      const swatches = el("div", { class: "swatches", role: "radiogroup", "aria-label": "Colour" });
      for (const c of PRESET_COLOURS) {
        swatches.append(el("button", {
          role: "radio", "aria-checked": String(s.color.toLowerCase() === c), title: c,
          style: `background:${c}`, onclick: () => changeSource({ color: c }),
        }));
      }
      const custom = el("input", { type: "color", value: s.color, "aria-label": "Any colour" });
      custom.addEventListener("input", () => changeSource({ color: custom.value }));
      const isCustom = !PRESET_COLOURS.includes(s.color.toLowerCase());
      swatches.append(el("label", { class: "custom", "aria-checked": String(isCustom), title: "Any colour" }, custom));
      box.append(swatches);
      break;
    }
    case "image":
      box.append(fileRow("image", s.path, "No picture chosen", s.path ? "Change" : "Choose"));
      break;
    case "video": {
      box.append(fileRow("video", s.path, "No video chosen", s.path ? "Change" : "Choose"));
      const loop = el("input", { type: "checkbox", class: "switch" });
      loop.checked = s.loop;
      loop.addEventListener("change", () => changeSource({ loop: loop.checked }));
      box.append(el("label", { class: "switch-row" }, el("span", {}, "Play in a loop"), loop));
      break;
    }
    case "webcam": {
      const cams = state.lists.webcams;
      if (!cams.length) {
        box.append(el("p", { class: "note" }, "No other camera is connected."));
        break;
      }
      const select = el("select", { "aria-label": "Camera" });
      cams.forEach((c) => {
        const option = el("option", { value: c.name }, c.name);
        if (c.name === s.webcam) option.selected = true;
        select.append(option);
      });
      if (!s.webcam || !cams.some((c) => c.name === s.webcam)) s.webcam = cams[0].name;
      select.addEventListener("change", () => changeSource({ webcam: select.value }));
      box.append(select, el("p", { class: "note" }, "The camera turns off while no app is watching."));
      break;
    }
    case "screen":
      box.append(monitorMap(), pointerSwitch());
      break;
    case "window":
      box.append(windowPicker(), pointerSwitch());
      break;
  }
}

// Monitors drawn at their real positions, scaled into the box.
function monitorMap() {
  const monitors = state.lists.monitors;
  const map = el("div", { class: "monitor-map", role: "radiogroup", "aria-label": "Screen" });
  if (!monitors.length) return map;
  const minX = Math.min(...monitors.map((m) => m.x)), minY = Math.min(...monitors.map((m) => m.y));
  const maxX = Math.max(...monitors.map((m) => m.x + m.width)), maxY = Math.max(...monitors.map((m) => m.y + m.height));
  const boxW = 314, boxH = 112, pad = 12;
  const scale = Math.min((boxW - 2 * pad) / (maxX - minX), (boxH - 2 * pad) / (maxY - minY));
  const offX = (boxW - (maxX - minX) * scale) / 2, offY = (boxH - (maxY - minY) * scale) / 2;
  const cam = current();
  for (const m of monitors) {
    const b = el("button", {
      role: "radio", "aria-checked": String(cam.source.monitor === m.number),
      title: `${m.width} × ${m.height}${m.primary ? ", main display" : ""}`,
      style: `left:${offX + (m.x - minX) * scale + 2}px;top:${offY + (m.y - minY) * scale + 2}px;` +
             `width:${m.width * scale - 4}px;height:${m.height * scale - 4}px`,
      onclick: () => { changeSource({ monitor: m.number }); },
    }, el("span", {}, String(m.number), el("small", {}, m.primary ? "main" : `${m.width}×${m.height}`)));
    map.append(b);
  }
  return map;
}

function windowPicker() {
  const cam = current();
  const wrap = el("div", { class: "source-options" });
  const input = el("input", { type: "search", placeholder: "Find a window", value: state.windowFilter, "aria-label": "Find a window" });
  const list = el("div", { class: "window-list", role: "radiogroup", "aria-label": "Window" });
  const fill = () => {
    list.replaceChildren();
    const needle = state.windowFilter.toLowerCase();
    const windows = state.lists.windows.filter((w) =>
      !needle || w.title.toLowerCase().includes(needle) || (w.process || "").toLowerCase().includes(needle));
    if (!windows.length) {
      list.append(el("div", { class: "empty" }, needle ? "No window matches." : "No windows to show."));
    }
    for (const w of windows) {
      list.append(el("button", {
        role: "radio", "aria-checked": String(cam.source.window === w.id), title: w.title,
        onclick: () => { changeSource({ window: w.id, windowTitle: w.title }); },
      }, el("span", { class: "title" }, w.title), el("span", { class: "proc" }, (w.process || "").replace(/\.exe$/i, ""))));
    }
  };
  input.addEventListener("input", () => { state.windowFilter = input.value; fill(); });
  fill();
  const refresh = el("button", { class: "icon-btn", title: "Refresh the list", "aria-label": "Refresh the list", onclick: () => send({ type: "refresh" }) }, icon("i-refresh"));
  const search = el("div", { class: "search" }, el("div", { class: "input-wrap" }, icon("i-search"), input), refresh);
  wrap.append(search, list);
  return wrap;
}

function renderPicture() {
  const o = current().options;
  for (const b of $("scale").querySelectorAll("button")) {
    b.setAttribute("aria-checked", String(b.dataset.value === o.scale));
  }
  $("mirror").setAttribute("aria-pressed", String(o.mirror));
  $("flip").setAttribute("aria-pressed", String(o.flip));
  $("background").value = o.background;
  $("background-swatch").style.background = o.background;
  $("background-row").hidden = o.scale !== "fit";
  $("always").checked = o.always;
}

function renderGo() {
  const cam = current();
  const go = $("go");
  go.classList.toggle("stop", cam.running);
  go.textContent = cam.running ? "Stop" : START_LABELS[cam.source.kind];
  go.disabled = !cam.running && !sourceReady(cam.source);
}

function sourceReady(s) {
  if (s.kind === "image" || s.kind === "video") return !!s.path;
  if (s.kind === "window") return !!s.window;
  if (s.kind === "webcam") return state.lists.webcams.length > 0;
  return true;
}

function start() {
  const cam = current();
  cam.running = true;
  send({ type: "start", camera: state.camera, source: cam.source, options: cam.options });
  saveState();
  renderGo();
}

function stop() {
  const cam = current();
  cam.running = false;
  send({ type: "stop", camera: state.camera });
  saveState();
  renderGo();
}

// --- status --------------------------------------------------------------

function tallyOf(i) {
  const st = state.status[i];
  if (!st) return { kind: "off", title: "Off", detail: "Apps see the placeholder picture" };
  const watching = st.consumer && st.consumer.active;
  const app = st.consumer && st.consumer.app;
  if (st.running) {
    if (watching) return { kind: "live", title: "On air", detail: app ? `${app.replace(/\.exe$/i, "")} is receiving` : "An app is receiving" };
    return { kind: "standby", title: "Standby", detail: `Waiting for an app to open ${cameraName(i)}` };
  }
  if (st.ended) {
    return { kind: "off", title: "Stopped", detail: st.error || "The source ended" };
  }
  if (st.producer && st.producer.external) {
    const by = st.producer.app ? `Fed by ${st.producer.app}` : "Fed by another program";
    return { kind: watching ? "live" : "standby", title: watching ? "On air" : "Standby", detail: by };
  }
  if (watching) return { kind: "live", title: "On air", detail: "Apps see the placeholder picture" };
  return { kind: "off", title: "Off", detail: "Apps see the placeholder picture" };
}

function applyStatus(cameras) {
  state.status = cameras;
  cameras.forEach((st, i) => {
    const cam = state.cameras[i];
    if (!cam) return;
    // The host stopped a source on its own (end of a file, closed window, error).
    if (cam.running && !st.running && st.ended) {
      cam.running = false;
      if (i === state.camera) renderGo();
      saveState();
    }
  });
  renderStatus();
  for (const [i, tab] of [...$("cams").children].entries()) {
    tab.querySelector(".dot").dataset.tally = tallyOf(i).kind;
  }
}

function setReadout(id, text) {
  const dd = $(id);
  dd.textContent = text ?? "–";
  dd.classList.toggle("idle", text === null || text === undefined);
}

function renderStatus() {
  if (!state.cameras.length) return;
  const i = state.camera;
  const st = state.status[i];
  const tally = tallyOf(i);
  const vf = $("viewfinder");
  if (vf.dataset.tally !== tally.kind) {
    if (tally.kind === "live" && !state.wasLive[i]) {
      vf.classList.remove("went-live");
      void vf.offsetWidth;
      vf.classList.add("went-live");
    }
    vf.dataset.tally = tally.kind;
  }
  state.wasLive[i] = tally.kind === "live";
  $("tally-title").textContent = tally.title;
  $("tally-detail").textContent = tally.detail;

  const c = st && st.consumer;
  const active = c && c.active;
  setReadout("r-size", active ? `${c.width} × ${c.height}` : null);
  setReadout("r-format", active ? c.format : null);
  setReadout("r-rate", active ? `${formatFps(c.fps)} fps` : null);
  setReadout("r-sent", st && st.running && active ? `${st.fps.toFixed(1)} fps` : null);
  setReadout("r-delivered", st && st.delivered ? group(st.delivered) : null);
  if (active) setAspect(c.width, c.height);
  renderSourcesRunningDot();
}

function renderSourcesRunningDot() {
  const st = state.status[state.camera];
  const running = st && st.running;
  for (const b of $("sources").children) {
    const on = b.querySelector(".on");
    const want = running && b.getAttribute("aria-checked") === "true";
    if (want && !on) b.append(el("span", { class: "on" }));
    if (!want && on) on.remove();
  }
}

const formatFps = (fps) => (Math.abs(fps - Math.round(fps)) < 0.01 ? String(Math.round(fps)) : fps.toFixed(2));
const group = (n) => n.toLocaleString("en-US").replace(/,/g, " ");

// --- preview -------------------------------------------------------------

let aspect = 16 / 9;
let lastPreviewSize = "";

function setAspect(w, h) {
  if (!w || !h) return;
  const a = w / h;
  if (Math.abs(a - aspect) > 0.001) {
    aspect = a;
    layoutViewfinder();
  }
}

const READOUTS_HEIGHT = 50, READOUTS_GAP = 14;

function layoutViewfinder() {
  const box = $("monitor");
  const W = box.clientWidth, H = box.clientHeight - READOUTS_HEIGHT - READOUTS_GAP;
  if (W < 10 || H < 10) return;
  let w = W, h = W / aspect;
  if (h > H) { h = H; w = H * aspect; }
  w = Math.floor(w); h = Math.floor(h);
  const vf = $("viewfinder");
  vf.style.width = w + "px";
  vf.style.height = h + "px";
  const group = $("vf-group");
  group.style.width = w + "px";
  group.style.left = Math.floor((W - w) / 2) + "px";
  group.style.top = Math.max(0, Math.floor((box.clientHeight - (h + READOUTS_GAP + READOUTS_HEIGHT)) / 2)) + "px";
  $("readouts").classList.toggle("compact", w < 640);
  // The host renders the preview at the canvas' real pixel size (capped).
  const dpr = window.devicePixelRatio || 1;
  let pw = Math.round(w * dpr), ph = Math.round(h * dpr);
  const cap = Math.min(1, 1280 / pw, 720 / ph);
  pw = Math.max(2, Math.round(pw * cap) & ~1);
  ph = Math.max(2, Math.round(ph * cap) & ~1);
  const key = `${pw}x${ph}`;
  if (key !== lastPreviewSize) {
    lastPreviewSize = key;
    send({ type: "previewSize", w: pw, h: ph });
  }
}

function drawFrame(message) {
  const canvas = $("preview");
  const buffer = state.previewBuffer;
  if (!buffer || message.camera !== state.camera) {
    send({ type: "frameAck" });
    return;
  }
  const { w, h } = message;
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
  }
  const bytes = w * h * 4;
  if (bytes <= buffer.byteLength) {
    const image = new ImageData(new Uint8ClampedArray(buffer, 0, bytes).slice(), w, h);
    canvas.getContext("2d").putImageData(image, 0, 0);
    $("viewfinder").classList.remove("blank");
  }
  send({ type: "frameAck" });
}

function drawThumb(canvas, thumb) {
  const raw = atob(thumb.data);
  const bytes = new Uint8ClampedArray(raw.length);
  for (let i = 0; i < raw.length; i++) bytes[i] = raw.charCodeAt(i);
  const tmp = document.createElement("canvas");
  tmp.width = thumb.w;
  tmp.height = thumb.h;
  tmp.getContext("2d").putImageData(new ImageData(bytes, thumb.w, thumb.h), 0, 0);
  const ctx = canvas.getContext("2d");
  ctx.fillStyle = "#000";
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  const s = Math.max(canvas.width / thumb.w, canvas.height / thumb.h);
  const dw = thumb.w * s, dh = thumb.h * s;
  ctx.drawImage(tmp, (canvas.width - dw) / 2, (canvas.height - dh) / 2, dw, dh);
}

function drawGlyphThumb(canvas, kind) {
  const ctx = canvas.getContext("2d");
  ctx.fillStyle = getComputedStyle(document.documentElement).getPropertyValue("--subtle-hover") || "#eee";
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  ctx.strokeStyle = getComputedStyle(document.documentElement).getPropertyValue("--text-3") || "#888";
  ctx.lineWidth = 1.4;
  const x = canvas.width / 2, y = canvas.height / 2;
  ctx.strokeRect(x - 11, y - 8, 22, 16);
  ctx.beginPath();
  if (kind === "video") {
    ctx.moveTo(x - 3, y - 4); ctx.lineTo(x + 4, y); ctx.lineTo(x - 3, y + 4); ctx.closePath();
  } else {
    ctx.moveTo(x - 9, y + 6); ctx.lineTo(x - 3, y); ctx.lineTo(x + 2, y + 4); ctx.lineTo(x + 5, y + 1); ctx.lineTo(x + 9, y + 6);
  }
  ctx.stroke();
}

// --- settings ------------------------------------------------------------

let draft = null;

function openSettings() {
  const c = state.init.config;
  draft = {
    names: state.cameras.map((_, i) => cameraName(i)),
    maxRes: `${c.maxWidth}x${c.maxHeight}`,
    allow60: !!c.allow60,
    logging: !!c.logging,
    autostart: !!state.init.settings.autostart,
    tray: !!state.init.settings.tray,
  };
  renderSettings();
  $("scrim").hidden = false;
  $("settings").hidden = false;
  $("h-settings").focus();
}

function closeSettings() {
  $("scrim").hidden = true;
  $("settings").hidden = true;
  draft = null;
}

function settingsChanged() {
  if (!draft) return false;
  const c = state.init.config;
  const names = state.cameras.map((_, i) => cameraName(i));
  return draft.names.length !== names.length || draft.names.some((n, i) => n !== names[i]) ||
    draft.maxRes !== `${c.maxWidth}x${c.maxHeight}` || draft.allow60 !== !!c.allow60 ||
    draft.logging !== !!c.logging || draft.autostart !== !!state.init.settings.autostart ||
    draft.tray !== !!state.init.settings.tray;
}

function renderSettings() {
  const fields = $("name-fields");
  fields.replaceChildren();
  draft.names.forEach((name, i) => {
    const input = el("input", { type: "text", value: name, maxlength: 60, spellcheck: "false", "aria-label": `Name of camera ${i + 1}` });
    input.addEventListener("input", () => { draft.names[i] = input.value; updateSettingsFoot(); });
    fields.append(el("label", {}, String(i + 1), input));
  });
  $("add-camera").disabled = draft.names.length >= 4;
  $("remove-camera").disabled = draft.names.length <= 1;
  const maxRes = $("max-res");
  if (![...maxRes.options].some((o) => o.value === draft.maxRes)) {
    maxRes.append(el("option", { value: draft.maxRes }, draft.maxRes.replace("x", " × ")));
  }
  maxRes.value = draft.maxRes;
  $("allow60").checked = draft.allow60;
  $("logging").checked = draft.logging;
  $("autostart").checked = draft.autostart;
  $("tray").checked = draft.tray;
  const mf = state.init.backend === "mediafoundation";
  $("restart-service").hidden = !mf;
  $("about-backend").textContent = mf
    ? "Windows virtual camera: every app can use VCam, including the Windows Camera app."
    : "DirectShow camera: desktop apps such as Zoom, Teams, browsers and OBS can use VCam. The Windows Camera app can't.";
  $("about-version").textContent = `Version ${state.init.version}, installed in ${state.init.config.installDir}`;
  updateSettingsFoot();
}

function updateSettingsFoot() {
  const changed = settingsChanged();
  $("save-settings").disabled = !changed || draft.names.some((n) => !n.trim());
  const countChanged = draft.names.length !== state.cameras.length;
  const renamed = draft.names.some((n, i) => i < state.cameras.length && n !== cameraName(i));
  $("settings-note").textContent = countChanged || renamed
    ? "Apps using a camera that changes lose it for a moment."
    : changed ? "New video modes apply the next time an app opens VCam." : "";
}

function saveSettings() {
  const [w, h] = draft.maxRes.split("x").map(Number);
  send({
    type: "saveSettings",
    names: draft.names.map((n) => n.trim()),
    cameras: draft.names.length,
    maxWidth: w, maxHeight: h,
    allow60: draft.allow60, logging: draft.logging,
    autostart: draft.autostart, tray: draft.tray,
  });
  closeSettings();
}

// --- busy, toasts ----------------------------------------------------------

function setBusy(text) {
  $("busy").hidden = !text;
  $("busy-text").textContent = text || "";
}

function toast(level, text) {
  const node = el("div", { class: `toast ${level}` }, icon("i-warn"), el("span", {}, text));
  $("toasts").append(node);
  setTimeout(() => node.remove(), level === "error" ? 9000 : 5000);
}

// --- wiring ----------------------------------------------------------------

function wire() {
  $("go").addEventListener("click", () => (current().running ? stop() : start()));
  for (const b of $("scale").querySelectorAll("button")) {
    b.addEventListener("click", () => changeOptions({ scale: b.dataset.value }));
  }
  $("mirror").addEventListener("click", () => changeOptions({ mirror: !current().options.mirror }));
  $("flip").addEventListener("click", () => changeOptions({ flip: !current().options.flip }));
  $("background").addEventListener("change", (e) => changeOptions({ background: e.target.value }));
  $("background").addEventListener("input", (e) => { $("background-swatch").style.background = e.target.value; });
  $("always").addEventListener("change", (e) => changeOptions({ always: e.target.checked }));

  $("open-settings").addEventListener("click", openSettings);
  $("close-settings").addEventListener("click", closeSettings);
  $("scrim").addEventListener("click", closeSettings);
  $("add-camera").addEventListener("click", () => {
    draft.names.push(`VCam ${draft.names.length + 1}`);
    renderSettings();
  });
  $("remove-camera").addEventListener("click", () => { draft.names.pop(); renderSettings(); });
  $("max-res").addEventListener("change", (e) => { draft.maxRes = e.target.value; updateSettingsFoot(); });
  for (const id of ["allow60", "logging", "autostart", "tray"]) {
    $(id).addEventListener("change", (e) => { draft[id] = e.target.checked; updateSettingsFoot(); });
  }
  $("save-settings").addEventListener("click", saveSettings);
  $("choose-placeholder").addEventListener("click", () => send({ type: "pick", kind: "placeholder" }));
  $("reset-placeholder").addEventListener("click", () => send({ type: "resetPlaceholder" }));
  $("open-logs").addEventListener("click", () => send({ type: "openLogs" }));
  $("restart-service").addEventListener("click", () => send({ type: "restartService" }));
  $("repair").addEventListener("click", () => send({ type: "repair" }));
  $("uninstall").addEventListener("click", () => send({ type: "uninstall" }));

  $("install").addEventListener("click", () => {
    const name = $("welcome-name").value.trim() || "VCam";
    send({ type: "install", name });
  });
  $("welcome-name").addEventListener("keydown", (e) => { if (e.key === "Enter") $("install").click(); });

  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape" && !$("settings").hidden) closeSettings();
    // Ctrl+1..4 switch cameras.
    if (e.ctrlKey && /^[1-4]$/.test(e.key) && Number(e.key) <= state.cameras.length) {
      selectCamera(Number(e.key) - 1);
      e.preventDefault();
    }
  });
  // Typing in a field must not reach the host's accelerators; nothing else needs the context menu.
  document.addEventListener("contextmenu", (e) => {
    if (!(e.target instanceof HTMLInputElement)) e.preventDefault();
  });

  new ResizeObserver(layoutViewfinder).observe($("monitor"));
  window.addEventListener("resize", layoutViewfinder);
}

function onMessage(event) {
  const m = event.data;
  switch (m.type) {
    case "init":
      applyInit(m);
      requestAnimationFrame(layoutViewfinder);
      break;
    case "status":
      if (state.cameras.length) applyStatus(m.cameras);
      break;
    case "lists":
      state.lists = m;
      if (state.cameras.length) { renderSourceOptions(); renderGo(); }
      break;
    case "file":
      state.files[m.path] = { name: m.name, thumb: m.thumb };
      if (m.kind === "image" || m.kind === "video") {
        const cam = state.cameras[m.id];
        if (cam) {
          if (m.id === state.camera) changeSource({ kind: m.kind, path: m.path });
          else { cam.source.kind = m.kind; cam.source.path = m.path; saveState(); }
        }
      }
      break;
    case "thumb":
      state.files[m.path] = { name: m.name, thumb: m.thumb };
      if (state.cameras.length) renderSourceOptions();
      break;
    case "placeholderThumb":
      if (m.thumb) drawThumb($("placeholder-thumb"), m.thumb);
      break;
    case "frame":
      drawFrame(m);
      break;
    case "blank":
      if (m.camera === state.camera) {
        $("viewfinder").classList.add("blank");
        $("vf-blank").textContent = m.text || "";
      }
      break;
    case "toast":
      toast(m.level || "info", m.text);
      break;
    case "busy":
      setBusy(m.text);
      break;
    case "welcomeNote":
      $("welcome-note").textContent = m.text;
      break;
    case "accent":
      state.accent = m;
      applyAccent();
      break;
    case "stopped": {
      // Stopped from the notification area.
      const cam = state.cameras[m.camera];
      if (cam) {
        cam.running = false;
        if (m.camera === state.camera) renderGo();
      }
      break;
    }
  }
}

if (host) {
  host.addEventListener("message", onMessage);
  host.addEventListener("sharedbufferreceived", (e) => {
    state.previewBuffer = e.getBuffer();
  });
}
wire();
send({ type: "ready" });
