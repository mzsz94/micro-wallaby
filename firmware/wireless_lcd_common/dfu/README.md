# NU40 application-only legacy BLE DFU client

This is a portable C11 client for the Nordic legacy DFU service implemented by
Adafruit nRF52 bootloader **0.9.1**, targeting the NU40's existing **nRF52840 / S140
6.1.1** layout. It has host-tested wire behavior, not hardware OTA validation.
It does not scan, pair, enter the bootloader, download images, or confirm the
health of the restarted application; the NU87 adapter owns those operations.

The client accepts only a raw application binary linked at `0x26000`, with an
exclusive end at or before `0xDC000`, and a length divisible by four. Metadata
must specify SoftDevice FWID `0x00B6`. Its API has no SoftDevice or bootloader
update mode. It emits application type `0x04` and zero sizes for both protected
components. HEX, UF2 and ZIP containers must be rejected/converted upstream,
never streamed directly to this API. The source must remain immutable through
preflight and transfer.

## Integration contract

1. Initialize a fresh session with `legacy_dfu_begin`. Drain all events from the
   previous connection/session before reusing its storage. `poll` first reads
   and checks the whole image with bounded 20-byte reads, including CRC16 and
   plausible nRF52840 stack/reset vectors. No BLE write is possible yet.
2. Wait until state `LEGACY_DFU_START`. The adapter may keep the old application
   connected throughout this preflight, then request bootloader entry. Verify
   the intended peer's identity, the legacy service and characteristic UUIDs
   in the header, revision characteristic `0x0008`, and a successful control
   CCCD subscription. A matching advertisement name alone is insufficient.
3. Call `legacy_dfu_transport_ready` only after those checks. Continue calling
   `poll` using monotonic `uint32_t` milliseconds; counter wrap is supported.
   The session deadline includes preflight and discovery. Waiting for readiness
   does not consume a control-response timeout.
4. Implement `write` with a GATT **Write Request** for control and **Write
   Command** for packet data, at most 20 bytes even when the negotiated MTU is
   larger. `ACCEPTED` requires the adapter to copy/queue the bytes and eventually
   deliver exactly one `write_complete`; control completion means its ATT
   response, packet completion means local TX completion. It does not mean that
   flash validation succeeded. Only one write may be outstanding.
5. Serialize all calls and callbacks on one task. Do not call client functions
   reentrantly from `read`, `write`, or `close`. Deliver control notifications
   and write completions in their observed order. One notification can arrive
   before completion and is buffered. Feed only this session's DFU events.
   `close` must discard queued work and disconnect, without sending a new reset
   or activation command.
6. After validation, the client sends Activate/Reset. A disconnect after its
   acceptance produces `LEGACY_DFU_REBOOT_OBSERVED`; this can also be link loss
   and **is not application-health confirmation**. The adapter must reconnect
   to the expected application identity/version and check health before
   reporting a successful update.

Calls to `poll` do at most one 20-byte preflight read or one write. There is no
heap allocation, dynamic packet length, blocking loop, implicit reconnect, or
flash-driver dependency. Progress is available from `checked_bytes`,
`sent_bytes`, and `acknowledged_bytes`. PRN byte counts report target receipt;
only its final Receive/Validate responses confirm the subsequent stages.

## Wire sequence

All multi-byte fields below are little endian. The init data uses the legacy
CRC layout despite the service revision being `0x0008`; that characteristic
must not be interpreted as nrfutil's signed `--dfu-ver 0.8` package format.

| Step | Characteristic / bytes | Expected target notification |
| --- | --- | --- |
| Start | Control `01 04`, then Packet `[u32 0, u32 0, u32 image_size]` | `10 01 01` after erase |
| Init | Control `02 00`, Packet `[u16 0052, u16 FFFF, u32 app_version, u16 1, u16 00B6, u16 crc16]`, Control `02 01` | `10 02 01` |
| Receipt interval | Control `08 [u16 packets]` | No successful protocol response |
| Receive | Control `03`, then word-aligned Packet chunks up to 20 bytes | Every configured nonfinal window: `11 [u32 received_bytes]` |
| Final packet | Last 4–20 bytes | `10 03 01`, **not a final PRN** |
| Validate | Control `04` | `10 04 01` |
| Activate/reset | Control `05` | No DFU response; expect disconnect |

CRC is CRC-16/CCITT-FALSE (polynomial `0x1021`, initial `0xFFFF`, no reflection,
no final XOR). The complete raw binary including any upstream padding is
covered. Preflight must match the supplied CRC before Start; a second rolling
CRC over sent bytes catches source mutation before Validate/Activate. Remote
responses 2–6 are retained as `remote_opcode` / `remote_status`; malformed,
unsolicited, duplicate or out-of-order responses stop the session. PRN counts
must exactly equal the current sent boundary.

Default receipt interval is one packet; configuration permits 1–4. The client
stops at each window. There is no retransmission of an accepted command or
firmware packet because uncertain delivery is not safely replayable in this
protocol. Only an `IO_BUSY` return that guarantees **nothing was queued** is
retried (16 retries with a 20 ms minimum delay by default). Start/erase has a
60 s timeout, other stages 10 s, and the complete session 30 minutes. These are
conservative initial policy choices to measure on hardware. A timeout,
transport error or unexpected disconnect closes the session. Recovery is a
fresh, explicitly managed bootloader session with full preflight and upload.
Cancellation stops and closes an active session; once Activate is accepted,
`cancel` returns false because activation cannot be undone.

## Preservation and recovery limits

The inspected bootloader's OTA path in `dfu_single_bank.c` calls
`pstorage_clear(..., image_size)` at the application start, then writes the
received image there. `pstorage_raw.c` erases enough 4096-byte pages to cover
that size. Thus for this client's maximum size `0xB6000`, the source predicts
an erase interval ending at `0xDC000` (exclusive), preserving the old IMU NVS
range `0xDC000..0xE0000` during the transfer. Its shorter images erase only the
pages needed for the new image.

The stock nRF52840 build reserves `10*4096` bytes below bootloader start
`0xF4000`, beginning at `0xEA000`. That stock reservation **does not protect the
old IMU NVS range**; this client's stricter application limit provides that
boundary. These are source-derived expectations, not measurements of the
installed NU40 bootloader. Physical flash contents, custom bootloader changes,
application NVS schema/migration, and retention across a complete update still
need hardware verification. The new application's own flash writes are outside
this transport's control.

The bootloader erases the current application as soon as Start's size packet
is processed, before init validation. It is single-bank in the inspected
release: there is no automatic rollback guarantee after power loss,
cancellation, a failed CRC, or application startup failure. Do not advertise
an update as atomic or safe to interrupt. Preserve a tested USB/recovery path
before deployment.

CRC16 provides error detection, not a signature, authentication, or resistance
to a malicious image. The legacy bootloader does not enforce `app_version`
anti-downgrade checks. Image authenticity, HTTP source integrity, target
selection, authorization to erase, version policy, NVS compatibility and
post-reboot health checks remain adapter/product responsibilities.

## Sources and verification

Protocol behavior was checked against official Adafruit repositories, without
copying their implementation into this client:

- Bootloader tag `0.9.1`, commit `29ca3ebfab262690c125329d94fcb77e5d0f5507`:
  [BLE characteristic/opcode handling](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.9.1/lib/sdk11/components/ble/ble_services/ble_dfu/ble_dfu.c),
  [BLE transport / PRN / final packet behavior](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.9.1/lib/sdk11/components/libraries/bootloader_dfu/dfu_transport_ble.c),
  [init prevalidation and CRC](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.9.1/src/dfu_init.c),
  [single-bank erase/write](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.9.1/lib/sdk11/components/libraries/bootloader_dfu/dfu_single_bank.c),
  [page erase completion](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.9.1/lib/sdk11/components/drivers_nrf/pstorage/pstorage_raw.c),
  [memory limits](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.9.1/lib/sdk11/components/libraries/bootloader_dfu/dfu_types.h),
  [build reservation](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.9.1/Makefile).
- The bundled [S140 6.1.1 HEX](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.9.1/lib/softdevice/s140_nrf52_6.1.1/s140_nrf52_6.1.1_softdevice.hex)
  has FWID bytes `B6 00` at `0x300C`, located using `SD_FWID_OFFSET` in its
  `nrf_sdm.h` (`MBR 0x1000 + info 0x2000 + 0x0C`).
- Adafruit nrfutil commit `7fdfe15feee5f304fb7d9b031721dcefa1f72b58`:
  [BLE sequence](https://github.com/adafruit/Adafruit_nRF52_nrfutil/blob/7fdfe15feee5f304fb7d9b031721dcefa1f72b58/nordicsemi/dfu/dfu_transport_ble.py),
  [init packet serialization](https://github.com/adafruit/Adafruit_nRF52_nrfutil/blob/7fdfe15feee5f304fb7d9b031721dcefa1f72b58/nordicsemi/dfu/init_packet.py),
  [CRC16](https://github.com/adafruit/Adafruit_nRF52_nrfutil/blob/7fdfe15feee5f304fb7d9b031721dcefa1f72b58/nordicsemi/dfu/crc16.py).
  Running those Python serializer/CRC modules independently produced the
  64-byte fixture CRC `0xEE75` and init bytes `52 00 FF FF 04 03 02 01 01 00 B6
  00 75 EE` asserted by the C host tests.

Run from the repository root:

```sh
firmware/wireless_lcd_common/dfu/tests/run_tests.sh
```

Tests compile with strict C11 warnings, AddressSanitizer and UndefinedBehaviorSanitizer.
They exercise literal protocol packets, PRN windows 1–4 and final-window rules,
notification/completion ordering, all remote error statuses, unexpected data,
wrong receipts, CRC/vector/size rejection, mutated/read-failing sources,
bounded busy retries, timeout and clock wrap, cancellation, preflight before
connection, disconnect handling, and protection against activation after failed
validation. No test connects to a board or writes device flash.
