# MatrixCube

ESP32-S3 firmware for an LED cube built from **six 8×8 WS2812 panels** (384 LEDs total, single daisy-chained strip) wired into a cube. Ships with a built-in WiFi access point + captive-portal web UI so you can pick an effect, tune brightness, run panel calibration, and configure startup behavior from your phone — no app to install.

## Hardware

- **MCU**: ESP32-S3 N4R2 (4 MB flash, 2 MB quad PSRAM). Any S3 dev board works; edit `sdkconfig.defaults` if your flash / PSRAM differs.
- **LEDs**: 6 × 8×8 WS2812 panels arranged into a cube, wired into one chain. 384 pixels.
- **Data pin**: defined in [`main/hardware.h`](main/hardware.h) — edit `CUBE_DATA_GPIO` for your wiring. Avoid strapping pins and USB-Serial/JTAG pins on the S3.
- **Current**: software current limiter in [`main/render.c`](main/render.c) caps per-frame current at `config.max_ma` (default 4000 mA). Use an appropriately-rated PSU.

## Panel layout

The expected flat layout (before folding into a cube, all panels facing outward after fold) is:

```
      N
   W  T  E
      S
      B
```

Per-panel pixel (0, 0) is at the flat top-left of each panel. Calibration handles rotation/mirror mismatches; see the Calibration section below.

## Building and flashing

Requires ESP-IDF ≥ 5.1. Tested with 5.4.1 on macOS.

```sh
# One-time: source the IDF environment
source ~/esp/v5.4.1/esp-idf/export.sh

cd MatrixCube
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

If you change `sdkconfig.defaults` or the partition table, run `rm sdkconfig && idf.py build` to regenerate.

### OTA firmware updates

The firmware ships with two OTA slots (`ota_0`, `ota_1`, 1.5 MB each) plus an `otadata` partition — see `partitions.csv` for the exact layout. Once the cube is running a version that includes OTA, you never need a USB cable again:

1. `idf.py build` on your dev machine to produce `build/matrixcube.bin` (the app image; do **not** use `build/matrixcube-complete.bin`).
2. Join the `MatrixCube` WiFi and open the control UI.
3. In the **Firmware update** card, pick the `.bin`, hit **Upload**. Progress bar updates as the image streams over HTTP directly into the inactive OTA slot.
4. On success the cube marks that slot bootable, replies `{"ok":true}`, and `esp_restart()`s. It comes back up in ~5 s running the new firmware; reconnect to the WiFi.

Validation is automatic: `esp_ota_end()` checks the image magic and checksum, and a bad upload aborts without changing the boot partition — so a corrupted transfer leaves the running firmware intact. NVS (calibration, startup settings, orientation, etc.) lives in its own partition at `0x9000` and is preserved across OTA updates.

**First-time migration (factory → OTA layout).** If you're upgrading from a pre-OTA build, a USB reflash is required once to write the new partition table:

```sh
rm sdkconfig           # regenerate from sdkconfig.defaults
idf.py build
idf.py -p <PORT> flash  # writes new partition table + firmware to ota_0
```

Calibration survives (same NVS location). The old `factory` partition contents become stale but harmless.

## First boot & calibration

On first boot the cube enters face-ID calibration automatically (no panel map yet). Connect to the **MatrixCube** WiFi (open, SSID configurable). Any URL will redirect to the control UI at `http://192.168.4.1/`.

1. In the Calibration card, click **Start face-ID**. Panel 0 lights up white.
2. Click the face button (TOP / BOTTOM / NORTH / SOUTH / EAST / WEST) that matches the physical panel currently lit. Panel selection is *sticky* — assigning a face doesn't advance; press **Next →** when you're ready for the next panel.
3. Switch to **Edge-match preview** (or just watch on the cube) and step through panel rotation + mirror options until every seam's colors match on both sides and the letters read correctly. The mirror toggle is necessary for any panel whose physical mount is a reflection of the face-local axes — left-right color swaps that no rotation can fix.
4. Click **Save calibration**. On next boot the cube will skip calibration and run whatever effect you pick.

"Swap E ↔ W" is a one-shot fixup that swaps the panel map entries for those two faces, in case you realize post-calibration that you identified them backwards.

## Orientation

In the Orientation card, pick how the cube physically sits:

- **Face up** (default) — TOP panel faces up. Effects use TOP face as the "up pole".
- **Corner up** — the cube rests on the BOTTOM+EAST+NORTH corner so the opposite (TOP+WEST+SOUTH) corner points up. Several effects adapt to this automatically:
  - **Matrix**, **Rain**, **Fire**, **Fireworks**, **Pulse**, **Lightning** use a BFS flow field seeded at the up-corner cluster so drops / sparks / bolts emanate from / converge on the correct pole.
  - **Pendulum** runs 6 pendulums (one per face), pivoted at each face's highest corner under corner-up gravity, rest-hanging along the face diagonal.
  - Face-local effects (Rainbow, Plasma, Galaxy, Ping-pong, Spiral, Warp, Life, Breakout) keep running per-face regardless of orientation.

## Effects

Visual effects:

| Name      | Notes |
|-----------|-------|
| Off       | all black |
| Solid     | single configurable color across every pixel |
| Rain      | blue clouds drift on TOP, drops fall down all sides, pool on BOTTOM. Corner-up: cloud at up corner, flow to opposite corner. |
| Fire      | per-side heatmap rising, licks over top edge, smoke swirl over TOP center. Corner-up: heat from the down corner climbs toward the up corner. |
| Rainbow   | per-face radial rainbow, rings cycle outward |
| Radial    | geodesic color wave emanating from TOP center, sides, converging on BOTTOM |
| Balls     | bouncing balls with 3D-ish physics and BOTTOM-edge restitution |
| Chase     | 6 rainbow "snakes" on 3 great-circle paths (equator + 2 meridians), each pair of same-kind paths drifts independently between laps |
| Plasma    | sinusoidal plasma field |
| Ping-pong | 6 independent 2D bouncing balls, one per face, with paddle wall-flashes |
| Fireworks | rockets launch from BOTTOM, detonate above TOP, sparks spill onto TOP. Corner-up: launch from down corner, detonate near up corner. |
| Matrix    | green rain in Matrix-movie style, orientation-aware flow |
| Galaxy    | per-face rotating log-spiral with differential rotation + per-face hues |
| Spiral    | per-face outer→inner rectangular spiral with rainbow tail |
| Ripple    | 3D drops create expanding wavefronts that cross face seams and interfere additively |
| Warp      | per-face warp-speed starfield with exponential acceleration |
| Aurora    | smooth green/cyan/violet bands flowing over TOP, spilling onto upper rows of sides |
| Lightning | dim cloud cover with jagged bolts striking down. Corner-up: cloud at up corner, bolts walk BFS flow to down corner. |
| Breakout  | 6 independent self-playing Breakout games, one per face |
| Pulse     | concentric Gaussian shockwaves emanate from the up pole |
| Tetris    | 4 independent self-playing Tetris boards on the side faces |
| Pendulum  | damped 2D pendulums per face. Face-up: 4 on side faces. Corner-up: 6, one per face, pivoting at each face's high corner. |
| Supernova | stellar core at cube center breathes and heats up, erupts in a spherical shockwave, then falls to a dim ember and cycles. Rainbow-speed knob sets eruption cadence. |
| Ocean     | volumetric sea filling the lower half of the cube: rolling turquoise waves build up into a storm, crests break into white foam caps, then calm back to glass. Side faces show the cross-section; TOP shows the surface from above; BOTTOM is the dark seabed. Rainbow-speed knob controls wave and storm pace. |
| Perlin fire | volumetric 3D Perlin-noise flames fill the entire cube, rising from the floor with turbulent advection. Every face reads a consistent flame body — hot yellow-white rising from the bottom, cooling through orange and red as it reaches the top. |
| Lava lamp | warm metaballs float inside the cube, deforming into organic blobs when they touch. Each blob heats at the bottom and buoys upward, cools at the top and sinks, then reheats — the classic convection loop of a real lava lamp. Deep red when cool, orange/yellow when hot. |
| Rubik     | all six faces display a 3x3 sticker grid in the standard Rubik's color scheme. Every ~2 seconds one of U / D / E (top layer, bottom layer, equatorial middle slice) fires in a random direction — affected stickers dim briefly, snap to their new positions, and brighten back to full color. |
| Tree      | an L-system tree sprouts from the cube floor, grows a trunk and two layers of branches, blooms a green canopy, tints through autumn yellow/orange/red, sheds its leaves as they flutter down under gravity, then reseeds with a new tree shape. Rainbow-speed knob controls how fast seasons pass. |
| DNA       | a double-helix DNA strand fills the cube vertically. Two lavender-white phosphate backbones twist around the Y axis; colored rungs (A-T red/blue, C-G yellow/green base pairs) connect them at regular intervals. The whole helix rotates slowly so every side face sees the strands cross in and out. |
| Black hole| a dark event horizon anchored at cube center, wrapped in a photon ring; a thin rotating accretion disk lies in the equatorial plane — blue-white at the inner edge, cooling to orange outside, with a two-arm trailing spiral and Doppler-brightened approach side. Occasional bright particles spiral in and disappear past the horizon. |

Calibration modes (`face_id`, `edge_match`, `face_test`) are available but not included in the random cycle pool.

## Startup behavior

The Startup card lets you pick what runs when the cube boots:

- **Last effect** (default) — whatever was running at last shutdown.
- **Specific effect** — force a given effect ID on boot.
- **Random cycle** — rotate through visual effects every *N* seconds (5–300, configurable).

Flipping random mode also exposes a **Cycle randomly** toggle on the main effect picker for on-demand cycling.

## Code layout

```
main/
├─ main.c                app_main: config → render → effects → net → http
├─ hardware.h            board-level pin config (CUBE_DATA_GPIO)
├─ cube.[ch]             cube topology — face adjacency, flip tables, coord→strip mapping
├─ orient.[ch]           orientation (face-up / corner-up) + shared BFS flow field
├─ config.[ch]           app config (NVS-persisted) + enums
├─ render.[ch]           RMT LED driver + framebuffer + current cap
├─ effects.[ch]          60 Hz effect task, dispatch, random cycler
├─ effect_*.c            one file per effect
├─ net.[ch]              SoftAP bring-up
├─ dns_captive.[ch]      DNS server that redirects all queries to the AP IP
├─ http.[ch]             HTTP server + JSON API + embedded web assets
└─ web/                  index.html + app.js + style.css (served from flash)
```

## API

Relevant endpoints exposed by the HTTP server:

| Method & path            | Body                                                   | Effect |
|--------------------------|--------------------------------------------------------|--------|
| `GET  /api/state`        | —                                                      | full state snapshot |
| `POST /api/effect`       | `{"name": "..."}`                                      | switch active effect |
| `POST /api/brightness`   | `{"value": 0..255}`                                    | global brightness |
| `POST /api/solid`        | `{"r","g","b"}`                                        | solid-effect color |
| `POST /api/calib/step`   | `{"panel": 0..5}`                                      | select panel for calibration |
| `POST /api/calib/face`   | `{"panel": 0..5, "face": "T"|"B"|...}`                 | assign face to panel |
| `POST /api/calib/rot`    | `{"panel": 0..5, "rot": 0..3}`                         | set panel rotation (×90°) |
| `POST /api/calib/mirror` | `{"panel": 0..5, "mirror": 0|1}`                       | set panel horizontal flip |
| `POST /api/calib/swap_ew`| —                                                      | swap FACE_EAST ↔ FACE_WEST panel map |
| `POST /api/calib/done`   | —                                                      | mark calibration complete |
| `POST /api/calib/reset`  | —                                                      | wipe calibration |
| `POST /api/startup`      | `{"mode": "last"|"random"|"specific", "effect": "...", "interval_s": N}` | startup behavior |
| `POST /api/random`       | `{"on": true|false}`                                   | runtime random-cycle toggle |
| `POST /api/orientation`  | `{"mode": "face_up"|"corner_up"}`                      | physical orientation |
| `POST /api/ota`          | *raw* firmware `.bin` as body                          | stream OTA image; cube reboots on success |

All responses are JSON. Captive-portal probes (`/generate_204`, `/hotspot-detect.html`, etc.) 302 to `/` so connecting phones auto-pop the control UI.
