# AiO v4.5 tree marker firmware (Teensy 4.1)

Firmware for the AgOpenGPS **AiO v4.5** board (Teensy 4.1, Keya CAN steering) that marks
tree positions while planting. This repo is the AgOpenGPS AiO v4 Keya CANBUS HPR Dual firmware
(27.04.2025, GPL-3.0), with these additions:

| File | What it adds |
|---|---|
| `aio_v4_tree/zzTree.ino` | Tree marker. Trees and settings arrive over UDP 8890. The fire is predicted from the F9P GGA, and the spray valve is driven through the onboard Cytron (pins 2/4). It is a dry run until the host sets "own Cytron". |
| `aio_v4_tree/zzzOta.ino` + `FlashTxx.c/.h` | Ethernet OTA: `http://<board>/` serves a page that uploads a `.hex`, built on [FlasherX](https://github.com/joepasquariello/FlasherX). |
| `aio_v4_tree/zEthernet.ino` | Fixes a stock bug where the DHCP client could clear the static IP. |
| `tools/tree_desk_test.py` | Desk test that drives a simulated row and reports the along-row error. |
| `tools/tree_gps_diag.py` | Shows what the board receives from its two F9Ps. |

## Loading a field (tree positions)

On the board's page (`http://192.168.5.126/`), in **Field**, pick the CSV and the MGA zone, check the preview (tree
count, block size, tree spacing, distance from the tractor), then press **Send to AiO**. The CSV rules are the same as
for the ESP32 marker: a header row with `X`/`Easting` and `Y`/`Northing` in MGA metres (other columns are ignored),
or `Lat`/`Lon` in degrees, with up to 20000 trees. The browser converts the coordinates; the board stores the trees in a
reserved 256 KB of flash at 0x60780000, so they survive power-off and firmware updates. Autosteer must be off.
`python tools/test_field_csv.py` checks the conversion against an independent projection (it runs in CI).

## Updating the board (no USB needed)

**Easiest, no browser:** download
[update_aio.bat](https://github.com/mandeepmildura/sandy-soils-aio-tree-marker/releases/latest/download/update_aio.bat)
on the Surface and double-click it. It checks the AiO answers, downloads the newest build and sends it with Windows'
built-in `curl`. If Windows says it protected your PC, choose **More info**, then **Run anyway**.

Or in the browser:

1. Open `http://192.168.5.126/` on the Surface (or any PC on the AgIO network).
2. Autosteer must be off. Press **Update to latest** (v1.1.0 and later). The browser on the Surface needs internet, for
   example the phone hotspot: it downloads the newest build and sends it to the board. Without internet, download
   `aio_v4_tree.hex` from the [releases](https://github.com/mandeepmildura/sandy-soils-aio-tree-marker/releases)
   while online, then pick it on the same page and press **Upload**.
3. The board checks the file (checksums, flash range, Teensy 4.1 OTA build) before writing it, and keeps the
   old firmware if anything is wrong. After "OK - verified" it reboots and is back in about 10 s.

The OTA buffer never touches the top 256 KB of flash, so the AgOpenGPS EEPROM settings survive updates.
The stock firmware has no OTA, so going back to it needs USB and Teensy Loader.

## Watching the AiO live from Mandeep's PC

The Surface has only a phone-hotspot link to the cab and no inbound ports.
`relay_aio.bat` runs on the Surface and forwards `/status` to HiveMQ Cloud over
TLS; a second script on Mandeep's PC subscribes and prints a live line.

**On the Surface** (double-click, nothing to install): download
[relay_aio.bat](https://github.com/mandeepmildura/sandy-soils-aio-tree-marker/releases/latest/download/relay_aio.bat)
and [relay_aio.ps1](https://github.com/mandeepmildura/sandy-soils-aio-tree-marker/releases/latest/download/relay_aio.ps1)
into the same folder, double-click the .bat. First run asks for the HiveMQ
host, user and password; they are saved per-user with Windows DPAPI under
`%APPDATA%\sandy-soils-aio\`. After that it runs quietly, one request per
second to the AiO (no parallel or idle sockets — the board hangs on those),
publishing JSON to `treemarker/aio/status` (retained) and one message to
`treemarker/aio/mark` every time `hits` or `fires` rises.

**On Mandeep's PC**: `tools/watch_aio.py` with env vars:

```
set AIO_MQTT_HOST=<your HiveMQ cluster>.s2.eu.hivemq.cloud
set AIO_MQTT_USER=<the PC's read-only user>
set AIO_MQTT_PASS=<password>
python tools/watch_aio.py
```

Uses `paho-mqtt` when installed; otherwise a stdlib-only fallback.

## Logging and live view (relay on the Surface)

Download `relay_aio.bat` **and** `relay_aio.ps1` from the latest release into the same folder on the Surface and
double-click the .bat; keep its window open while marking. On first run, press Enter at the HiveMQ question to run
without MQTT. It then:

- reads the AiO once a second (the only client the AiO has),
- logs every reading to `Documents\AiO logsio-log-<date>.csv` and every mark to `aio-marks-<date>.csv`,
- shares the latest reading on port 8126 of every network the Surface is on (e.g. the phone hotspot):
  `http://<surface-ip>:8126/status`, `/log`, `/marks`. The window prints the addresses. Windows asks once to allow
  the firewall: choose **Allow**,
- optionally publishes to HiveMQ (`treemarker/aio/status`, `treemarker/aio/mark`) for `tools/watch_aio.py`.

`python tools/test_relay_aio.py` tests all of it against a stand-in AiO and broker.

## Releasing

Bump `AIO_TREE_VERSION` in `aio_v4_tree/zzzOta.ino`, commit, then `git tag vX.Y.Z && git push origin vX.Y.Z`.
CI builds with `teensy:avr@1.59.0`, checks the hex is safe for OTA and attaches `aio_v4_tree.hex` to the release, then pushes it plus `version.txt` to the `ota` branch, which the board's button reads (raw.githubusercontent.com allows browser downloads; release files don't).

## Licence

GPL-3.0, the same as the AgOpenGPS firmware it is based on (`LICENSE`). `FlashTxx.c/.h` are public domain (FlasherX).
