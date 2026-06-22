# 江東區居民的相框 — e-paper photo frame firmware

ESP32-C6 firmware for a WiFi e-paper photo frame. On a timer it wakes from deep
sleep, pulls the current photo from hsuyuting.com over HTTPS, draws it on the
e-paper panel, and sleeps again. First boot runs a provisioning WiFi AP to set
credentials (stored in NVS — never in the repo). Photos are uploaded from the web
side at <https://hsuyuting.com/build/1>.

## Hardware

- Seeed Studio XIAO ESP32-C6 (4 MB flash)
- An e-paper display over SPI — driver in `main/epd.c`, pins in `main/epd.h`
- Wiring and the build story: `guide.html`

## Build & flash

Needs **ESP-IDF v6.0.1** (the version it's built against — match it; ESP-IDF is
version-sensitive). Install per Espressif's getting-started guide, then:

```sh
. $IDF_PATH/export.sh                         # load ESP-IDF into this shell
idf.py set-target esp32c6                      # REQUIRED on a fresh clone: sdkconfig isn't committed
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor      # your XIAO's serial port
```

`sdkconfig.defaults` holds the committed config (4 MB flash, large app partition,
mbedTLS CA bundle for HTTPS); `idf.py` regenerates `sdkconfig` from it on first
build. Change settings with `idf.py menuconfig`. No external components — only
built-in ESP-IDF ones (see `main/CMakeLists.txt`), so nothing extra is fetched.

## Layout

- `main/main.c` — WiFi provisioning + connect, HTTPS image fetch, deep-sleep schedule
- `main/epd.c` · `epd.h` · `epd_text.c` — e-paper driver and text rendering
- `tools/convert.py` — host-side: convert a photo into the e-paper image format
- `CMakeLists.txt` — the IDF project (named `1`, so build artifacts are `1.elf` / `1.bin`)
- `guide.html` — making-of record

Everything else regenerates and is gitignored — `build/`, the ESP-IDF SDK itself,
`sdkconfig`. This README plus `idf.py build` rebuilds all of it.
