# TODO / Roadmap — DualLuma VF

Status of known gaps after the v1.0 documentation review. Checked items are implemented in the current tree.

## Done

- [x] UDP luma link: 128×64 gray8, 6 fragments, 18.5–20.5 fps
- [x] tjpgd Y-only decode on the camera (encode 11–12 ms)
- [x] UDP reassembly with `frame_id` + 64-bit mask, latest-frame mailbox
- [x] Adaptive send interval from viewer feedback (15–20 fps target)
- [x] JPEG frame buffer fix (3,840 B → 32 KB) for busy scenes
- [x] Auto flash: AEC + panel-average fallback, hysteresis and 3 s dwell
- [x] Hidden SoftAP by default
- [x] Viewer photo freeze / long-press resume (logic in place)
- [x] English README + Chinese technical design doc

## P0 — before calling it a product

- [ ] **Confirm the viewer button GPIO.** `APP_BUTTON_GPIO=0` produces no events on the current board. Identify the BOOT pin (0/9/28?) or wire a button to a free pin, then update Kconfig.
- [ ] **Camera-side shutter.** Use a free camera GPIO (e.g. GPIO13) and a small control packet, so the shutter is on the camera like the reference DIY-Camera.
- [ ] **SD card photo capture.** The camera already has a microSD slot. Use SDMMC **1-bit** (CLK 14, CMD 15, D0 2) to keep the flash LED on GPIO4. Save the full JPEG to `IMG_%04u.JPG`, keep the counter in NVS, and show `SAVED IMG_0007` on the viewer.
- [ ] **Long-run validation.** ≥24 h soak, complex scenes, temperature and range; confirm no `CAMERA LOST` after the 32 KB buffer fix.
- [ ] **Power/thermal measurement.** Current draw, regulator temperature, Wi-Fi duty cycle.

## P1 — robustness and usability

- [ ] Feedback-loss recovery: if no feedback for N seconds, slowly increase the rate or reset the interval (currently frozen).
- [ ] Fragment NACK or FEC for larger frames / worse links.
- [ ] Photo review over HTTP: `/photos` list and `/photos/last.jpg`; the viewer or a phone on the AP can browse.
- [ ] Manual flash override (auto / on / off) via a long press.
- [ ] Camera-side status packet: photo count, save result, SD free space.
- [ ] Battery option: LiPo + TP4056 + ADC monitor + deep sleep / power latch.
- [ ] Protocol version field (use `reserved`) and compatibility rules.

## P2 — engineering hygiene

- [ ] Automated tests / CI (host-side parser tests, protocol round-trip).
- [ ] OTA / dual-partition rollback.
- [ ] Clean up dead code and stale docs; keep one final technical document.
- [ ] Security review: UDP payload authentication, replay protection.
- [ ] Production provisioning and calibration flow.

## Non-goals (evaluated, not adopted)

- RTSP + H.264/H.265: the ESP32/ESP32-C5 have no hardware video encoder/decoder, and the SSD1306 I2C refresh caps the panel at ~30–33 fps.
- YUV422 raw streaming: the OV2640 only reaches ~13.5 fps in YUV422 QQVGA with this driver; JPEG capture is faster.
- Partial OLED refresh: dithered frames change most pages, and an I2C glitch can leave a stale page; full refresh is self-healing.
