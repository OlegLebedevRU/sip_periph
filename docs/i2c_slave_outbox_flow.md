# I2C slave outbox flow

## Overview

This document describes the **slave-initiated** delivery flow implemented in `Core/Src/app_i2c_slave.c`.

The important point is that delivery is tracked as **two logical master read operations**, not as one single hardware I2C transaction:

1. the master reads `ram[0x00]` (`I2C_PACKET_TYPE_ADDR`) to learn what packet is pending;
2. the master then reads the actual payload register for that packet type.

The packet is considered delivered **only after the payload register is read successfully**.
A read of `ram[0x00]` does **not** acknowledge delivery.

## Publish phase

When `app_i2c_slave_publish()` is called, STM32 prepares an outbox packet:

1. Writes the packet type into `s_ram[I2C_PACKET_TYPE_ADDR]`;
2. Resolves the target payload register with `packet_reg_for_type()`;
3. Resolves the expected payload length with `packet_len_for_type()`;
4. Copies the payload into `s_ram[target_reg]`;
5. Sets:
   - `s_outbox_busy = 1`
   - `s_outbox_type = packet type`
   - `s_outbox_reg = target payload register`
   - `s_outbox_len = expected payload length`
6. Clears `s_last_tx_reg` / `s_last_tx_len`;
7. Pulls the EVENT line active (`GPIO_PIN_RESET`) to notify the master.

So after publish, the slave has a pending packet and is waiting for the master to fetch it.

## Read handshake before any slave transmit

Before the slave returns data, the master first performs a small write that prepares the read request.

This is handled by the FSM in `s_i2c_sec_ctrl_h`:

- `offset`
- `rx_count`
- `first`
- `second`
- `final`

### Step 1: master writes register offset

In `app_i2c_slave_addr_callback()` with `I2C_DIRECTION_TRANSMIT`, the slave starts receiving the first header byte into `offset`.

Then `app_i2c_slave_rx_complete()`:

- clears `first`
- sets `second`
- stores `last_base_ram_rcvd_addr = offset`
- validates that the register is known
- starts receiving the second header byte into `rx_count`

### Step 2: master writes requested length

On the next `app_i2c_slave_rx_complete()` call:

- clears `second`
- sets `final = 1`
- validates length and address range

After that, one of two things happens:

- if `rx_count == 0`, this is treated as a zero-length prepared request and the watchdog is simply cleared;
- if `rx_count > 0`, the slave either starts payload reception (for a master write flow) or waits for the next read address phase (for a master read flow).

For the slave-initiated outbox flow, this header write is what prepares the subsequent master read.

## Full slave-initiated sequence

### 1. STM32 publishes a packet

`app_i2c_slave_publish()` fills the outbox state and asserts the EVENT line.

### 2. Master prepares a read for `ram[0x00]`

The master writes a two-byte header:

- `offset = I2C_PACKET_TYPE_ADDR`
- `len = 1` (or another valid read length starting at `0x00`)

This moves the FSM into the prepared state:

- `first = 0`
- `second = 0`
- `final = 1`

### 3. Master reads `ram[0x00]`

In `app_i2c_slave_addr_callback()` with read direction:

- the slave checks that:
  - `first == 0`
  - `second == 0`
  - `final == 1`
  - `validate_read_request(offset, rx_count) != 0`
- then starts `HAL_I2C_Slave_Seq_Transmit_IT()` from `&s_ram[offset]`
- stores:
  - `s_last_tx_reg = offset`
  - `s_last_tx_len = rx_count`

So this read exposes the current packet type to the master.

### 4. `app_i2c_slave_tx_complete()` runs after the type read

When the transmit completes, `app_i2c_slave_tx_complete()` does this:

1. verifies that callback belongs to `hi2c1`;
2. clears `s_i2c_sec_ctrl_h.final`;
3. clears the progress watchdog;
4. checks whether the just-finished transmit is exactly the pending outbox payload.

The ACK condition is:

- `s_outbox_busy != 0U`
- `s_last_tx_reg == s_outbox_reg`
- `s_last_tx_len >= s_outbox_len`

After the type read, this condition is **not** true, because:

- `s_last_tx_reg == I2C_PACKET_TYPE_ADDR` (`0x00`)
- `s_outbox_reg` points to the actual payload register for the packet type

So `outbox_complete_ack()` is **not** called yet.
The outbox remains pending, which is the intended behavior.

### 5. Master prepares a second read for the payload register

The master again writes a two-byte header:

- `offset = payload register`
- `len = payload length`

The FSM is prepared again with `final = 1`.

### 6. Master reads the payload register

The slave transmits the payload from `s_ram[payload_reg]` and records:

- `s_last_tx_reg = payload register`
- `s_last_tx_len = transmitted length`

### 7. `app_i2c_slave_tx_complete()` acknowledges delivery

Now the completion condition matches the outbox state:

- outbox is still busy;
- transmitted register matches `s_outbox_reg`;
- transmitted length is at least `s_outbox_len`.

So `outbox_complete_ack()` is called.

## What `outbox_complete_ack()` means

`outbox_complete_ack()` marks the published packet as delivered and releases the notification line:

- clears `s_outbox_busy`
- resets `s_outbox_type`
- resets `s_outbox_reg`
- resets `s_outbox_len`
- resets `s_last_tx_reg`
- resets `s_last_tx_len`
- writes `PACKET_NULL` into `s_ram[I2C_PACKET_TYPE_ADDR]`
- calls `force_idle_event_line()` to release the EVENT pin

So the packet is no longer advertised as pending after the payload read completes.

## Why reading only `ram[0x00]` is not enough

This is an intentional part of the contract.

Reading `ram[0x00]` serves only to discover **what kind of packet** is pending.
It does not prove that the master fetched the actual payload bytes.

Because of that, the implementation requires a second read of the real payload register before it considers the outbox delivered.

## Behavior on `HAL_I2C_ERROR_AF`

`app_i2c_slave_error()` treats `HAL_I2C_ERROR_AF` specially.

If all of the following are true:

- `s_last_tx_reg != 0xFFU`
- `s_last_tx_len > 0U`
- `s_i2c_sec_ctrl_h.final != 0U`

then the code interprets AF as a normal read termination / NACK at the end of the transfer:

- clears `final`
- clears the progress watchdog
- calls `restart_listen_if_needed()`
- returns without hard recovery

Otherwise AF is treated as a malformed flow and recovery is scheduled.

This means a normal master NACK at the end of a read should not be treated as a bus failure.

## Completion rule summary

The slave-initiated outbox contract is therefore:

1. STM32 publishes packet and asserts EVENT;
2. master writes header for `I2C_PACKET_TYPE_ADDR`;
3. master reads `ram[0x00]` to get packet type;
4. master writes header for the payload register;
5. master reads payload;
6. only after payload transmit completion does `outbox_complete_ack()` clear the outbox.

## Example: `TIME` packet in this flow

`TIME` follows exactly the same outbox contract, but in this project its source is explicit:

- `service_time_sync_on_tick()` reads current RTC time from DS3231;
- copies the raw 7-byte BCD time into `ram[0x60]`;
- creates `I2cPacketToMaster_t` with:
  - `type = PACKET_TIME`
  - `len = I2C_TIME_SYNC_WRITE_LEN`
  - `payload = s_hw_time`
- pushes that packet into `myQueueToMasterHandle`.

Then `StartTaskRxTxI2c1()` receives the packet from the queue and calls `app_i2c_slave_publish()`.

Inside `app_i2c_slave_publish()`:

- `packet_reg_for_type(PACKET_TIME)` resolves to `I2C_REG_HW_TIME_ADDR`;
- `packet_len_for_type(PACKET_TIME, pckt->len)` resolves to `I2C_PACKET_TIME_LEN`;
- `s_ram[I2C_PACKET_TYPE_ADDR]` is set to `PACKET_TIME`;
- the payload is copied into `s_ram[I2C_REG_HW_TIME_ADDR]`;
- outbox state is armed:
  - `s_outbox_busy = 1`
  - `s_outbox_type = PACKET_TIME`
  - `s_outbox_reg = I2C_REG_HW_TIME_ADDR`
  - `s_outbox_len = I2C_PACKET_TIME_LEN`
- the EVENT line is asserted.

### `TIME` read sequence

#### 1. Master reads packet type

The master first prepares a read for `I2C_PACKET_TYPE_ADDR` and then reads `ram[0x00]`.

The returned byte is `PACKET_TIME`.

After this transfer:

- `s_last_tx_reg = I2C_PACKET_TYPE_ADDR`
- `s_last_tx_len = 1`

When `app_i2c_slave_tx_complete()` runs, the outbox is **not** acknowledged yet, because:

- `s_last_tx_reg == I2C_PACKET_TYPE_ADDR`
- `s_outbox_reg == I2C_REG_HW_TIME_ADDR`

So the `TIME` packet remains pending.

#### 2. Master reads `TIME` payload

Next the master prepares a second read for `I2C_REG_HW_TIME_ADDR` with the expected time payload length.

The slave then transmits the content of `s_ram[I2C_REG_HW_TIME_ADDR]`, which was filled from `s_hw_time`.

After this transfer:

- `s_last_tx_reg = I2C_REG_HW_TIME_ADDR`
- `s_last_tx_len = transmitted TIME payload length`

Now `app_i2c_slave_tx_complete()` sees that:

- `s_outbox_busy != 0U`
- `s_last_tx_reg == s_outbox_reg`
- `s_last_tx_len >= s_outbox_len`

So `outbox_complete_ack()` is called, which clears the outbox and releases the EVENT line.

### Practical meaning for `TIME`

For `TIME`, successful delivery means:

1. DS3231 time was sampled and published as `PACKET_TIME`;
2. the master read `ram[0x00]` and discovered that the pending packet type is `TIME`;
3. the master performed a second read from `I2C_REG_HW_TIME_ADDR`;
4. only that second read is treated as actual delivery of the `TIME` packet.

So if the master stops after reading only `ram[0x00]`, the `TIME` outbox remains pending by design.

## Practical conclusion

The implementation explicitly supports the scenario where the master must:

1. first read `ram[0x00]`;
2. then read the main packet register.

And only the second step is treated as successful packet delivery.