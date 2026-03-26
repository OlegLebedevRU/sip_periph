# DWIN TX Stage 3 Plan

Date: 2026-03-26

## Goal

Safely unify all DWIN TX traffic on `USART2` so that `hmi.c`, `dwin_gfx.c`, and external callers do not compete for the same UART with separate buffers and separate `HAL_UART_Transmit_IT(...)` paths.

Priority order:
1. Do not break current HMI behavior.
2. Keep public API `dwin_text_output(...)` stable.
3. Remove parallel TX paths for the same `huart2`.

---

## Current call map

### Public text TX path
- `Core/Src/hmi.c`
  - implements `dwin_text_output(...)`
  - builds a DWIN text frame and sends it through `HAL_UART_Transmit_IT(&huart2, ...)`

### Direct `dwin_text_output(...)` call sites
- `Core/Src/hmi.c`
  - `hmi_clear_display_if_unlocked()`
  - `hmi_process_message_events()`
  - `hmi_show_auth_result()`
  - `StartTaskHmi()`
  - `StartTaskHmiMsg()` for host message
  - `StartTaskHmiMsg()` for diagnostic rotation
- `Core/Src/app_i2c_slave.c`
  - `StartTaskRxTxI2c1()` displays `"BUS ERROR"` through `dwin_text_output(0x5200, ...)`

### Separate competing TX path
- `Core/Src/dwin_gfx.c`
  - `gfx_send()` waits for `huart2.gState == HAL_UART_STATE_READY`
  - then directly calls `HAL_UART_Transmit_IT(&huart2, s_gfx_buf, len)`
  - uses its own private buffer, separate from `hmi.c`

### Same UART RX path to preserve
- `Core/Src/app_uart_dwin.c`
  - owns DWIN RX FSM on the same `huart2`
  - `dwin_uart_start()`
  - `app_uart_dwin_rx_callback()`

---

## Important observations

1. `dwin_text_output(...)` is no longer called from timer callback context.
   - `cb_Hmi_Pin_Timeout()` and `cb_Hmi_Ttl()` now only raise pending events.
   - actual display updates happen in task context.

2. There are still two independent TX implementations on the same `USART2`.
   - `hmi.c` has its own mutex and text buffer
   - `dwin_gfx.c` has its own wait logic and gfx buffer

3. `app_i2c_slave.c` is already clean from a layering perspective.
   - it uses `dwin_text_output(...)`
   - this call site should stay unchanged if possible

---

## Main risks before implementation

### 1) Concurrent TX on the same UART
Current risk:
- `hmi.c` serializes only its own text path
- `dwin_gfx.c` bypasses that serialization completely

Effect:
- interleaved or dropped DWIN frames
- `HAL_BUSY` / unstable UI updates

### 2) Shared buffer lifetime with IT-based transmit
`HAL_UART_Transmit_IT(...)` is asynchronous.
If a common buffer is introduced incorrectly, it may be overwritten before TX completion.

### 3) API breakage
`dwin_text_output(...)` is already used outside `hmi.c`.
Changing its signature or expected behavior would create unnecessary blast radius.

### 4) RX/TX coupling on the same `huart2`
Refactor must not interfere with:
- `dwin_uart_start()`
- `app_uart_dwin_rx_callback()`
- current DWIN receive FSM lifecycle

---

## Target architecture

Introduce one shared DWIN TX transport module responsible for all writes to `huart2`.

Suggested files:
- `Core/Inc/app_uart_dwin_tx.h`
- `Core/Src/app_uart_dwin_tx.c`

### Responsibilities of the transport
- own the common TX mutex
- own the common TX buffer(s)
- centralize waiting for `huart2.gState`
- provide one function to send an already built frame
- optionally provide helper(s) for DWIN frame construction later

### Responsibilities kept outside the transport
- business logic in `hmi.c`
- graphics command building in `dwin_gfx.c`
- error/reporting decisions in `app_i2c_slave.c`

### API stability rule
- `dwin_text_output(...)` stays public and keeps the same signature
- internally it delegates to the new TX transport

---

## Recommended implementation steps

### Step 1 — Introduce shared TX transport
Create a small transport API for `USART2` TX, for example:
- send prepared frame
- optionally send with bounded wait/retry

Keep it minimal in stage 3.
Do not change RX logic in `app_uart_dwin.c`.

### Step 2 — Move TX serialization out of `hmi.c`
Move these concerns from `hmi.c` into the shared transport:
- mutex ownership
- `huart2.gState` wait loop
- actual `HAL_UART_Transmit_IT(...)`
- buffer ownership policy

### Step 3 — Keep `dwin_text_output(...)` as facade
In `Core/Src/hmi.c`:
- keep `dwin_text_output(...)`
- keep frame-building logic for now
- replace direct UART TX with a call to the shared transport

This minimizes behavioral change.

### Step 4 — Convert `dwin_gfx.c`
Replace direct TX in `gfx_send()` so it uses the same transport.
Goal:
- no direct `HAL_UART_Transmit_IT(&huart2, ...)` remains in `dwin_gfx.c`
- no private serialization path remains there

### Step 5 — Keep external call sites unchanged
Leave `Core/Src/app_i2c_slave.c` as-is.
It should continue to call `dwin_text_output(...)`.
That preserves behavior and reduces risk.

### Step 6 — Recheck all DWIN TX ownership
After refactor, verify:
- all DWIN writes on `USART2` go through the same transport
- no second mutex/buffer path still exists
- no direct `HAL_UART_Transmit_IT(&huart2, ...)` remains except inside the shared transport

### Step 7 — Validate HMI behavior
Recheck these flows:
- PIN typing and echo on display
- auth result display
- host message display with TTL
- diagnostic rotation
- `BUS ERROR` message from I2C task

---

## Edge cases to verify

1. Text update and gfx update happen nearly at the same time.
2. `BUS ERROR` display occurs while HMI is updating PIN or diagnostics.
3. Long text is truncated safely without buffer overrun.
4. `dwin_text_output(..., elen = 0)` still works through `strlen`.
5. Empty / NULL input does not crash or corrupt frame state.
6. Repeated quick sends do not corrupt the common TX buffer.
7. DWIN input RX still works after TX refactor on the same `huart2`.

---

## Definition of done for stage 3

- All DWIN TX on `USART2` uses one shared transport.
- `dwin_text_output(...)` keeps its current public signature.
- `dwin_gfx.c` no longer directly calls `HAL_UART_Transmit_IT(&huart2, ...)`.
- No duplicate mutex/buffer ownership remains for the same UART TX path.
- Existing HMI behavior remains unchanged from user perspective.
- Build passes for the touched files at minimum.

---

## Notes for implementation

### Low-risk option for stage 3
Use the same current design style:
- mutex
- short bounded polling on `huart2.gState`
- IT-based transmit

This is consistent with the current project and avoids a bigger jump to a full TX queue owner.

### Deferred improvement for later
A future stage could move to a stricter single-owner TX task or completion-driven transport using TX-complete callbacks.
That is useful, but not required to safely complete stage 3.
