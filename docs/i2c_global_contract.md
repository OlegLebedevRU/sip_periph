# Global I2C Contract (ESP32 master <-> STM32 slave)

This document is the normative source of truth for the I2C transport contract between ESP32 (`master`) and STM32 (`slave`) in project `sip`.

`docs/i2c_uid532_contract.md` and `docs/i2c_gm810_contract.md` are scenario-specific profiles built on top of this document.

## 1. Scope

This contract defines:

- physical bus expectations relevant to interoperability
- wire-level transaction shape
- strict meaning of `reg`, `len`, and `type`
- register map used by the master
- packet-type to payload-window mapping for slave-initiated data
- STM32 slave FSM requirements for atomic receive/transmit handling
- migration gaps between the current STM32 implementation and the target contract

This contract does **not** redefine higher-level auth business rules except where they shape I2C packet layout.

## 2. Terms and invariants

### 2.1 Terms

- `reg`: RAM window offset inside STM32 I2C register space. This is the **first byte** sent by the master.
- `len`: exact payload size in bytes for the current transaction. This is the **second byte** sent by the master.
- `type`: semantic packet discriminator published by STM32 in `ram[0x00]` for slave-initiated flows.
- `payload`: bytes transferred after `[reg,len]`.

### 2.2 Non-negotiable invariants

1. ESP32 is always I2C master; STM32 is always I2C slave.
2. STM32 slave address is `0x11` (7-bit).
3. Bus speed is `400 kHz`.
4. Every master transaction starts with the fixed 2-byte header `[reg,len]`.
5. There are no variable-header transactions.
6. For master writes, the full frame is `[reg,len,payload...]`.
7. For master reads, the header is transmitted first and then exactly `len` bytes are read back.
8. `len` is part of the master contract and is never inferred by STM32 from `type`.
9. `type` is not a substitute for `reg`; it only tells the master which payload window to read after `0x00` is read.
10. Side effects on STM32 must occur only after the complete write frame has been atomically received.

## 3. Physical bus semantics

### 3.1 Read strategy

ESP32 uses EEPROM-style register reads.

Normative sequence:

1. `START`
2. master sends slave address with `WRITE`
3. master sends `reg`
4. master sends `len`
5. master issues `REPEATED START`
6. master sends slave address with `READ`
7. STM32 returns exactly `len` bytes from window `reg`
8. `STOP`

This is one logical read transaction.

### 3.2 Write strategy

Normative sequence:

1. `START`
2. master sends slave address with `WRITE`
3. master sends `reg`
4. master sends `len`
5. master sends exactly `len` payload bytes
6. `STOP`

This is one logical write transaction.

## 4. Role of `type`

`type` exists only for slave-initiated publish flows.

STM32 publishes packet type in:

- `0x00`: `I2C_PACKET_TYPE_ADDR`

Master flow is always:

1. receive IRQ from STM32
2. read `0x00` with `len=1`
3. interpret returned byte as `type`
4. map `type` to the target payload `reg`
5. read that payload window using the fixed master header `[reg,len]`

So:

- `type` answers **what kind of packet is pending**
- `[reg,len]` answers **what exact bytes the master reads/writes on the bus**

## 5. Register map

| Reg | Symbol | Purpose |
|---|---|---|
| `0x00` | `I2C_PACKET_TYPE_ADDR` | slave-published packet type |
| `0x01` | `I2C_REG_532_ADDR` | PN532/UID payload window |
| `0x10` | `I2C_REG_MATRIX_PIN_ADDR` | matrix PIN payload window |
| `0x20` | `I2C_REG_WIEGAND_ADDR` | wiegand payload window |
| `0x30` | `I2C_REG_COUNTER_ADDR` | service/counter write window |
| `0x40` | `I2C_REG_HMI_PIN_ADDR` | HMI PIN payload window |
| `0x50` | `I2C_REG_HMI_MSG_ADDR` | HMI message write window |
| `0x70` | `I2C_REG_HMI_ACT_ADDR` | auth/action result write window |
| `0x80` | `I2C_REG_HW_TIME_ADDR` | STM32 time read window |
| `0x88` | `I2C_REG_HW_TIME_SET_ADDR` | time sync write window |
| `0x90` | `I2C_REG_QR_GM810_ADDR` | GM810 QR payload window |
| `0xE0` | `I2C_REG_CFG_ADDR` | runtime config write block |
| `0xF0` | `I2C_REG_STM32_ERROR_ADDR` | STM32 diagnostic/error read window |

## 6. Slave-initiated packet catalog (`type -> reg -> len`)

The master reads `0x00`, obtains `type`, then uses this table.

| Type value | Symbol | Payload reg | Fixed read len | Notes |
|---|---|---|---|---|
| `0x00` | `PACKET_NULL` | `0x00` | `1` | no pending packet |
| `0x01` | `PACKET_UID_532` | `0x01` | `15` | `byte0=uid_len`, remaining bytes are data window |
| `0x02` | `PACKET_PIN` | `0x10` | `13` | matrix keyboard packet |
| `0x03` | `PACKET_WIEGAND` | `0x20` | `15` | wiegand packet |
| `0x04` | `PACKET_HMI` | `0x50` | implementation-defined / currently not used as slave publish path in master flow | reserved for explicit definition |
| `0x05` | `PACKET_TIME` | `0x80` | `8` | STM32 RTC packet |
| `0x06` | `PACKET_PIN_HMI` | `0x40` | `15` | touch HMI PIN packet |
| `0x07` | `PACKET_ACK` | n/a | n/a | reserved |
| `0x08` | `PACKET_NACK` | n/a | n/a | reserved |
| `0x09` | `PACKET_ERROR` | `0xF0` | `16` | diagnostic/error export |
| `0x0A` | `PACKET_QR_GM810` | `0x90` | `16` | fixed-length QR/GM810 publish window; byte0=`data_len`, byte1=`flags`, byte2=`chunk_index`, byte3=`chunk_total`, byte4..15=`data` |

### 6.1 `PACKET_QR_GM810` v1 payload profile

`PACKET_QR_GM810` uses the fixed `16`-byte window at `0x90`:

- `byte0`: `data_len`
- `byte1`: `flags`
- `byte2`: `chunk_index`
- `byte3`: `chunk_total`
- `byte4..15`: diagnostic or QR data bytes

For v1 the following rules are mandatory:

- `data_len <= 12`
- `chunk_index = 0`
- `chunk_total = 1`
- chunking/multi-frame reassembly is not used
- normal QR payload is limited to printable ASCII `0x20..0x7E`
- oversize payload is not published as a normal QR; STM32 still publishes `PACKET_QR_GM810` with `flags.error_oversize`
- payload containing non-printable / non-ASCII bytes is not published as a normal QR; STM32 still publishes `PACKET_QR_GM810` with `flags.error_non_ascii`
- when an error flag is set, `data[0..data_len-1]` is diagnostic payload inside the same fixed window; bytes placed there remain printable ASCII

`flags` bits for v1:

- bit0 = `from_protocol_mode`
- bit1 = reserved for future chunked mode, must be `0` in v1
- bit2 = `error_oversize`
- bit3 = `error_non_ascii`
- bit4..7 = reserved, must be treated as `0` unless a future profile defines them

## 7. Master write catalog (`reg -> len -> payload layout`)

All master writes use the exact frame `[reg,len,payload...]`.

| Reg | Fixed len | Payload layout | STM32 effect |
|---|---:|---|---|
| `0x70` | `5` | `[result, reg1_b3, reg1_b2, reg1_b1, reg1_b0]` | process auth result, parse `res_register_1` as big-endian, drive buzzer/relay flow |
| `0x50` | fixed allowed profile(s) per master revision | payload bytes after header are still carried under strict master-fixed `len`; no headerless/implicit-length write is allowed | deliver HMI message frame |
| `0x30` | `2` | service bytes currently observed as `[0x00, 0x03]` in normal cycle | refresh/apply runtime-related service state |
| `0x88` | `7` | RTC sync payload | validate and apply time sync |
| `0xE0` | `16` | config bytes `E0..EF` | refresh runtime config block |

### 7.1 `0x50` clarification

Even when message content originates from a logical variable-size application object, the I2C transaction itself is still defined by the strict master header `[reg,len]`.

The transport contract does **not** allow a variable-length transaction shape.

If future master implementation chooses more than one allowed `len` profile for `0x50`, each profile must still be explicitly enumerated in a higher-level scenario contract and treated as a fixed allowed frame for that master revision.

## 8. Atomic STM32 slave FSM requirements

The STM32 slave implementation shall behave as a 3-phase atomic FSM for master-originated transactions:

1. receive exactly 1 byte: `reg`
2. receive exactly 1 byte: `len`
3. receive exactly `len` bytes payload for write transactions, or use `len` as transmit size for read transactions

Required behavioral rules:

- `reg` and `len` are received as separate atomic phases
- no business side effect may run after only `reg` or only `len`
- side effects are allowed only after complete payload reception
- for reads, transmit length must come from received `len`
- after complete transmit or aborted transaction, FSM must return to LISTEN-ready state
- `AF` on end-of-read may complete a valid master read and must be handled without corrupting state
- real bus faults such as `BERR` must trigger deterministic recovery and LISTEN re-enable

## 9. Current implementation anchors

The following current code paths are the primary anchors for this contract:

- master transport helpers: `docs/l4_i2c_master.c`
  - `l4_i2c_dev_read_bytes()`
  - `l4_i2c_dev_write_bytes()`
- master packet flow: `docs/leo4_stm32_bus.c`
  - `stm32_task()`
  - `stm32_act_result_write_bus()`
- STM32 register map and packet lengths: `Core/Inc/main.h`
- STM32 slave FSM: `Core/Src/app_i2c_slave.c`
  - `app_i2c_slave_addr_callback()`
  - `app_i2c_slave_rx_complete()`
  - `app_i2c_slave_tx_complete()`
  - `app_i2c_slave_error()`

## 10. Migration gaps in current STM32 implementation

These gaps must be treated as concrete rework targets.

### 10.1 Read-without-header tolerance

Current `app_i2c_slave_addr_callback()` contains a tolerant path for read/probe access when the expected two-byte header has not been fully established.

Target contract requirement:

- production contract must only rely on the strict `[reg,len]` header
- tolerant probe compatibility must be removed from normative behavior or isolated as explicit legacy compatibility mode

### 10.2 Relaxed auth-result length

Current `strict_rx_len_for_register()` intentionally returns relaxed length for `HOST_AUTH_RESULT_RAM_ADDR` (`0x70`).

Target contract requirement:

- `0x70` must be fixed at `len=5`
- short writes are non-compliant and must not define normative behavior

### 10.3 Slave-side length derivation for publish packets

Current `packet_len_for_type()` derives published read sizes from `type` on the STM32 side.

That is acceptable as local outbox preparation, but it must not be confused with the wire-level source of truth.

Target contract requirement:

- on the wire, master always supplies `len`
- slave-side helper lengths are only local publication defaults for the corresponding RAM window contents

### 10.4 `0x50` documentation ambiguity

Existing scenario text may read as if HMI write length is variable by transaction shape.

Target contract requirement:

- transaction shape is always fixed-header `[reg,len]`
- any allowed `0x50` lengths must be explicitly listed by scenario/profile, not implied as header-less variability

## 11. Compliance rules for code rework

A reworked implementation is compliant only if all statements below are true:

- no master transaction is handled without first consuming `reg`
- no master transaction is handled without first consuming `len`
- all read paths use EEPROM-style `[reg,len] + repeated-start + read(len)`
- all write paths use `[reg,len,payload...]`
- `type` is read from `0x00` and then mapped to `reg+len` by the master
- STM32 side effects occur only after full write-frame completion
- `0x70` write is enforced as fixed `len=5`
- transport contract text contains no variable-length exception for master packets

## 12. Relationship to scenario documents

Scenario documents such as `docs/i2c_uid532_contract.md` and `docs/i2c_gm810_contract.md` must:

- inherit transport rules from this document
- specify only scenario-specific `type`, payload interpretation, and expected side effects
- not redefine physical read/write framing in a conflicting way
