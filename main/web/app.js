// MatrixCube web UI. Talks to the on-device HTTP API at /api/*.

const EFFECTS = [
  { id: "off",        label: "Off"       },
  { id: "solid",      label: "Solid"     },
  { id: "rain",       label: "Rain"      },
  { id: "fire",       label: "Fire"      },
  { id: "rainbow",    label: "Rainbow"   },
  { id: "radial",     label: "Radial"    },
  { id: "balls",      label: "Balls"     },
  { id: "chase",      label: "Chase"     },
  { id: "plasma",     label: "Plasma"    },
  { id: "pingpong",   label: "Ping-pong" },
  { id: "fireworks",  label: "Fireworks" },
  { id: "matrix",     label: "Matrix"    },
  { id: "galaxy",     label: "Galaxy"    },
  { id: "spiral",     label: "Spiral"    },
  { id: "ripple",     label: "Ripple"    },
  { id: "warp",       label: "Warp"      },
  { id: "aurora",     label: "Aurora"    },
  { id: "lightning",  label: "Lightning" },
  { id: "breakout",   label: "Breakout"  },
  { id: "pulse",      label: "Pulse"     },
  { id: "tetris",     label: "Tetris"    },
  { id: "pendulum",   label: "Pendulum"  },
  { id: "disco",      label: "Disco"     },
  { id: "calib_face", label: "Face-ID"   },
  { id: "calib_edge", label: "Edge test" },
  { id: "face_test",  label: "QA test"   },
];
const FACES = ["TOP","BOTTOM","NORTH","SOUTH","EAST","WEST"];

let state = null;

// ---------- fetch helpers ----------
async function api(path, body) {
  const opts = body ? { method:"POST", headers:{"content-type":"application/json"}, body: JSON.stringify(body) } : {};
  const r = await fetch(path, opts);
  return r.ok ? r.json() : Promise.reject(await r.text());
}
async function refresh() {
  try {
    state = await api("/api/state");
    render();
    setStatus("connected");
  } catch (e) {
    setStatus("offline");
  }
}
function setStatus(t) { document.getElementById("status").textContent = t; }

// ---------- UI render ----------
function render() {
  renderEffects();
  renderBrightness();
  renderSolid();
  renderBreakout();
  renderOrientation();
  renderStartup();
  renderCalib();
}

function renderBreakout() {
  const card = document.getElementById("breakout-card");
  card.classList.toggle("hidden", state.effect !== "breakout");
  const cb = document.getElementById("breakout-single");
  cb.checked = !!(state.breakout && state.breakout.single_pixel);
  cb.onchange = () => api("/api/breakout", { single_pixel: cb.checked });
}

function renderOrientation() {
  const cur = state.orientation || "face_up";
  for (const btn of document.querySelectorAll("#orientation button")) {
    btn.classList.toggle("active", btn.dataset.mode === cur);
    btn.onclick = () => api("/api/orientation", { mode: btn.dataset.mode }).then(refresh);
  }
}

function renderStartup() {
  const su = state.startup || {};
  const mode = document.getElementById("startup-mode");
  const effSel = document.getElementById("startup-effect");
  const effWrap = document.getElementById("startup-effect-wrap");
  const iv = document.getElementById("random-interval");
  const ivOut = document.getElementById("random-interval-val");
  const rt = document.getElementById("random-toggle");
  const rs = document.getElementById("random-status");

  mode.value = su.mode || "last";
  effWrap.style.display = (mode.value === "specific") ? "" : "none";

  if (effSel.options.length === 0) {
    // Populate once with the visual effects (exclude calibration/off).
    const opts = EFFECTS.filter(e => !e.id.startsWith("calib_") && e.id !== "face_test" && e.id !== "off");
    for (const e of opts) {
      const o = document.createElement("option");
      o.value = e.id; o.textContent = e.label;
      effSel.appendChild(o);
    }
  }
  effSel.value = su.effect || "rainbow";

  iv.value = su.interval_s || 30;
  ivOut.textContent = iv.value;
  iv.oninput = () => { ivOut.textContent = iv.value; };

  mode.onchange = () => {
    effWrap.style.display = (mode.value === "specific") ? "" : "none";
  };

  rt.classList.toggle("active", !!su.random_active);
  rt.textContent = su.random_active ? "Cycling randomly — stop" : "Cycle randomly";
  rs.textContent = su.random_active ? `next switch in ≤${su.interval_s}s` : "";
  rt.onclick = () => api("/api/random", { on: !su.random_active }).then(refresh);

  document.getElementById("startup-save").onclick = () => {
    api("/api/startup", {
      mode: mode.value,
      effect: effSel.value,
      interval_s: parseInt(iv.value, 10),
    }).then(refresh);
  };
}

function renderEffects() {
  const box = document.getElementById("effects");
  box.innerHTML = "";
  for (const e of EFFECTS) {
    const b = document.createElement("button");
    b.textContent = e.label;
    if (state.effect === e.id) b.classList.add("active");
    b.onclick = async () => { await api("/api/effect", {name: e.id}); refresh(); };
    box.appendChild(b);
  }
}

function renderBrightness() {
  const s = document.getElementById("brightness");
  const o = document.getElementById("brightness-val");
  s.value = state.brightness;
  o.textContent = state.brightness;
  s.oninput = () => { o.textContent = s.value; };
  s.onchange = () => api("/api/brightness", { value: parseInt(s.value, 10) });
}

function renderSolid() {
  const card = document.getElementById("solid-card");
  card.classList.toggle("hidden", state.effect !== "solid");
  const r = document.getElementById("solid-r");
  const g = document.getElementById("solid-g");
  const b = document.getElementById("solid-b");
  const sw = document.getElementById("solid-preview");
  r.value = state.solid.r; g.value = state.solid.g; b.value = state.solid.b;
  const paint = () => {
    sw.style.background = `rgb(${r.value},${g.value},${b.value})`;
  };
  paint();
  const push = () => api("/api/solid", { r:+r.value, g:+g.value, b:+b.value });
  for (const el of [r,g,b]) { el.oninput = paint; el.onchange = push; }
}

function renderCalib() {
  document.getElementById("calib-step").textContent = state.calib_step;
  for (const el of document.querySelectorAll(".calib-step-echo")) {
    el.textContent = state.calib_step;
  }
  // Highlight the currently selected panel.
  for (const btn of document.querySelectorAll("#calib-panel-select button")) {
    btn.classList.toggle("active", +btn.dataset.panel === state.calib_step);
  }
  // Highlight the rotation currently applied to the selected panel.
  const curRot = state.panel_rot[state.calib_step] || 0;
  for (const btn of document.querySelectorAll("#calib-rot button")) {
    btn.classList.toggle("active", +btn.dataset.rot === curRot);
  }
  // Mirror button reflects current state.
  const mirrors = state.panel_mirror || [0,0,0,0,0,0];
  const mirrorBtn = document.getElementById("calib-mirror");
  const mirrorOn = !!mirrors[state.calib_step];
  mirrorBtn.classList.toggle("active", mirrorOn);
  mirrorBtn.textContent = mirrorOn ? "Mirror: ON (click to disable)"
                                   : "Mirror: off (click to enable)";

  const tbody = document.getElementById("calib-table");
  tbody.innerHTML = "";
  for (let i = 0; i < 6; i++) {
    const tr = document.createElement("tr");
    if (i === state.calib_step) tr.classList.add("active-row");
    tr.innerHTML = `<td>${i}</td><td>${FACES[state.panel_map[i]] || "?"}</td><td>${state.panel_rot[i] * 90}°</td><td>${mirrors[i] ? "yes" : "no"}</td>`;
    tbody.appendChild(tr);
  }
}

// ---------- calibration actions ----------
document.getElementById("calib-start").onclick = async () => {
  await api("/api/effect", { name: "calib_face" });
  await api("/api/calib/step", { panel: 0 });
  refresh();
};
// Panel selector — tap any panel to make it the "current" panel for face
// assignment, rotation, and mirror. In face-ID mode it also lights that
// panel up white so you can confirm which physical panel you're targeting.
for (const btn of document.querySelectorAll("#calib-panel-select button")) {
  btn.onclick = async () => {
    await api("/api/calib/step", { panel: +btn.dataset.panel });
    refresh();
  };
}
// Explicit Next button: advances the selected panel by 1 (wraps 5 → 0).
// No auto-advance from face-assign means you can tap a face, then freely
// switch to edge-match preview / toggle mirror / tweak rotation without
// the panel selector jumping forward on you.
document.getElementById("calib-next").onclick = async () => {
  if (!state) return;
  const next = (state.calib_step + 1) % 6;
  await api("/api/calib/step", { panel: next });
  refresh();
};
for (const btn of document.querySelectorAll("#calib-face-buttons button")) {
  btn.onclick = async () => {
    if (!state) return;
    await api("/api/calib/face", { panel: state.calib_step, face: btn.dataset.face });
    refresh();
  };
}
for (const btn of document.querySelectorAll("#calib-rot button")) {
  btn.onclick = async () => {
    if (!state) return;
    await api("/api/calib/rot", { panel: state.calib_step, rot: +btn.dataset.rot });
    refresh();
  };
}
document.getElementById("calib-mirror").onclick = async () => {
  if (!state) return;
  const cur = (state.panel_mirror || [0,0,0,0,0,0])[state.calib_step] || 0;
  await api("/api/calib/mirror", { panel: state.calib_step, mirror: cur ? 0 : 1 });
  refresh();
};
document.getElementById("calib-edge").onclick = () => api("/api/effect", { name: "calib_edge" }).then(refresh);
document.getElementById("calib-done").onclick = () => api("/api/calib/done", {}).then(refresh);
document.getElementById("calib-swap-ew").onclick = () => api("/api/calib/swap_ew", {}).then(refresh);
document.getElementById("calib-reset").onclick = () => {
  if (confirm("Reset panel calibration?")) api("/api/calib/reset", {}).then(refresh);
};

// ---------- OTA firmware upload ----------
// Uses XMLHttpRequest (not fetch) so we get upload-progress events for the
// potentially multi-MB binary. The cube reboots on success; the polling
// refresh loop will automatically reconnect and redraw once the new
// firmware is back up.
document.getElementById("ota-upload").onclick = () => {
  const input = document.getElementById("ota-file");
  if (!input.files || !input.files.length) return;
  const file = input.files[0];
  const status = document.getElementById("ota-status");
  const prog = document.getElementById("ota-progress");

  prog.classList.remove("hidden");
  prog.value = 0;
  status.textContent = `uploading ${file.name} (${(file.size/1024).toFixed(1)} KB)…`;

  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/api/ota", true);
  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) {
      prog.value = (e.loaded / e.total) * 100;
      status.textContent = `${(e.loaded/1024).toFixed(1)} / ${(e.total/1024).toFixed(1)} KB`;
    }
  };
  xhr.onload = () => {
    if (xhr.status === 200) {
      status.textContent = "upload ok — cube rebooting; reconnect to MatrixCube WiFi in ~15 s";
    } else {
      status.textContent = `upload failed (${xhr.status}): ${xhr.responseText || "no body"}`;
    }
  };
  xhr.onerror = () => { status.textContent = "network error during upload"; };
  xhr.send(file);
};

refresh();
setInterval(refresh, 4000);
