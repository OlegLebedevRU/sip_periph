# GM810 I2C Contract (ESP32 master <- STM32 slave)

This document is the GM810-specific profile for QR events published by STM32 to ESP32.

It is both:

- a developer-readable contract for ESP32 firmware work
- a prompt/spec that can be given to an AI agent implementing the ESP32 side

`docs/i2c_global_contract.md` remains the normative transport source of truth. This document only defines the GM810 packet profile on top of that transport.

## 1. Scope

This profile defines how ESP32 must consume `PACKET_QR_GM810` from STM32 in v1.

It does **not** redefine:

- the I2C header shape `[reg,len]`
- the IRQ-driven `type -> reg -> len` read flow
- generic STM32 slave behavior outside the GM810 window

## 2. Packet identity

- packet type value: `0x0A`
- packet symbol: `PACKET_QR_GM810`
- payload register: `0x90`
- payload register symbol: `I2C_REG_QR_GM810_ADDR`
- fixed read length: `16`

ESP32 flow is always:

1. receive STM32 event IRQ
2. read `0x00` with `len=1`
3. if returned `type == 0x0A`, read register `0x90` with `len=16`
4. decode the fixed GM810 payload window

## 3. Fixed 16-byte payload window

| Byte | Name | Meaning |
|---|---|---|
| `0` | `data_len` | number of bytes present in `data[0..11]` |
| `1` | `flags` | QR/diagnostic flags |
| `2` | `chunk_index` | reserved field |
| `3` | `chunk_total` | reserved field |
| `4..15` | `data` | QR bytes or diagnostic payload |

## 4. Mandatory v1 rules

- `data_len <= 12`
- `chunk_index = 0`
- `chunk_total = 1`
- chunking is not used in v1
- normal QR payload is limited to printable ASCII `0x20..0x7E`
- ESP32 must not expect UTF-8 multibyte payloads in v1
- ESP32 must not implement multi-frame assembly for this packet in v1

## 5. Flags

| Bit | Symbol | Meaning in v1 |
|---|---|---|
| `0` | `from_protocol_mode` | STM32 received the GM810 frame in protocol mode `<0x03><len><data>` |
| `1` | `reserved_chunked` | reserved for future chunking; must be `0` in v1 |
| `2` | `error_oversize` | source frame length was greater than `12` |
| `3` | `error_non_ascii` | source frame contained bytes outside printable ASCII `0x20..0x7E` |
| `4..7` | reserved | must be treated as reserved |

## 6. How ESP32 must interpret the packet

### 6.1 Normal QR packet

Normal QR packet means:

- `error_oversize == 0`
- `error_non_ascii == 0`
- `data_len` is in `1..12`

ESP32 should treat `data[0:data_len]` as the QR payload.

### 6.2 Oversize packet

Oversize packet means:

- `error_oversize == 1`

Rules:

- this is **not** a normal QR payload
- STM32 still publishes `PACKET_QR_GM810` instead of a separate error packet
- `data_len` reflects how many diagnostic bytes were placed into the fixed 12-byte data field
- diagnostic bytes remain printable ASCII inside the fixed window; invalid source bytes may be sanitized to `?`
- ESP32 must treat the packet as an input error / rejected QR

### 6.3 Non-ASCII packet

Non-ASCII packet means:

- `error_non_ascii == 1`

Rules:

- this is **not** a normal QR payload
- STM32 still publishes `PACKET_QR_GM810` instead of a separate error packet
- `data_len` reflects how many diagnostic bytes were placed into the fixed 12-byte data field
- diagnostic bytes remain printable ASCII inside the fixed window; invalid source bytes may be sanitized to `?`
- ESP32 must treat the packet as an input error / rejected QR

### 6.4 Combined error case

If both bits are set:

- the source frame was both oversize and contained non-ASCII bytes
- ESP32 must still treat it as one rejected GM810 packet
- no chunking fallback exists in v1

## 7. ESP32 implementation requirements

ESP32 implementation must:

- keep the existing project transport pattern: IRQ -> read `type` from `0x00` -> map to fixed `(reg,len)` -> read payload
- add mapping `0x0A -> (reg=0x90, len=16)`
- decode the fixed 16-byte payload exactly as specified above
- accept only `data_len <= 12`
- reject any attempt to interpret v1 packets as chunked or multi-frame
- branch on `flags.error_oversize` and `flags.error_non_ascii`
- treat `chunk_index/chunk_total` as reserved in v1

ESP32 implementation must **not**:

- infer payload length from packet type without sending `[reg,len]`
- read a variable-length GM810 payload
- assume raw source payload can exceed the fixed `16`-byte contract on the wire
- introduce chunked reassembly for v1

## 8. AI-agent task spec for the ESP32 project

Use the following as the implementation brief:

1. Extend the existing STM32 packet-type mapping with `PACKET_QR_GM810 = 0x0A`.
2. Map this type to register `0x90` and fixed read length `16`.
3. Preserve the current transport pattern: after IRQ, first read `0x00/len=1`, then read the mapped payload window.
4. Parse the 16-byte payload as:
   - `byte0=data_len`
   - `byte1=flags`
   - `byte2=chunk_index`
   - `byte3=chunk_total`
   - `byte4..15=data`
5. Enforce v1 assumptions:
   - `data_len <= 12`
   - `chunk_index == 0`
   - `chunk_total == 1`
   - no chunking logic
6. If `flags.error_oversize` or `flags.error_non_ascii` is set, do not treat the packet as a normal QR result.
7. For normal packets, treat `data[0:data_len]` as printable ASCII QR payload.
8. Keep all existing packet handlers unchanged unless required for the new type.

## 9. Test checklist for ESP32 side

- normal ASCII payload with `data_len <= 12`
- oversize payload with `error_oversize = 1`
- non-ASCII payload with `error_non_ascii = 1`
- combined error flags
- repeated IRQ/read cycle with unchanged transport framing
- validation that no chunked path is entered in v1
