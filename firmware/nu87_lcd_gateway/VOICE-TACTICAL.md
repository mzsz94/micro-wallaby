# NU87 Wallaby / NU40 control tower voice extension

This branch adds optional services to the existing KIT display connection:

- **NU40 control tower:** PDM microphone, BTN4 warning input, MAX98357A speaker output and OLED.
- **NU87 Wallaby:** explicit BLE peer selection, audio relay, simulated robot command state and warning interlock.
- **HostPC:** local speech recognition, local AI/manual processing, records and motion visualization over Wi-Fi.

The original KIT display firmware remains compatible. Missing audio, alarm or authenticated SMP services report unavailable; discovering them never substitutes an unrelated BLE peer. These extensions require the corresponding `firmware/nu40_voice` application. The host adapter is `voice_tactical/gateway.py`.

## Authenticated HTTP interface

All routes use the existing bearer token. Host tooling accepts only numeric private/loopback IP addresses, ignores HTTP proxy settings and never follows redirects. Credentials come from `NU87_GATEWAY_TOKEN` or the existing private Kconfig file. No route below drives physical motors.

| Route | Request / result |
| --- | --- |
| `GET /v1/audio/status` | Audio service availability, running request state, session, received/dropped/queued counts, error, sample rate and codec |
| `POST /v1/audio/start` | Nonzero uint32 session as decimal ASCII; HTTP 202 means control accepted, actual frames confirm capture |
| `POST /v1/audio/stop` | Raw ASCII `stop`; no implicit restart |
| `GET /v1/audio/chunk` | Up to 32 complete 176-byte frames; an empty body means no queued frame |
| `GET /v1/alarm/status` | Availability, `active` (boolean or null when unknown), output ACK, button event counter, generation, source and output status |
| `POST /v1/alarm` | Raw ASCII `on` / `off`; HTTP 200 only after the matching output-completion ACK |
| `POST /v1/robot/command` | `stop`, `dock`, `greet`, `forward`, `back`, `left`, `right`; returns accepted command/state/sequence, alert and `simulated:true` |
| `GET /v1/robot/status` | Current simulated robot state; manual movement/docking expires after 900 ms and greeting after 3 s |
| `GET /v1/smp/status` | Authenticated MCUmgr service availability |
| `POST /v1/smp` | One complete SMP packet (8–1024 bytes), returning the matching complete binary reply |

The existing `/v1/status` retains `capabilities.ota:false` for the incompatible legacy OTA routes and adds `microphone` and `smp` readiness. The Python adapter reports `capabilities.ota:true` only for an available authenticated SMP service, with `ota_protocol:"mcumgr-smp"`.

## Audio framing and loss

KIT service `90e2c000-3c0d-4ef3-9d78-574cd1a64087` adds control `…c003` and data `…c004`. Control writes contain version 1, operation 1 start / 0 stop and a little-endian uint32 session. Data notifications are exactly 176 bytes: `WA`, version 1, flags 0, session LE32, sequence LE32, predictor LE16, IMA ADPCM index (0–88), reserved 0, then 160 ADPCM bytes. Decode low nibble first; the final high nibble is padding zero. Each frame independently yields 320 mono PCM16 samples at 16 kHz (20 ms).

The ATT MTU must be at least 179. NU87 allocates a 64-frame queue and drains at most 32 frames per HTTP request. It rejects invalid frames, old sessions and duplicate/backward sequence numbers; gaps and overwritten frames increment a saturating dropped counter. Disconnect clears queued audio and cancels capture. Reusing the last session ID is rejected. Start acceptance alone does not prove that the PDM driver is delivering samples.

## BTN4 and warning state

Alarm control `…c005` is `[1, on/off, sequence LE32]`. ACK/event `…c006` is `[1, active, sequence LE32, output-status, source]`; status 0 means applied, 1 unsupported, 2 driver failure. Source 0 is a command ACK. Source 1 with sequence 0 is an unsolicited BTN4 event.

BTN4 first activates the NU40 speaker locally, so HostPC loss cannot delay the local warning. Receiving a valid button event latches the NU87 robot warning and stops simulated motion even when speaker output reports an error. Other motion commands are rejected while latched. Only a successful matching alarm-off ACK clears the NU87 interlock. No command is replayed after disconnection.

`eventCounter` increments for button events only and survives BLE reconnects during the same NU87 boot. `generation` increments for each new alarm-link setup. Host consumers compare the event counter even if the latest `source` has since changed to `command`; a NU87 reboot resets counters. An unknown/disconnected output is represented by `active:null`, never an assumed silent speaker.

## SMP pairing and update preparation

MCUmgr uses service `8D53DC1D-1DB7-4CD3-868B-8A527460AA84` and characteristic `DA2E7828-FBCE-4E01-AE9E-261174997C48`. The relay requires BLE security level L3 and the private six-digit passkey shared with the NU40 build. An empty passkey disables the SMP relay; display and audio remain usable. Passkeys are never logged or supplied as command-line values.

Use the NU40 build's private pairing-code file (mode 0600):

```sh
bash firmware/nu87_lcd_gateway/build.sh --pairing-code-file build/private/ble-passkey.txt
```

`local.conf` must select `CONFIG_GATEWAY_NU40_KIT=y`. The helper generates a mode-0600 build-only Kconfig fragment. For an already configured external Zephyr workspace, `pairing_config.py --pairing-code-file PATH --output BUILD/private-pairing.conf` creates the fragment for `EXTRA_CONF_FILE` without printing its contents. Use the same private file for both boards.

The relay stops active microphone capture before an SMP transaction. It fragments requests at MTU minus 3, checks reply operation/group/sequence/id and lengths, and disconnects after an ambiguous error/timeout to prevent stale reply reuse. Write-without-response allocation runs on the system workqueue so exhausted ATT buffers cannot block the HTTP thread indefinitely. It never selects an image, tests it, resets a device or confirms boot automatically. The host updater performs those explicit steps against the MCUboot-enabled NU40 application. Existing legacy KIT firmware does not acquire OTA support simply by updating the NU87.

The existing AmebaD setup still uses a test random generator; this remains an isolated development setup, not a validated production cryptographic platform.

## Verification

```sh
sh firmware/nu87_lcd_gateway/tests/run_kit_tests.sh
python3 -m unittest discover -s firmware/nu87_lcd_gateway/tests -p 'test_*.py'
python3 -m unittest voice_tactical.tests.test_gateway
```

Host tests exercise production callback paths and HTTP handlers with ASan/UBSan, including ADPCM queue bounds, stale sessions, SMP fragmentation/correlation/security rejection, output ACK failures, button events, motion timeouts and alert interlocks. Successful firmware builds and host tests do not establish microphone audio quality, speaker audibility, radio throughput or end-to-end OTA behavior on the connected hardware.
