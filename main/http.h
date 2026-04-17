#pragma once
// HTTP server: embedded web UI + JSON control API.
//
// Routes:
//   GET  /              -> index.html
//   GET  /app.js, /style.css
//   GET  /api/state     -> current config/effect as JSON
//   POST /api/effect    -> { "name": "rain" | "fire" | ... }
//   POST /api/brightness-> { "value": 0..255 }
//   POST /api/solid     -> { "r":..,"g":..,"b":.. }
//   POST /api/calib/face-> { "panel": 0..5, "face": "T"|"B"|... }
//   POST /api/calib/rot -> { "panel": 0..5, "rot": 0|1|2|3 }
//   POST /api/calib/done-> marks calibration complete, persists
//   POST /api/calib/reset
//   GET  /generate_204, /hotspot-detect.html, ...  captive portal probes
//
// All JSON reads/writes hold config_lock around mutations.

void http_start(void);
