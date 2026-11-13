# Current gaps and roadmap — ESP32-AirCamDuo

Status legend: **Done** / **P0** (needed before an alpha release) / **P1** (robustness and usability) / **P2** (engineering hygiene).

## Decisions (locked for v1.x)

- Keys: `KEY_FREEZE=GPIO3`, `KEY_SHUTTER=GPIO0`, `KEY_PREVIEW=GPIO1` (active low, pull-up, 30 ms debounce).
- Shutter: capture the current frame directly and save the full JPEG to SD; the viewer stays on that frame and shows `SAVED`; `KEY_FREEZE` returns to video.
- Preview: last N JPEGs from SD, N=10 default (`APP_PREVIEW_COUNT`); `KEY_PREVIEW` cycles; long `KEY_FREEZE` exits.
- License: GPL-2.0 (see `LICENSE`); Apache-2.0 component compatibility to review.
- Note: GPIO0 is the BOOT strapping pin; holding it at reset enters download mode.

## Done

- [x] UDP gray8 link: 128×64, 6 fragments, 18.5–20.5 fps measured
- [x] tjpgd Y-only decode on the camera (encode 11–12 ms)
- [x] UDP reassembly with `frame_id` + receiver-side 64-bit fragment mask
- [x] Adaptive send interval from viewer feedback (product target 15–20 fps, deadband 18–20 fps, acceptance ≥18 fps)
- [x] Dual-core pinning: camera encode/send on core 1, feedback/Wi-Fi/HTTP on core 0; measured 20.2–20.4 fps
- [x] Hardened partial OLED refresh behind `APP_VIEW_PARTIAL`; measured 29.5 fps vs 32.2 fps full with dithering, so full refresh stays default
- [x] JPEG frame buffer fix (3,840 B → 32 KB) for busy scenes
- [x] Auto flash: AEC + panel-average fallback, hysteresis and 3 s dwell
- [x] Hidden SoftAP by default
- [x] Single-key photo-freeze logic (hardware unverified)
- [x] English README + English technical design document (`docs/TECHNICAL_DESIGN.md`)
- [x] Design decisions locked: keys GPIO3/0/1, shutter semantics, preview source, GPL-2.0

## P0 — alpha release

| Item | What | Acceptance | Dependency | Affected docs |
|---|---|---|---|---|
| C5 multi-button controls | Implement the decided mapping: `KEY_FREEZE=GPIO3`, `KEY_SHUTTER=GPIO0`, `KEY_PREVIEW=GPIO1` (active low, 30 ms debounce). Kconfig: `APP_BUTTON_FREEZE_GPIO=3`, `APP_BUTTON_SHUTTER_GPIO=0`, `APP_BUTTON_PREVIEW_GPIO=1`. **GPIO0 is the BOOT strap**; verify wiring. | Each key produces independent events; no double-click gestures; long `KEY_FREEZE` only forces video | Confirm wiring and the BOOT pin behaviour | Technical doc 5.2.6 / 7.2 / 7.5.2 / 7.7 / 11.3; README features |
| Viewer button wiring | Mapping decided (3/0/1); verify each physical key and that GPIO0 does not hold low at reset. Current firmware still uses a single key on GPIO0 and produces no events. | Press/release/long-press logs appear per key; freeze/shutter/preview work | Board schematic or multimeter | README limits; technical doc 5.2.6 |
| Shutter semantics | **Decided**: capture the current frame directly, save the full JPEG to SD; the viewer stays on that frame and shows `SAVED`; `KEY_FREEZE` returns to video. Implement the state machine and SAVED indication. | Current frame saved as a valid JPEG; viewer stays; SAVED visible; defined return path | P0 multi-button + SD | Technical doc 7.5.2 / 10.6 |
| SD JPEG capture | SDMMC **1-bit** (CLK14/CMD15/D0 2, keeping GPIO4 flash); write the current frame as `IMG_%04u.JPG`; NVS counter; increment only after a successful write; dedicated low-priority photo task with a depth-1 queue; keep the viewer on the captured frame | Files readable on PC; counter survives reboot; writes do not stall UDP/display; viewer stays on the saved frame | SD hardware verification, FAT32 card | Technical doc 10.6; README features/config |
| Image preview switching | **Decided**: preview the last N (default 10) JPEGs from SD; `KEY_PREVIEW` cycles; long `KEY_FREEZE` exits. Data source is the SD photo list; the CAM reads/decodes and sends 128×64 over UDP (no HTTP client on the C5 required). | Switch within the defined time; no corruption; return to LIVE | SD capture, `APP_PREVIEW_COUNT` | Technical doc 7.5.2 / 10.6; README planned |
| Long-run validation | ≥24 h soak, complex scenes, temperature and range; confirm no `CAMERA LOST` after the 32 KB fix | No reboot/leak; fps and heap stable | Two boards + bench | Technical doc 10.4 |

## P1 — robustness and usability

| Item | What | Acceptance | Dependency | Affected docs |
|---|---|---|---|---|
| Feedback-loss recovery | If no feedback for N seconds, gradually raise the rate or reset the interval | Recovers within N seconds; no runaway | — | Technical doc 8.5.3 |
| Fragment NACK / FEC | Recover a missing fragment for larger frames or worse links | Lower drop rate at fixed fps | Protocol extension | Technical doc 8.8 |
| Photo review over HTTP | `/photos` list and `/photos/last.jpg` on the camera | Phone on the AP can browse; viewer can fetch a thumbnail | SD capture | Technical doc 10.6 |
| Manual flash override | Auto / always-on / always-off via long press | Mode visible and persistent | Button plan | Technical doc 7.4.4 |
| Camera status packet | Photo count, save result, SD free space to the viewer | Viewer shows `SAVED IMG_0007` / count | SD capture, protocol | Technical doc 7.1 / 10.6 |
| Battery option | LiPo + TP4056 + ADC monitor + deep sleep / power latch | Portable runtime measured; low-battery warning | Hardware | README hardware |
| Protocol versioning | Use `reserved` or a version field; compatibility rules | Old firmware ignores new fields without crashing | — | Technical doc 7.1 / 9.8 |

## P2 — engineering hygiene

| Item | What | Acceptance | Dependency | Affected docs |
|---|---|---|---|---|
| Tests / CI | Host-side protocol parser tests; UDP round-trip; build matrix | CI green on push | — | README / technical doc |
| OTA / dual partition | OTA image, A/B rollback | Upgrade and rollback verified | Partition table | Technical doc 10.5 |
| Dead-code / doc cleanup | One final technical doc; remove stale drafts | Repo has one source of truth | — | docs/ |
| Security review | UDP payload authentication, replay protection | Threat model documented | Protocol change | Technical doc 10.5 |
| Production provisioning | Serial/SoftAP provisioning and calibration | New board joins without recompiling | — | README |

## Non-goals (evaluated, not adopted)

- **RTSP + H.264/H.265**: the ESP32/ESP32-C5 have no hardware video encoder/decoder, and the SSD1306 I2C refresh caps the panel at ~30–33 fps.
- **YUV422 raw streaming**: the OV2640 only reaches ~13.5 fps in YUV422 QQVGA with this driver; JPEG capture is faster.
- **Partial OLED refresh**: dithered frames change most pages, and an I2C glitch can leave a stale page; full refresh is self-healing.
- **Camera-side shutter on GPIO13**: an alternative only; the primary plan is C5-side multi-button control.
- **XCLK 40 MHz overclock**: not used; at QQVGA the sensor is not the bottleneck, and the OV2640 spec is 27 MHz.
