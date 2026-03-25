# UID_532 I2C Contract (ESP32 master <-> STM32 slave)

This document defines the `UID_532` scenario on top of the global transport contract in `docs/i2c_global_contract.md`.

If any transport-level wording in older notes conflicts with the global contract, `docs/i2c_global_contract.md` is authoritative.

## 1. Scope and source files

- Global transport contract: `docs/i2c_global_contract.md`
- ESP32 packet handling: `docs/leo4_stm32_bus.c`
- Input/auth pipeline: `docs/l4_input_processing.c`
- I2C wire framing: `docs/l4_i2c_master.c`
- STM32 register constants: `Core/Inc/main.h`
- STM32 slave FSM: `Core/Src/app_i2c_slave.c`

## 2. Bus and signaling

- I2C role: ESP32 = master, STM32 = slave
- STM32 7-bit slave address: `0x11`
- ESP32 GPIO: `SDA=GPIO14`, `SCL=GPIO13`
- I2C speed: `400 kHz`
- IRQ line from STM32 to ESP32: `EVENT_INT_PIN=34`, active on negative edge

Event path for data-ready:

1. STM32 drives IRQ low.
2. ESP32 ISR posts `STM32_EVENT_I2C`.
3. Event handler pushes the event to queue.
4. `stm32_task` dequeues it and starts the read sequence.

## 3. Transport reminder for this scenario

This scenario uses the global fixed transport rules:

- master read header is always `[reg,len]`
- master write frame is always `[reg,len,payload...]`
- master read uses EEPROM-style access:
  - TX header `[reg,len]`
  - repeated-start
  - RX exactly `len` bytes

In this scenario:

- `type` is first read from `0x00`
- after `type` is known, master selects payload `reg` and fixed `len`

## 4. Registers used by UID_532 flow

- `0x00`: `I2C_PACKET_TYPE_ADDR`
- `0x01`: `I2C_REG_532_ADDR`
- `0x30`: `I2C_REG_COUNTER_ADDR`
- `0x50`: `I2C_REG_HMI_MSG_ADDR`
- `0x70`: `I2C_REG_HMI_ACT_ADDR`
- `0xE0`: `I2C_REG_CFG_ADDR` (block `E0..EF`)

Packet type values used here:

- `0x01`: `PACKET_UID_532`

Type mapping for this scenario:

- if `ram[0x00] == 0x01`, master must read payload from `0x01` with fixed `len=15`

## 5. End-to-end UID_532 packet path

After IRQ, ESP32 executes the following sequence.

### Step A: Read packet type

EEPROM-style read of packet type register:

- master TX header: `[0x00, 0x01]`
- repeated-start
- master RX (1 byte): `[ptype]`
- expected for this scenario: `ptype=0x01` (`PACKET_UID_532`)

### Step B: Read UID payload from `0x01`

EEPROM-style read of the UID payload window:

- master TX header: `[0x01, 0x0F]`
- repeated-start
- master RX (15 bytes):
  - `byte0`: `uid_len`
  - `byte1..byte14`: UID bytes / payload bytes

Parsing on ESP32 side:

- `input_dispatch(PN532_READER, data[0], &data[1], ...)`
- non-text normalization is hex (`%02X` per byte)
- stop condition while formatting: first `0x23` or `0x00`
- output string buffer is bounded (`16` chars with null terminator)

### Step C: ESP32 writes auth/action result to `0x70`

This write is always attempted after `UID_532` processing.

- master TX: `[0x70, 0x05, result, reg1_b3, reg1_b2, reg1_b1, reg1_b0]`
- `result`:
  - `0x00` fail
  - `0x01` success
  - `0x02` busy
- `res_register_1` is big-endian (`MSB first`)

STM32 requirement for this step:

- accept only the fixed master header shape for this write
- treat `len=5` as normative contract value
- parse `res_register_1` as big-endian

### Step D: ESP32 writes HMI message to `0x50`

In this flow, HMI write is optional at application level, but transport shape is still fixed.

Current master code writes:

- master TX: `[0x50, msg_len, payload...]`
- transport header is always present
- there is no header-less or alternate transaction shape

Payload source is `InputAuthResult_t.msg[]`.

Typical success display payload for default `free_pass` path:

- text is `"-- OTKP --"`
- display payload is derived from `InputAuthResult_t.msg[]`
- trailing message bytes may be padded with `0xFF`

For contract rework, allowed `0x50` payload profiles must be enumerated explicitly in the global/master documentation, while keeping the fixed `[reg,len]` master header.

### Step E: Service writes after packet processing

After packet branch handling, ESP32 also writes:

1. Counter write:
   - master TX: `[0x30, 0x02, 0x00, 0x03]`
2. Config block write:
   - master TX: `[0xE0, 0x10, cfg_E0, cfg_E1, ..., cfg_EF]`

STM32 must accept these writes in the normal `UID_532` processing cycle.

## 6. Behavior when NVS config is absent (default case)

### 6.1 Frontend/auth behavior for `PN532_READER`

Default frontend settings for `pn532` are pre-wired in code:

- `auth_ns = "auth_free"`
- `flags = 124 (0x7C)`
- `script = "free_pass"`

Impact in runtime when no overriding `cfg_frontend/pn532` is present:

- `free_pass` script marks input as valid immediately
- action result becomes success
- `res_register_1 = 0x00010000`
- display text is set to `"-- OTKP --"`

### 6.2 STM32 config block `E0..EF` origin and precedence

ESP32 builds `cfg_E0..cfg_EF` as follows:

1. start from master-delivered fallback (`STM32_CFG_DEFAULT`)
2. for each key `"E0".."EF"` found in NVS namespace `cfg_stm32`, override that byte
3. write final 16-byte block to STM32 register `0xE0`

This means partial NVS is valid: some bytes can come from NVS, others remain fallback.

## 7. Config bytes `E0..EF`

| Addr | Bits/Format | Meaning | Value | Source |
|---|---|---|---|---|
| `0xE0` | `bit0` | `relay_pulse_en` | `0/1` | runtime semantics |
| `0xE0` | `bit1` | `auth_timeout_act` | reserved for contract-facing compatibility | runtime semantics |
| `0xE0` | `bit2` | `auth_fail_act` | reserved for contract-facing compatibility | runtime semantics |
| `0xE0` | `bit3..7` | reserved | reserved | runtime semantics |
| `0xE0` | full byte | master-delivered fallback byte | `0x07` | master-delivered fallback |
| `0xE0` | full byte | baseline NVS profile | `0x01` | recommended NVS baseline profile |
| `0xE1` | `bits[3:0]` | `relay_act_sec` | `0..15 sec` | runtime semantics |
| `0xE1` | `bits[7:4]` | `relay_before_100ms` | `0..15` steps of `100 ms` | runtime semantics |
| `0xE1` | full byte | master-delivered fallback byte | `0x15` | master-delivered fallback |
| `0xE1` | full byte | baseline NVS profile | `0x05` | recommended NVS baseline profile |
| `0xE2` | full byte | reserved | reserved | runtime semantics |
| `0xE2` | full byte | master-delivered fallback byte | `0x00` | master-delivered fallback |
| `0xE3` | `bits[3:0]` | `matrix_freeze_sec` | `0..15 sec` | runtime semantics |
| `0xE3` | `bits[7:4]` | reserved | reserved | runtime semantics |
| `0xE3` | full byte | master-delivered fallback byte | `0x05` | master-delivered fallback |
| `0xE4` | `bits[3:0]` | `reader_interval_sec` | `0..15 sec` | runtime semantics |
| `0xE4` | `bits[7:4]` | reserved | reserved | runtime semantics |
| `0xE4` | full byte | master-delivered fallback byte | `0x05` | master-delivered fallback |
| `0xE5..0xEF` | full byte | reserved | `0xFF` each | master-delivered fallback |

STM32 requirement for reserved bytes:

- treat `E2`, `E5..EF` as reserved
- accept them without error
- keep forward compatibility

## 8. STM32-side checklist for this scenario

- support fixed master header `[reg,len]` for every read and write transaction
- for reads, honor EEPROM-style sequence `[reg,len] + repeated-start + RX(len)`
- on packet-ready IRQ, ensure `0x00` and `0x01` are stable before master polling
- for `UID_532`, provide 15-byte response at `0x01` with `byte0=uid_len`
- accept result write at `0x70` with fixed `len=5`
- accept HMI write at `0x50` only in fixed-header master form `[0x50,len,payload...]`
- accept service writes `[0x30,0x02,0x00,0x03]` and `[0xE0,0x10,...16 bytes...]`
- handle partial NVS-derived config naturally
- treat reserved config bytes as non-fatal and forward-compatible

## 9. Notes for interoperability tests

Recommended minimum STM32-side tests for this scenario:

1. Positive cycle: IRQ -> packet type `0x01` -> UID read -> result write accepted.
2. Verify EEPROM-style reads with repeated-start for `0x00` and `0x01`.
3. Verify `0x70` fixed-length write with big-endian `res_register_1` parsing.
4. Verify `0x50` is accepted only as fixed-header master transaction `[reg,len,payload...]`.
5. Verify config frame acceptance with:
   - pure fallback (`E0=0x07`, `E1=0x15`, ...)
   - recommended baseline profile via NVS (`E0=0x01`, `E1=0x05`)
   - partial override mix.
6. Verify slave FSM does not trigger side effects before full write-frame completion.