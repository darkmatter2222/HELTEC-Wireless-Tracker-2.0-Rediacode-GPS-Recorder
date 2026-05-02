# RadiaCode Device — BLE Protocol Reference

Full technical reference for how the RadiaCode radiation-detector product
family communicates over Bluetooth Low Energy (BLE).

**Sources used to compile this document**:
- Reverse-engineered open-source Python library: https://github.com/cdump/radiacode
- Firmware implementation in this repo: `src/radiacode.h` / `src/radiacode.cpp`
- Official product pages: https://www.radiacode.com/

---

## 1. Supported Hardware Models

| Model        | Notes |
|--------------|-------|
| RC-101       | Earliest 100-series; legacy BLE (BT 4.x classic advertising) |
| RC-102       | Most common; legacy BLE + USB; stable BLE connection |
| RC-103       | Same protocol as 102; adds 3rd detector crystal |
| RC-103G      | Same protocol; upgraded scintillator |
| RC-110       | Latest (BT5 extended advertising on secondary PHY channel); quirky behaviour — see Section 9 |
| RC ZERO      | High-range high-dose variant; same BLE protocol family |

All RC-10x models share the same BLE GATT profile and wire protocol.
RC-110 has BT5-specific advertisement behaviour and requires additional
SMP security steps before commands are accepted.

---

## 2. BLE Advertisement

### How to detect a RadiaCode device

A peer is a "likely RadiaCode" if **any** of the following are true:

1. Advertised Local Name starts with `radiacode`, `radiacod`, or `rc-`
   (case-insensitive). Examples: `RadiaCode`, `RadiaCode-102`, `RC-102`.
2. Advertises service UUID `e63215e5-7003-49d8-96b0-b024798fb901`
   (the Custom RadiaCode Service).
3. Advertises short UUID `0xFEAF` (Nordic UART Service).  
   The custom service UUID is only visible post-connect, so `0xFEAF` is a
   reliable pre-connect signal for all current models.

### RC-110 advertisement quirk

The RC-110 uses **BT5 extended advertising** on secondary ADV channels. Its
connectable advertisement packets appear briefly and infrequently because the
peer is often bonded or sleeping. The scanner must use:

```c
CONFIG_BT_NIMBLE_EXT_ADV=1
CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES=0
CONFIG_BT_NIMBLE_EXT_ADV_MAX_SIZE=255
CONFIG_BT_CTRL_SCAN_DUPL_TYPE_DATA_DEVICE=1
CONFIG_BT_CTRL_SCAN_DUPL_TYPE=2
```

Without `CONFIG_BT_NIMBLE_EXT_ADV=1` the RC-110 is **never** found even
though it is broadcasting.

The RC-110 also often broadcasts `NONCONN_IND` (non-connectable) for long
windows between brief connectable windows. Always filter for a genuinely
**connectable** advertisement before sending `CONNECT_REQ`.

---

## 3. GATT Profile

All communication uses a single custom service with two characteristics.

| Role         | UUID                                   | BLE Properties |
|--------------|----------------------------------------|----------------|
| Service      | `e63215e5-7003-49d8-96b0-b024798fb901` | — |
| **Write**    | `e63215e6-7003-49d8-96b0-b024798fb901` | Write Without Response |
| **Notify**   | `e63215e7-7003-49d8-96b0-b024798fb901` | Notify |

The device name is sometimes absent from advertising data (especially RC-110).
Read GAP Service `0x1800`, Characteristic `0x2A00` (Device Name) after
connecting to resolve it.

### Enabling notifications

Write `0x0100` to the CCCD (handle = notify characteristic handle + 1) before
sending any commands. NimBLE-Arduino does this automatically via
`characteristic->subscribe(true, callback)`.

---

## 4. BLE Connection Parameters (what works)

| Parameter              | Recommended Value | Notes |
|------------------------|-------------------|-------|
| Min connection interval | 50 ms (40 units)  | 15–30 ms caused status=13 timeouts |
| Max connection interval | 100 ms (80 units) | |
| Slave latency          | 0                 | |
| Supervision timeout    | 30 s (3000 units) | RC-110 needs >6s for initial pairing setup |
| Connect timeout        | 20 s              | |
| PHY                    | 1M only           | Offering CODED PHY breaks some peers |

Write chunks: **18 bytes** maximum per write-without-response packet.
This matches both the Nordic SDK TX buffer size and what both the Python lib
and Android app use.

---

## 5. Protocol Frame Format

All requests and responses follow the same TLV-ish framing.

### Request frame

```
[ 4 bytes: U32LE total inner length ]
[ 2 bytes: U16LE command_id        ]
[ 1 byte : 0x00 (always zero)      ]
[ 1 byte : seq_no (0x80 | (counter & 0x1F)) ]
[ N bytes: command arguments (little-endian) ]
```

- `seq_no` cycles 0x80–0x9F (5-bit counter OR'd with 0x80).
- Must be split into ≤18-byte chunks for write-without-response.
- Brief `delay(5 ms)` between consecutive chunks prevents buffer overflow on the peer.

### Response / notification frame

Responses arrive via BLE notifications and may span multiple packets.

```
First notification packet:
[ 4 bytes: U32LE total payload length ]
[ N bytes: first payload bytes        ]

Subsequent notification packets:
[ N bytes: continuation payload bytes ]
```

Once accumulated bytes reach `total_payload_length`, the response is complete.

### Response payload structure

```
[ 2 bytes: U16LE echoed command_id ]
[ 1 byte : 0x00                   ]
[ 1 byte : echoed seq_no          ]
[ M bytes: actual response data   ]
```

The echoed 4-byte header must match what was sent (used to validate no
sequence skew). Actual response data begins at offset 4 within the payload.

---

## 6. Command Reference

### 6.1 Top-Level Commands (`COMMAND` enum)

| Command              | Opcode   | Description |
|----------------------|----------|-------------|
| `GET_STATUS`         | `0x0005` | Read device status flags (U32) |
| `SET_EXCHANGE`       | `0x0007` | Handshake; must be first command after connect |
| `GET_VERSION`        | `0x000A` | Read boot + target firmware version |
| `GET_SERIAL`         | `0x000B` | Read hardware serial number |
| `FW_IMAGE_GET_INFO`  | `0x0012` | Firmware image info |
| `FW_SIGNATURE`       | `0x0101` | Firmware signature, filename, ID string |
| `RD_HW_CONFIG`       | `0x0807` | Read hardware configuration |
| `RD_VIRT_SFR`        | `0x0824` | Read one Virtual Special Function Register |
| `WR_VIRT_SFR`        | `0x0825` | Write one VSFR |
| `RD_VIRT_STRING`     | `0x0826` | Read a Virtual String (VS) buffer |
| `WR_VIRT_STRING`     | `0x0827` | Write a Virtual String buffer |
| `RD_VIRT_SFR_BATCH`  | `0x082A` | Read multiple VSFRs in one round-trip |
| `WR_VIRT_SFR_BATCH`  | `0x082B` | Write multiple VSFRs in one round-trip |
| `RD_FLASH`           | `0x081C` | Read raw flash memory |
| `SET_TIME`           | `0x0A04` | Synchronise device clock |

### 6.2 SET_EXCHANGE (0x0007)

**Must be the very first command** sent after connecting. Initializes the
session; without it the device ignores all subsequent commands.

Arguments (4 bytes): `0x01 0xFF 0x12 0xFF`

Timeout: 25 s (longer than other commands — give extra time on first handshake).

### 6.3 SET_TIME (0x0A04)

Synchronises the device's real-time clock. Arguments are 8 bytes:

```
[ U8: day         ] (1–31)
[ U8: month       ] (1–12)
[ U8: year-2000   ] (e.g. 26 for 2026)
[ U8: 0x00        ]
[ U8: seconds     ]
[ U8: minutes     ]
[ U8: hours       ]
[ U8: 0x00        ]
```

### 6.4 RD_VIRT_STRING (0x0826)

Read a named buffer by VS (Virtual String) ID.

Arguments: `U32LE vs_id`

Response layout (after the echoed header):
```
[ U32LE: retcode  ]  must be 1 for success
[ U32LE: data_len ]
[ data_len bytes  ]  trailing 0x00 is stripped if present
```

### 6.5 WR_VIRT_SFR (0x0825)

Write a VSFR by address.

Arguments:
```
[ U32LE: vsfr_address ]
[ U32LE: value        ]
```

Response: `U32LE retcode` (must be 1), then empty.

### 6.6 RD_VIRT_SFR (0x0824)

Read a VSFR by address.

Arguments: `U32LE vsfr_address`

### 6.7 RD_VIRT_SFR_BATCH (0x082A)

Read multiple VSFRs atomically.

Arguments:
```
[ U32LE: count    ]
[ U32LE: vsfr_id0 ]
[ U32LE: vsfr_id1 ]
...
```

Response:
```
[ U32LE: valid_flags bitmask ]  bit N = 1 if VSFR N was read successfully
[ U32LE: value0 ]
[ U32LE: value1 ]
...
```

---

## 7. Virtual Strings (VS)

Read with `RD_VIRT_STRING` using the VS ID as the 32-bit argument.

| VS Name          | ID      | Content |
|------------------|---------|---------|
| `CONFIGURATION`  | `0x002` | Device configuration text, CP-1251 encoded |
| `FW_DESCRIPTOR`  | `0x003` | Firmware descriptor |
| `SERIAL_NUMBER`  | `0x008` | Serial number, ASCII |
| `TEXT_MESSAGE`   | `0x00F` | Pending text message, ASCII |
| `MEM_SNAPSHOT`   | `0x0E0` | Memory snapshot |
| `DATA_BUF`       | `0x100` | Real-time measurement ring buffer (see Section 8) |
| `SFR_FILE`       | `0x101` | SFR address map (lists all VSFRs, their size, type, sign) |
| `SPECTRUM`       | `0x200` | Live spectrum (current measurement) |
| `ENERGY_CALIB`   | `0x202` | Energy calibration coefficients (3x F32LE) |
| `SPEC_ACCUM`     | `0x205` | Accumulated spectrum |
| `SPEC_DIFF`      | `0x206` | Differential spectrum |
| `SPEC_RESET`     | `0x207` | Spectrum reset trigger |

**Polling `DATA_BUF` at ~1 Hz** is the standard real-time data loop.

---

## 8. VS_DATA_BUF Decoder

`DATA_BUF` is a TLV ring buffer containing timestamped records.
Each record starts with a 7-byte header:

```
[ U8:  seq       ]  sequence number (monotonic, wraps mod 256)
[ U8:  eid       ]  event class (0 = measurement, 1 = spectrum)
[ U8:  gid       ]  group/record type within the class
[ I32LE: ts_off  ]  time offset from base_time in 10ms units
```

Timestamp = `base_time + ts_off * 10 ms`  
`base_time` is established after `SET_TIME` + `WR_VIRT_SFR(DEVICE_TIME, 0)`.
The device then reports offsets relative to that anchor.

### Record types (`eid=0`)

| GID | Name            | Payload Layout | Description |
|-----|-----------------|----------------|-------------|
| `0` | `RealTimeData`  | `<ffHHHB>` (15 bytes) | count_rate (F32), dose_rate (F32), count_rate_err×10 (U16), dose_rate_err×10 (U16), flags (U16), rt_flags (U8) |
| `1` | `RawData`       | `<ff>` (8 bytes) | count_rate (F32), dose_rate (F32) |
| `2` | `DoseRateDB`    | `<IffHH>` (16 bytes) | count (U32), count_rate (F32), dose_rate (F32), dose_rate_err×10 (U16), flags (U16) |
| `3` | `RareData`      | `<IfHHH>` (14 bytes) | duration (U32, seconds), dose (F32), temp_raw (U16), charge_raw (U16), flags (U16) |
| `4` | `UserData`      | `<IffHH>` (16 bytes) | Same layout as DoseRateDB |
| `5` | `ScheduleData`  | `<IffHH>` (16 bytes) | Same layout as DoseRateDB |
| `6` | `AccelData`     | `<HHH>` (6 bytes) | acc_x, acc_y, acc_z (raw) |
| `7` | `Event`         | `<BBH>` (4 bytes) | event_id (U8), param1 (U8), flags (U16) |
| `8` | `RawCountRate`  | `<fH>` (6 bytes) | count_rate (F32), flags (U16) |
| `9` | `RawDoseRate`   | `<fH>` (6 bytes) | dose_rate (F32), flags (U16) |

### Record types (`eid=1` — spectrum segments)

| GID | Bytes per sample | Description |
|-----|-----------------|-------------|
| `1` | 8               | Spectrum segment type 1 |
| `2` | 16              | Spectrum segment type 2 |
| `3` | 14              | Spectrum segment type 3 |

For spectrum records, after the 7-byte header:
```
[ U16: samples_num  ]
[ U16: smpl_time_ms ]
[ samples_num * bytes_per_sample ]
```

**Unknown group IDs**: stop parsing immediately. Do not attempt to skip
unknown records — the length is not encoded and sync cannot be recovered
without restarting the poll.

### Field unit conversions

| Field             | Raw unit         | Converted unit    | Formula |
|-------------------|------------------|-------------------|---------|
| `dose_rate`       | internal F32     | µSv/h             | `raw × 10000.0` |
| `count_rate_err`  | U16 ÷ 10         | percent           | `raw / 10.0` |
| `dose_rate_err`   | U16 ÷ 10         | percent           | `raw / 10.0` |
| `temp_raw`        | U16              | °C                | `(raw - 2000) / 100.0` |
| `charge_raw`      | U16              | % (0–100)         | `raw / 100`, clamped to [0, 100] |
| `duration`        | U32              | seconds           | direct |
| `dose`            | F32              | µR accumulated    | direct |

---

## 9. Virtual SFRs (VSFR)

Write/read with `WR_VIRT_SFR` / `RD_VIRT_SFR` (or their BATCH variants).
Values are always 4 bytes little-endian on the wire.

### Device Control (0x05xx)

| VSFR           | Address  | Type   | Description |
|----------------|----------|--------|-------------|
| `DEVICE_CTRL`  | `0x0500` | U8(3x) | Device control flags |
| `DEVICE_LANG`  | `0x0502` | U8(3x) | Language: 0=Russian, 1=English |
| `DEVICE_ON`    | `0x0503` | bool   | Power device on/off |
| `DEVICE_TIME`  | `0x0504` | U32    | Write 0 to reset time base after SET_TIME |

### Display (0x051x)

| VSFR            | Address  | Type   | Description |
|-----------------|----------|--------|-------------|
| `DISP_CTRL`     | `0x0510` | U8(3x) | Display control |
| `DISP_BRT`      | `0x0511` | U8(3x) | Brightness (0–9) |
| `DISP_CONTR`    | `0x0512` | U8(3x) | Contrast |
| `DISP_OFF_TIME` | `0x0513` | U32    | Auto-off: 0=5s, 1=10s, 2=15s, 3=30s |
| `DISP_ON`       | `0x0514` | bool   | Display on/off |
| `DISP_DIR`      | `0x0515` | U8(3x) | Direction: 0=auto, 1=right, 2=left |
| `DISP_BACKLT_ON`| `0x0516` | bool   | Backlight on/off |

### Sound (0x052x)

| VSFR           | Address  | Type   | Description |
|----------------|----------|--------|-------------|
| `SOUND_CTRL`   | `0x0520` | U16(2x)| Event bitmask for sounds (see CTRL flags) |
| `SOUND_VOL`    | `0x0521` | U8(3x) | Volume |
| `SOUND_ON`     | `0x0522` | bool   | Sounds enabled |
| `SOUND_BUTTON` | `0x0523` | bool   | Button click sound |

### Vibration (0x053x)

| VSFR          | Address  | Type   | Description |
|---------------|----------|--------|-------------|
| `VIBRO_CTRL`  | `0x0530` | U8(3x) | Event bitmask for vibration (CTRL.CLICKS not supported) |
| `VIBRO_ON`    | `0x0531` | bool   | Vibration enabled |

### LEDs (0x054x)

| VSFR       | Address  | Type   | Description |
|------------|----------|--------|-------------|
| `LEDS_CTRL`| `0x0540` | —      | LED control |
| `LED0_BRT` | `0x0541` | U8(3x) | LED 0 brightness |
| `LED1_BRT` | `0x0542` | U8(3x) | LED 1 brightness |
| `LED2_BRT` | `0x0543` | U8(3x) | LED 2 brightness |
| `LED3_BRT` | `0x0544` | U8(3x) | LED 3 brightness |
| `LEDS_ON`  | `0x0545` | bool   | LEDs enabled |

### Alarm / Signal (0x05Ex)

| VSFR          | Address  | Type   | Description |
|---------------|----------|--------|-------------|
| `ALARM_MODE`  | `0x05E0` | U8(3x) | Alarm mode |
| `PLAY_SIGNAL` | `0x05E1` | U8(3x) | Play signal |

### Measurement Source (0x060x)

| VSFR        | Address  | Type  | Description |
|-------------|----------|-------|-------------|
| `MS_CTRL`   | `0x0600` | —     | Measurement source control |
| `MS_MODE`   | `0x0601` | —     | Mode |
| `MS_SUB_MODE` | `0x0602`| —    | Sub-mode |
| `MS_RUN`    | `0x0603` | bool  | Start/stop measurement |

### BLE (0x070x)

| VSFR       | Address  | Type   | Description |
|------------|----------|--------|-------------|
| `BLE_TX_PWR`| `0x0700`| U8(3x) | TX power level |

### Alarm Limits (0x80xx)

| VSFR             | Address  | Type | Description |
|------------------|----------|------|-------------|
| `DR_LEV1_uR_h`   | `0x8000` | U32  | Dose rate level 1 (µR/h) |
| `DR_LEV2_uR_h`   | `0x8001` | U32  | Dose rate level 2 (µR/h) |
| `DS_LEV1_100uR`  | `0x8002` | U32  | Dose level 1 (×100 µR) |
| `DS_LEV2_100uR`  | `0x8003` | U32  | Dose level 2 (×100 µR) |
| `DS_UNITS`       | `0x8004` | bool | 0=Roentgen, 1=Sievert |
| `CPS_FILTER`     | `0x8005` | U8(3x)| CPS filter setting |
| `RAW_FILTER`     | `0x8006` | —    | Raw filter |
| `DOSE_RESET`     | `0x8007` | bool | Write true to reset accumulated dose |
| `CR_LEV1_cp10s`  | `0x8008` | U32  | Count rate level 1 (counts/10s) |
| `CR_LEV2_cp10s`  | `0x8009` | U32  | Count rate level 2 (counts/10s) |
| `USE_nSv_h`      | `0x800C` | bool | Display in nSv/h instead of µSv/h |
| `CR_UNITS`       | `0x8013` | bool | 0=cps, 1=cpm |
| `DS_LEV1_uR`     | `0x8014` | U32  | Dose level 1 (µR) |
| `DS_LEV2_uR`     | `0x8015` | U32  | Dose level 2 (µR) |

### Read-Only Sensor Values (0x802x–0x803x)

| VSFR          | Address  | Type | Description |
|---------------|----------|------|-------------|
| `CPS`         | `0x8020` | U32  | Current counts per second |
| `DR_uR_h`     | `0x8021` | U32  | Dose rate (µR/h) |
| `DS_uR`       | `0x8022` | U32  | Accumulated dose (µR) |
| `TEMP_degC`   | `0x8024` | F32  | Temperature (°C) |
| `ACC_X`       | `0x8025` | I16(2x)| Accelerometer X |
| `ACC_Y`       | `0x8026` | I16(2x)| Accelerometer Y |
| `ACC_Z`       | `0x8027` | I16(2x)| Accelerometer Z |
| `OPT`         | `0x8028` | U16(2x)| Optical sensor |
| `RAW_TEMP_degC`| `0x8033`| F32  | Raw temperature |
| `TEMP_UP_degC`| `0x8034` | F32  | Upper temperature sensor |
| `TEMP_DN_degC`| `0x8035` | F32  | Lower temperature sensor |

### Calibration / Hardware (0xC0xx)

| VSFR              | Address  | Type   | Description |
|-------------------|----------|--------|-------------|
| `VBIAS_mV`        | `0xC000` | U16(2x)| Detector bias voltage (mV) |
| `COMP_LEV`        | `0xC001` | I16(2x)| Comparator level |
| `CALIB_MODE`      | `0xC002` | bool   | Calibration mode |
| `DPOT_RDAC`       | `0xC004` | U8(3x) | Digital potentiometer |
| `DPOT_RDAC_EEPROM`| `0xC005` | U8(3x) | Pot EEPROM value |
| `DPOT_TOLER`      | `0xC006` | U8(3x) | Pot tolerance |

### Energy Calibration VSFRs (0x801x)

| VSFR          | Address  | Type | Description |
|---------------|----------|------|-------------|
| `CHN_TO_keV_A0`| `0x8010`| F32  | Energy calibration: offset (keV) |
| `CHN_TO_keV_A1`| `0x8011`| F32  | Energy calibration: linear (keV/channel) |
| `CHN_TO_keV_A2`| `0x8012`| F32  | Energy calibration: quadratic (keV/ch²) |

Energy (keV) = A0 + A1×channel + A2×channel²

### System VSFRs (0xFFFF000x)

| VSFR               | Address       | Type | Description |
|--------------------|---------------|------|-------------|
| `SYS_MCU_ID0`      | `0xFFFF0000`  | U32  | MCU unique ID word 0 |
| `SYS_MCU_ID1`      | `0xFFFF0001`  | U32  | MCU unique ID word 1 |
| `SYS_MCU_ID2`      | `0xFFFF0002`  | U32  | MCU unique ID word 2 |
| `SYS_DEVICE_ID`    | `0xFFFF0005`  | U32  | Device identifier |
| `SYS_SIGNATURE`    | `0xFFFF0006`  | U32  | Firmware signature |
| `SYS_RX_SIZE`      | `0xFFFF0007`  | U16(2x)| RX buffer size |
| `SYS_TX_SIZE`      | `0xFFFF0008`  | U16(2x)| TX buffer size |
| `SYS_BOOT_VERSION` | `0xFFFF0009`  | U32  | Boot loader version |
| `SYS_TARGET_VERSION`| `0xFFFF000A` | U32  | Target firmware version |
| `SYS_STATUS`       | `0xFFFF000B`  | U32  | System status flags |
| `SYS_MCU_VREF`     | `0xFFFF000C`  | I32  | MCU voltage reference |
| `SYS_MCU_TEMP`     | `0xFFFF000D`  | I32  | MCU temperature |
| `SYS_FW_VER_BT`    | `0xFFFF0010`  | —    | BT firmware version |

---

## 10. CTRL Event Flags

Used to configure which device events trigger sounds or vibration
(`SOUND_CTRL` / `VIBRO_CTRL` VSFRs).

| Flag                  | Bit mask |
|-----------------------|----------|
| `BUTTONS`             | `0x01`   |
| `CLICKS`              | `0x02`   |
| `DOSE_RATE_ALARM_1`   | `0x04`   |
| `DOSE_RATE_ALARM_2`   | `0x08`   |
| `DOSE_RATE_OUT_OF_SCALE`| `0x10` |
| `DOSE_ALARM_1`        | `0x20`   |
| `DOSE_ALARM_2`        | `0x40`   |
| `DOSE_OUT_OF_SCALE`   | `0x80`   |

Note: `CLICKS` is not supported for `VIBRO_CTRL`.

---

## 11. Event IDs (in DATA_BUF Event records)

| EventId                  | Value | Description |
|--------------------------|-------|-------------|
| `POWER_OFF`              | 0     | Device powered off |
| `POWER_ON`               | 1     | Device powered on |
| `LOW_BATTERY_SHUTDOWN`   | 2     | Shutdown due to low battery |
| `CHANGE_DEVICE_PARAMS`   | 3     | Device parameters changed |
| `DOSE_RESET`             | 4     | Accumulated dose was reset |
| `USER_EVENT`             | 5     | User-triggered event |
| `BATTERY_EMPTY_ALARM`    | 6     | Battery empty alarm |
| `CHARGE_START`           | 7     | Charging started |
| `CHARGE_STOP`            | 8     | Charging stopped |
| `DOSE_RATE_ALARM1`       | 9     | Dose rate exceeded level 1 |
| `DOSE_RATE_ALARM2`       | 10    | Dose rate exceeded level 2 |
| `DOSE_RATE_OFFSCALE`     | 11    | Dose rate off scale |
| `DOSE_ALARM1`            | 12    | Accumulated dose exceeded level 1 |
| `DOSE_ALARM2`            | 13    | Accumulated dose exceeded level 2 |
| `DOSE_OFFSCALE`          | 14    | Accumulated dose off scale |
| `TEMPERATURE_TOO_LOW`    | 15    | Temperature below operating range |
| `TEMPERATURE_TOO_HIGH`   | 16    | Temperature above operating range |
| `TEXT_MESSAGE`           | 17    | Text message available (read VS.TEXT_MESSAGE) |
| `MEMORY_SNAPSHOT`        | 18    | Memory snapshot ready |
| `SPECTRUM_RESET`         | 19    | Spectrum cleared |
| `COUNT_RATE_ALARM1`      | 20    | Count rate exceeded level 1 |
| `COUNT_RATE_ALARM2`      | 21    | Count rate exceeded level 2 |
| `COUNT_RATE_OFFSCALE`    | 22    | Count rate off scale |

---

## 12. Initialization Sequence

The mandatory 3-step init must complete before polling data:

```
Step 1: SET_EXCHANGE  args=[0x01, 0xFF, 0x12, 0xFF]
         (timeout 25s; longer than all other commands)
         Purpose: activates the BLE session on the device side

Step 2: SET_TIME  args=[day, month, year-2000, 0, sec, min, hour, 0]
         Purpose: sets the RTC; provides the base_time anchor for
                  all subsequent DATA_BUF timestamps

Step 3: WR_VIRT_SFR(DEVICE_TIME=0x0504, value=0)
         Purpose: resets the device's internal time counter to 0
                  so ts_offset values start from a known epoch

--> State: Ready  (poll DATA_BUF at ~1 Hz now)
```

After init, poll `RD_VIRT_STRING(VS.DATA_BUF = 0x100)` repeatedly.
Each poll returns all records accumulated since the last poll.

---

## 13. Spectrum Data

Spectrum data is read with `RD_VIRT_STRING(VS.SPECTRUM = 0x200)`.
The response contains a `Spectrum` object:

```
duration:  timedelta  (measurement live time)
a0:        float      (energy calibration offset, keV)
a1:        float      (energy calibration linear, keV/channel)
a2:        float      (energy calibration quadratic, keV/ch²)
counts:    list[int]  (per-channel photon counts, typically 1024 channels)
```

Energy of channel N (keV) = a0 + a1×N + a2×N²

Related VS IDs:
- `SPECTRUM (0x200)` — current live spectrum
- `SPEC_ACCUM (0x205)` — accumulated (total) spectrum
- `ENERGY_CALIB (0x202)` — calibration coefficients only (3× F32LE)

To reset spectrum: `WR_VIRT_STRING(VS.SPECTRUM, value=0)`

---

## 14. Model-Specific Quirks

### RC-102 (and 101, 103, 103G)

- Legacy BLE 4.x advertising; standard connectable ADV.
- Name is in the advertisement packet; no extra scan required.
- `secureConnection()` works harmlessly but is not required.
- Stable; short-lived link disconnects are rare.
- Minimum firmware for Python lib: v4.8. Upgrade if below.

### RC-110

- **BT5 extended advertising** — must have `CONFIG_BT_NIMBLE_EXT_ADV=1`.
- Does **not** advertise its name; resolve via GAP `0x2A00` post-connect.
- Enters **brief connectable windows** separated by long NONCONN_IND periods
  because it is typically bonded and uses BT5 secondary channel advertising.
  Use a 100%-duty-cycle scanner and only act on `isConnectable()=true` events.
- **Requires `secureConnection()` immediately after connect** (before GATT
  discovery). The SMP pairing always fails (rc=1283 "auth requirements") — this
  is expected and non-fatal. The SMP attempt alone is sufficient to flip the
  peer's internal "client authenticated" policy bit, after which it accepts
  `SET_EXCHANGE` writes. Without this call, all writes are silently ignored.
- **Soft-brick risk**: if a connection drops within ~90 seconds of linking
  (before the device has finished internal BT setup), subsequent reconnect
  attempts can compound the brick. Strategy:
  - Detect short-lived links (<90s) and halt auto-retry.
  - Do NOT immediately re-scan/reconnect after a short-lived drop.
  - The user must power-cycle the RC-110, then manually reconnect.
- Service discovery can take >6s; use a 30s supervision timeout.
- Offer 1M PHY only (`BLE_GAP_LE_PHY_1M_MASK`). Offering CODED PHY prevents
  some peers from ACKing `CONNECT_REQ`.
- Bulk-discover all characteristics in one round-trip (`svc->getCharacteristics(true)`)
  before looking up individual UUIDs — two sequential lookups can cause the
  second to time out on the 110 firmware.

---

## 15. NVS Persistence (This Firmware)

This firmware stores two keys in ESP32 NVS (namespace `rctracker`):

| Key          | Content |
|--------------|---------|
| `last_peer`  | BLE address of the most recently connected peer |
| `pinned_peer`| User-pinned target address; auto-mode ONLY connects here |
| `grab_pat`   | Auto-grab name pattern (case-insensitive substring match) |

`pinned_peer` prevents wasted connect attempts to imposters that advertise the
RadiaCode service UUID but fail service discovery.

`grab_pat` is for racing the RC-110's brief connectable window: the scanner
immediately pins + connects to any peer whose name contains the pattern.

Serial commands:
- `t <pattern>` — set grab pattern (e.g. `t RadiaCode`)
- `t`           — clear grab pattern
- `f`           — forget pinned peer, trigger rescan
- `D`           — disconnect, keep pin (auto-reconnect resumes)

---

## 16. Firmware Compatibility

The Python library (cdump/radiacode) requires device firmware **≥ 4.8**.
Older firmware uses a different DATA_BUF format and may not respond correctly
to `SET_EXCHANGE`. Upgrade via the official RadiaCode mobile app over USB if
the device is below v4.8.

---

## 17. References

| Resource | URL |
|----------|-----|
| Open-source Python library (cdump/radiacode) | https://github.com/cdump/radiacode |
| Official RadiaCode product site | https://www.radiacode.com/ |
| Official documentation | https://radiacode.com/docs/en |
| This firmware's BLE implementation | `src/radiacode.h`, `src/radiacode.cpp` |
| NimBLE-Arduino library | https://github.com/h2zero/NimBLE-Arduino |
