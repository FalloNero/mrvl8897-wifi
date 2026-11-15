# Technical notes

Findings behind the design, for anyone working on the driver. Tested on a Dell Wyse 3040 thin
client (Intel Cherry Trail, 8086:2295 SDIO host controller), Windows 11 26200, sdbus.sys 10.0.26100.7019.

## Source layout

| File | Role |
|------|------|
| `src/driver.c` | KMDF entry, PnP/power, boot re-enumeration, crash guard, diagnostics in the registry |
| `src/sdio.c` | sdbus interface: attach, CMD52/CMD53, block size, bus settings, card interrupt |
| `src/card.c` | mwifiex port: firmware download, host commands, scan, TX (TxPD) / RX (RxPD), events |
| `src/wifi.cpp` | WiFiCx/NetAdapterCx client: capabilities, WDI command handling, data path |
| `src/log.c` | in-memory log ring, flushed to `Parameters\Log` once a second |
| `src/mrvl8897.inf` | Net-class INF (Native 802.11), firmware copy, boot trace AutoLogger |

## Windows SD bus driver (sdbus) behaviour

### The OCR mismatch

`SdbusInitializeCardWorker` has two compare paths for the card's identity:

- **Detection** (work packet 0x0D, used at enumeration): if card type / OCR differ from what was
  saved (or the saved info was cleared), the new values are saved. No failure.
- **Function start** (packet 0x0C, INITIALIZE_CARD, used when the function driver's START reaches
  sdbus): strict compare of the saved values (`socket+1180h` = OCR) with the fresh CMD5 response;
  a mismatch fails the start with `0xC0000001` ("different card").

The 88W8897 answers the very first CMD5 after power-on with OCR **0x800000**, every later one with
**0xFF8000**. So after a cold boot the saved OCR never matches and the first START always fails;
the failed re-init also leaves the card unresponsive until the next power-off.

No `OverrideFlags` bit affects that compare. The per-slot bit **0x200** (controller
`Device Parameters\<slot>\OverrideFlags`) only skips a different check in `SdbusSetPowerWorker`
(card OCR AND slot voltage mask must be non-zero), which the stale 0x800000 also failed. It is not
needed once the card has been re-detected (confirmed on the Wyse 3040 with the flag removed). Never change the global sdbus parameters
(`Services\sdbus\Parameters`): the same driver runs the boot eMMC, and `DisableRuntimePower` there
made the machine unbootable.

### Re-enumeration

sdbus implements `GUID_REENUMERATE_SELF_INTERFACE_STANDARD` on the function PDO
(`SdbusPdoGetReenumerationInterface`, Size >= 0x28, Version 1). Its
`SurpriseRemoveAndReenumerateSelf` clears the saved card info (`socket+3Ch = 0`), flags the
socket as card-changed and re-detects the card through the 0x0D path (saving the current OCR),
then re-creates the PDO.

The driver uses it like this:

1. `EvtDeviceAdd` of the first instance in a boot (volatile registry value
   `Parameters\ThisBoot\Reenumerated` not set): no card I/O, just `WdfFdoQueryForInterface`.
2. Its `IRP_MN_START_DEVICE` is caught in a WDM preprocess callback: set the volatile value, call
   the interface, complete the START with `STATUS_DEVICE_NOT_READY` (it never reaches sdbus).
3. The re-created device starts normally; the firmware download runs in `EvtDevicePrepareHardware`.

After the firmware has run, any further sdbus re-init (disable/enable, driver update) fails until
a real power-off; the D0Exit path sends `FUNC_SHUTDOWN` and an SDIO I/O reset, which is not enough.
A normal restart works (the platform power-cycles the card); resume from sleep/hibernate is
expected to fail for the same reason (D0Entry does no re-initialisation).

### Firmware download timing

The download (mwifiex `prog_fw_w_helper`, 16-byte header chunks + up to 2 KB data chunks, polled
via `CARD_IO_READY | DN_LD_CARD_RDY` at register 0x50, length at 0x60/0x61) hangs after the first
16-byte chunk unless sdbus's WPP tracing is enabled. Delays in our own code (5 ms around chunks)
do not help, so the difference is inside sdbus (it logs from its interrupt DPC). The INF keeps an
AutoLogger session on both sdbus WPP providers (`{3B9E3DA4-70B8-46D3-9EF2-3DDF128BDED8}`,
`{9C205A39-1250-487D-ABD7-E831C6290539}`, level 0xFF, all flags). The AutoLogger `FileName` must be
a `REG_SZ` full path; `REG_EXPAND_SZ` with `%SystemRoot%` makes the session fail with status 0x57.

## mwifiex details used

- SD8897 register set: `start_wr_port 0`, `data_port_mask 0xffffffff`, write bitmap at 0x08–0x0b,
  read bitmap 0x04–0x07, read lengths from 0x0c, host int status 0x03, CMD53 "new mode", command
  port `ioport | 0x8000`, data ports `ioport + n`.
- Key API: HW-spec TLV 0x1C7 (api id 1) selects `KEY_MATERIAL` v1 or v2; the driver logs the
  version it found and supports both for CCMP keys.
- RX `rx_pkt_type`: this firmware uses **2** for ordinary data; only 0xE5 (management) and 0xE7
  (block-ack request) are not data. Frames are 802.3 + SNAP or Ethernet II.
- Association: `HostCmd_CMD_802_11_ASSOCIATE` (0x12) with SSID, DS, CF, rates, auth type
  (0x11F, open), channel list and an RSN IE TLV (group cipher from the AP, pairwise CCMP, AKM PSK
  or 802.1X). The response is the IEEE association response (cap, status, AID, IEs).

## WiFiCx notes

- WiFiCx 1.0 / NetAdapterCx 2.1 / KMDF 1.33 (WDK defaults for WiFiCx 1.0); TLV generator/parser
  from `km/wlan/2.0` (`WificxTLVGenParse.lib`), compiled as C++17; the driver provides
  `operator new/delete` for it.
- Capabilities are set in `EvtDevicePrepareHardware` (after the firmware is up); WiFiCx then calls
  `EvtWifiDeviceCreateAdapter`. The station adapter needs the MAC address at that point.
- Messages seen at start: `SET_ADAPTER_CONFIGURATION` (85), `DOT11_RESET` (7), `GET_STATISTICS`
  (34, not supported), `SCAN` (4), 75 (not supported), `SET_LOCATION_PRIVACY` (138).
- Data path: the driver treats TX packets as native 802.11 data frames (ToDS) and falls back to
  passing them through if they already look like Ethernet II; RX frames are indicated as 802.11
  (FromDS). Conversion to/from Ethernet II uses RFC1042 / bridge-tunnel SNAP headers. The first
  four TX and RX frames are hex-dumped to the log.
