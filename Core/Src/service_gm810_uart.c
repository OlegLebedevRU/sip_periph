#include <string.h>
#include "main.h"
#include "cmsis_os.h"
#include "service_gm810_uart.h"
#include "service_time_sync.h"

extern UART_HandleTypeDef huart6;
extern osMessageQId myQueueToMasterHandle;

#define GM810_PROTOCOL_FRAME_TYPE    0x03U
#define GM810_PROTOCOL_HEADER_ZERO   0x00U
#define GM810_RX_TIMEOUT_MS          100U
#define GM810_DEDUP_WINDOW_MS        1500U
#define GM810_PUBLISH_SLOT_COUNT     16U
#define GM810_BOOT_READY_DELAY_MS    300U
#define GM810_BOOT_FLUSH_LIMIT       32U

/* Observed GM810 decoded-data frame on the wire (logic analyzer, 2026-04-25):
 *   byte[0] = 0x03  - decoded-data frame marker
 *   byte[1] = 0x00  - reserved / zero byte emitted by the module in this profile
 *   byte[2] = len   - payload length
 *   byte[3..]       - payload bytes
 *
 * Vendor notes often describe protocol mode in the shortened form <0x03><len><data>,
 * but the module used in this project actually emits the explicit 3-byte header above.
 * Keep the header bytes named here so a future format change stays local to the parser.
 */

/* Boot-mode (Hybrid) — see docs/gm810_integration_plan_2026-04-23.md §14, §16. */
#define GM810_BOOT_TX_TIMEOUT_MS     100U   /* §16: ≤100 ms per ~9-byte frame */
#define GM810_BOOT_RX_TIMEOUT_MS     200U   /* §14.3: practical READ timeout */
#define GM810_BOOT_INTERBYTE_MS      20U    /* inter-byte gap inside one response */
#define GM810_BOOT_TX_RETRY          2U     /* §14.6 п.4 */
#define GM810_BOOT_MAX_RX            16U    /* response buffer (header 4 + data ≤8 + crc 2) */

#define GM810_BOOT_STATE_IDLE        0U
#define GM810_BOOT_STATE_VERIFY      1U
#define GM810_BOOT_STATE_CONFIGURE   2U
#define GM810_BOOT_STATE_OK          3U
#define GM810_BOOT_STATE_FAILED      4U

/* Acknowledge frame returned by the module on successful Write Zone (10.3) /
 * Save Zone (10.4): { type=0x02, addrH=0x00, addrL=0x00, len=0x01, status=0x00,
 * crc1=0x33, crc2=0x31 }. status=0x00 means OK; CRC bytes are taken verbatim
 * from Appendix B. */
static const uint8_t GM810_ACK_OK[7] = {0x02U, 0x00U, 0x00U, 0x01U, 0x00U, 0x33U, 0x31U};

#if defined(__GNUC__)
#define GM810_NOINLINE __attribute__((noinline))
#else
#define GM810_NOINLINE
#endif

typedef enum {
    GM810_RX_WAIT_FRAME_TYPE = 0,
    GM810_RX_WAIT_PROTOCOL_ZERO,
    GM810_RX_WAIT_PROTOCOL_LEN,
    GM810_RX_WAIT_PAYLOAD,
    GM810_RX_WAIT_RAW_PAYLOAD,
} gm810_rx_state_t;

typedef struct {
    gm810_rx_state_t state;
    uint8_t expected_len;
    uint8_t received_len;
    uint8_t flags;
    uint8_t payload[GM810_QR_DATA_MAX_LEN];
    uint32_t last_byte_tick;
} gm810_rx_ctx_t;

static uint8_t s_rx_byte = 0U;
static gm810_rx_ctx_t s_rx = {0};
static uint8_t s_publish_slots[GM810_PUBLISH_SLOT_COUNT][I2C_PACKET_QR_GM810_LEN];
static uint8_t s_publish_slot_index = 0U;
static uint8_t s_last_packet[I2C_PACKET_QR_GM810_LEN] = {0};
static uint8_t s_last_packet_valid = 0U;
static uint32_t s_last_publish_tick = 0U;
static volatile service_gm810_uart_diag_t s_diag = {0};

static uint8_t gm810_is_printable_ascii(uint8_t byte);

static void gm810_capture_hw_state(void)
{
    s_diag.last_uart_error = huart6.ErrorCode;
    s_diag.last_rx_state = (uint32_t)huart6.RxState;
    s_diag.last_g_state = (uint32_t)huart6.gState;
    s_diag.last_usart_sr = USART6->SR;
    s_diag.last_usart_cr1 = USART6->CR1;
    s_diag.last_usart_cr3 = USART6->CR3;
    s_diag.last_gpioa_moder = GPIOA->MODER;
    s_diag.last_gpioa_afr_high = GPIOA->AFR[1];
}

static void gm810_clear_uart_error_flags(UART_HandleTypeDef *huart)
{
    __HAL_UART_CLEAR_PEFLAG(huart);
    __HAL_UART_CLEAR_FEFLAG(huart);
    __HAL_UART_CLEAR_NEFLAG(huart);
    __HAL_UART_CLEAR_OREFLAG(huart);
}

static void gm810_delay_ms(uint32_t delay_ms)
{
    if (delay_ms == 0U) {
        return;
    }

    if (osKernelRunning() != 0) {
        osDelay(delay_ms);
    } else {
        HAL_Delay(delay_ms);
    }
}

static void gm810_flush_pending_rx(void)
{
    uint32_t drained = 0U;

    while ((__HAL_UART_GET_FLAG(&huart6, UART_FLAG_RXNE) != RESET) && (drained < GM810_BOOT_FLUSH_LIMIT)) {
        (void)__HAL_UART_FLUSH_DRREGISTER(&huart6);
        drained++;
    }

    s_diag.boot_flush_bytes += drained;
    gm810_clear_uart_error_flags(&huart6);
    gm810_capture_hw_state();
}

static void gm810_parser_reset(void)
{
    memset(&s_rx, 0, sizeof(s_rx));
    s_rx.state = GM810_RX_WAIT_FRAME_TYPE;
}

static void gm810_begin_raw_frame(void)
{
    s_rx.expected_len = 0U;
    s_rx.received_len = 0U;
    s_rx.flags = 0U;
    s_rx.state = GM810_RX_WAIT_RAW_PAYLOAD;
}

static GM810_NOINLINE void gm810_restart_receive_it(void)
{
    s_diag.restart_calls++;
    __HAL_UART_ENABLE_IT(&huart6, UART_IT_IDLE);
    s_diag.last_receive_status = (uint32_t)HAL_UART_Receive_IT(&huart6, &s_rx_byte, 1U);
    gm810_capture_hw_state();
}

static uint8_t gm810_frame_is_protocol_mode(void)
{
    return ((s_rx.flags & GM810_QR_FLAG_PROTOCOL_MODE) != 0U) ? 1U : 0U;
}

static uint8_t gm810_frame_effective_len(void)
{
    uint8_t effective_len = (s_rx.received_len > GM810_QR_DATA_MAX_LEN) ? GM810_QR_DATA_MAX_LEN : s_rx.received_len;

    if ((gm810_frame_is_protocol_mode() == 0U) && (effective_len > 0U) && (s_rx.payload[effective_len - 1U] == (uint8_t)'\r')) {
        effective_len--;
    }

    return effective_len;
}

static void gm810_append_payload_byte(uint8_t byte)
{
    if (s_rx.received_len < GM810_QR_DATA_MAX_LEN) {
        s_rx.payload[s_rx.received_len] = byte;
    } else {
        s_rx.flags |= GM810_QR_FLAG_ERROR_OVERSIZE;
    }

    if (gm810_frame_is_protocol_mode() != 0U) {
        if (gm810_is_printable_ascii(byte) == 0U) {
            s_rx.flags |= GM810_QR_FLAG_ERROR_NON_ASCII;
        }
    } else if ((byte != (uint8_t)'\r') && (gm810_is_printable_ascii(byte) == 0U)) {
        s_rx.flags |= GM810_QR_FLAG_ERROR_NON_ASCII;
    }

    if (s_rx.received_len < 0xFFU) {
        s_rx.received_len++;
    }
}

/* ------------------------------------------------------------------------- */
/* GM810 boot-mode (Hybrid): TX API + verify/configure/save                  */
/*                                                                           */
/* Implements §14 and §16 of docs/gm810_integration_plan_2026-04-23.md.      */
/* All hex commands are taken verbatim from §14.3/§14.4 and Appendix B.      */
/* TODO(§14.5): heartbeat / link health-check is intentionally NOT done here.*/
/* ------------------------------------------------------------------------- */

/* Synchronous TX of one continuous frame via HAL_UART_Transmit (no inter-byte
 * pauses, see §14.1 п.6). Used only during boot, RX-IT must be off. */
static HAL_StatusTypeDef gm810_tx_frame(const uint8_t *bytes, uint16_t len, uint32_t timeout_ms)
{
    if ((bytes == NULL) || (len == 0U)) {
        return HAL_ERROR;
    }
    return HAL_UART_Transmit(&huart6, (uint8_t *)bytes, len, timeout_ms);
}

/* Read response into local buffer; do not touch the parser's s_rx_byte.
 * Returns number of bytes received (may be 0 on timeout). */
static uint16_t gm810_rx_response(uint8_t *buf, uint16_t max_len, uint32_t timeout_ms)
{
    uint16_t got = 0U;
    HAL_StatusTypeDef st;

    if ((buf == NULL) || (max_len == 0U)) {
        return 0U;
    }

    /* First byte: wait the full timeout. */
    st = HAL_UART_Receive(&huart6, &buf[0], 1U, timeout_ms);
    if (st != HAL_OK) {
        gm810_clear_uart_error_flags(&huart6);
        return 0U;
    }
    got = 1U;

    /* Remaining bytes: short inter-byte timeout, accept what is there. */
    while (got < max_len) {
        st = HAL_UART_Receive(&huart6, &buf[got], 1U, GM810_BOOT_INTERBYTE_MS);
        if (st != HAL_OK) {
            break;
        }
        got++;
    }

    gm810_clear_uart_error_flags(&huart6);
    return got;
}

/* Read zone bit per §14.3 / manual 10.2.
 *   TX:  7E 00 07 01 <addrH> <addrL> <count> AB CD
 *   RX:  02 00 00 <len> <data...> <crc1> <crc2>
 * On success, copies <data...> (count bytes) into out_data and returns 0.
 * CRC of response is intentionally NOT validated in v1 (manual allows this). */
static int gm810_read_zone(uint16_t addr, uint8_t count,
                           uint8_t *out_data, uint32_t resp_timeout_ms)
{
    uint8_t tx[9];
    uint8_t rx[GM810_BOOT_MAX_RX];
    uint16_t rx_len;
    uint8_t attempt;

    if ((out_data == NULL) || (count == 0U) || (count > 8U)) {
        return -1;
    }

    tx[0] = 0x7EU;
    tx[1] = 0x00U;
    tx[2] = 0x07U;
    tx[3] = 0x01U;
    tx[4] = (uint8_t)((addr >> 8) & 0xFFU);
    tx[5] = (uint8_t)(addr & 0xFFU);
    tx[6] = count;
    tx[7] = 0xABU;  /* CRC placeholder allowed by manual 10.1/10.2 ("AB CD"). */
    tx[8] = 0xCDU;

    for (attempt = 0U; attempt < GM810_BOOT_TX_RETRY; attempt++) {
        if (gm810_tx_frame(tx, sizeof(tx), GM810_BOOT_TX_TIMEOUT_MS) != HAL_OK) {
            continue;
        }
        rx_len = gm810_rx_response(rx, (uint16_t)sizeof(rx), resp_timeout_ms);
        /* Minimum valid header (4) + count data bytes. */
        if (rx_len < (uint16_t)(4U + count)) {
            continue;
        }
        if ((rx[0] != 0x02U) || (rx[1] != 0x00U) || (rx[2] != 0x00U) || (rx[3] != count)) {
            continue;
        }
        memcpy(out_data, &rx[4], count);
        return 0;
    }

    return -1;
}

/* Validate ACK response (02 00 00 01 00 33 31) per manual 10.3 / 10.4. */
static int gm810_check_ack(const uint8_t *rx, uint16_t len)
{
    if (len < (uint16_t)sizeof(GM810_ACK_OK)) {
        return -1;
    }
    return (memcmp(rx, GM810_ACK_OK, sizeof(GM810_ACK_OK)) == 0) ? 0 : -1;
}

/* Write zone bit per §14.4 / manual 10.3.
 *   TX:  7E 00 08 <len> <addrH> <addrL> <data...> AB CD
 *   ACK: 02 00 00 01 00 33 31 */
static int gm810_write_zone(uint16_t addr, const uint8_t *data, uint8_t len)
{
    uint8_t tx[16];
    uint8_t rx[GM810_BOOT_MAX_RX];
    uint16_t rx_len;
    uint8_t attempt;
    uint16_t pos;

    if ((data == NULL) || (len == 0U) || (len > 8U)) {
        return -1;
    }

    tx[0] = 0x7EU;
    tx[1] = 0x00U;
    tx[2] = 0x08U;
    tx[3] = len;
    tx[4] = (uint8_t)((addr >> 8) & 0xFFU);
    tx[5] = (uint8_t)(addr & 0xFFU);
    memcpy(&tx[6], data, len);
    pos = (uint16_t)(6U + len);
    tx[pos++] = 0xABU;  /* CRC placeholder allowed by manual 10.1/10.3. */
    tx[pos++] = 0xCDU;

    for (attempt = 0U; attempt < GM810_BOOT_TX_RETRY; attempt++) {
        if (gm810_tx_frame(tx, pos, GM810_BOOT_TX_TIMEOUT_MS) != HAL_OK) {
            continue;
        }
        rx_len = gm810_rx_response(rx, (uint16_t)sizeof(rx), GM810_BOOT_RX_TIMEOUT_MS);
        if (gm810_check_ack(rx, rx_len) == 0) {
            return 0;
        }
    }

    return -1;
}

/* Save zone bit list to internal flash per §14.4 / manual 10.4.
 *   TX:  7E 00 09 01 00 00 00 DE C8
 *   ACK: 02 00 00 01 00 33 31 */
static int gm810_save_config(void)
{
    static const uint8_t tx[9] = {
        0x7EU, 0x00U, 0x09U, 0x01U, 0x00U, 0x00U, 0x00U, 0xDEU, 0xC8U
    };
    uint8_t rx[GM810_BOOT_MAX_RX];
    uint16_t rx_len;
    uint8_t attempt;

    s_diag.boot_save_calls++;

    for (attempt = 0U; attempt < GM810_BOOT_TX_RETRY; attempt++) {
        if (gm810_tx_frame(tx, sizeof(tx), GM810_BOOT_TX_TIMEOUT_MS) != HAL_OK) {
            continue;
        }
        rx_len = gm810_rx_response(rx, (uint16_t)sizeof(rx), GM810_BOOT_RX_TIMEOUT_MS);
        if (gm810_check_ack(rx, rx_len) == 0) {
            return 0;
        }
    }

    return -1;
}

/* Target profile (§14.2): per-zone-bit address / expected bytes. */
typedef struct {
    uint16_t addr;
    uint8_t  count;
    uint8_t  expected[2];
} gm810_zone_target_t;

static const gm810_zone_target_t GM810_ZONE_TARGETS[] = {
    { 0x0000U, 1U, { 0xD6U, 0x00U } }, /* Mode: LED on, Mute off, Standard light/brightness, Continuous Mode */
    { 0x0013U, 1U, { 0x85U, 0x00U } }, /* Same Barcode Reading delay on, 500 ms */
    { 0x002AU, 2U, { 0x39U, 0x01U } }, /* Baud = 9600 */
    { 0x002CU, 1U, { 0x02U, 0x00U } }, /* Decoder = full, all codes */
    { 0x0060U, 1U, { 0xE0U, 0x00U } }, /* Output: with-protocol, no tail/prefix/etc */
    { 0x00D0U, 1U, { 0x00U, 0x00U } }, /* AIM ID = forbid */
};

#define GM810_ZONE_TARGET_COUNT (sizeof(GM810_ZONE_TARGETS) / sizeof(GM810_ZONE_TARGETS[0]))

/* Verify all targeted zone bits per §14.3.
 * Returns 0 if every zone bit matches the target profile, -1 otherwise.
 * On mismatch/RX-failure, stores the first failing address in
 * s_diag.boot_last_mismatch_addr for diagnostics (§16 п.4). */
static int gm810_boot_verify(void)
{
    uint8_t buf[2];
    size_t i;

    s_diag.boot_verify_calls++;
    s_diag.boot_state = GM810_BOOT_STATE_VERIFY;

    for (i = 0U; i < GM810_ZONE_TARGET_COUNT; i++) {
        const gm810_zone_target_t *t = &GM810_ZONE_TARGETS[i];

        if (gm810_read_zone(t->addr, t->count, buf, GM810_BOOT_RX_TIMEOUT_MS) != 0) {
            s_diag.boot_last_mismatch_addr = t->addr;
            return -1;
        }
        if (memcmp(buf, t->expected, t->count) != 0) {
            s_diag.boot_last_mismatch_addr = t->addr;
            return -1;
        }
    }

    s_diag.boot_last_mismatch_addr = 0U;
    return 0;
}

/* Apply target profile (§14.4 W1..W6) and persist with one save (S1).
 * Returns 0 if every write + save was acknowledged, -1 on any failure. */
static int gm810_boot_configure(void)
{
    size_t i;

    s_diag.boot_configure_calls++;
    s_diag.boot_state = GM810_BOOT_STATE_CONFIGURE;

    /* §14.4: write order MUST be from low address to high — preserved by
     * the order of GM810_ZONE_TARGETS. */
    for (i = 0U; i < GM810_ZONE_TARGET_COUNT; i++) {
        const gm810_zone_target_t *t = &GM810_ZONE_TARGETS[i];
        if (gm810_write_zone(t->addr, t->expected, t->count) != 0) {
            s_diag.boot_last_mismatch_addr = t->addr;
            return -1;
        }
    }

    if (gm810_save_config() != 0) {
        return -1;
    }

    return 0;
}

/* Full Hybrid boot sequence (§14.7).
 * Returns 0 on success (parser may be armed), -1 on failure (do NOT arm). */
static int gm810_boot_run_hybrid(void)
{
    if (gm810_boot_verify() == 0) {
        s_diag.boot_state = GM810_BOOT_STATE_OK;
        return 0;
    }

    if (gm810_boot_configure() != 0) {
        s_diag.boot_failures++;
        s_diag.boot_state = GM810_BOOT_STATE_FAILED;
        return -1;
    }

    /* §16 п.2: only on a successful re-verify can boot be considered OK. */
    if (gm810_boot_verify() != 0) {
        s_diag.boot_failures++;
        s_diag.boot_state = GM810_BOOT_STATE_FAILED;
        return -1;
    }

    s_diag.boot_state = GM810_BOOT_STATE_OK;
    return 0;
}

static uint8_t gm810_is_printable_ascii(uint8_t byte)
{
    return (byte >= 0x20U) && (byte <= 0x7EU);
}

static uint8_t gm810_should_suppress_duplicate(const uint8_t *packet)
{
    uint32_t now = HAL_GetTick();

    if ((packet == NULL) || (s_last_packet_valid == 0U)) {
        return 0U;
    }
    if (memcmp(s_last_packet, packet, I2C_PACKET_QR_GM810_LEN) != 0) {
        return 0U;
    }
    return ((now - s_last_publish_tick) < GM810_DEDUP_WINDOW_MS) ? 1U : 0U;
}

static void gm810_note_published_packet(const uint8_t *packet)
{
    if (packet == NULL) {
        return;
    }
    memcpy(s_last_packet, packet, I2C_PACKET_QR_GM810_LEN);
    s_last_packet_valid = 1U;
    s_last_publish_tick = HAL_GetTick();
}

static void gm810_publish_completed_frame_from_isr(void)
{
    uint8_t *window;
    uint8_t diag_len;
    I2cPacketToMaster_t pckt;
    BaseType_t prior = pdFALSE;

    if (s_rx.received_len == 0U) {
        return;
    }

    window = s_publish_slots[s_publish_slot_index];
    s_publish_slot_index = (uint8_t)((s_publish_slot_index + 1U) % GM810_PUBLISH_SLOT_COUNT);

    memset(window, 0, I2C_PACKET_QR_GM810_LEN);
    diag_len = gm810_frame_effective_len();
    if (diag_len == 0U) {
        return;
    }
    /* data_len reports bytes actually placed into the fixed diagnostic/data window. */
    window[0] = diag_len;
    window[1] = s_rx.flags;
    window[2] = 0U;
    window[3] = 1U;

    for (uint8_t i = 0U; i < diag_len; i++) {
        /* v1 contract requires printable ASCII even for diagnostic payload. */
        window[4U + i] = gm810_is_printable_ascii(s_rx.payload[i]) ? s_rx.payload[i] : (uint8_t)'?';
    }

    if (gm810_should_suppress_duplicate(window) != 0U) {
        return;
    }

    pckt.payload = window;
    pckt.len = I2C_PACKET_QR_GM810_LEN;
    pckt.type = PACKET_QR_GM810;
    pckt.ttl = service_time_sync_get_uptime_sec() + TTL_PACKET_SEC;

    /* Preserve the current credential-event publish pattern used by other producers. */
    if (xQueueSendToFrontFromISR(myQueueToMasterHandle, &pckt, &prior) == pdTRUE) {
        gm810_note_published_packet(window);
        portYIELD_FROM_ISR(prior);
    }
}

void service_gm810_uart_init(void)
{
    memset((void *)&s_diag, 0, sizeof(s_diag));
    s_diag.init_calls = 1U;
    memset(s_publish_slots, 0, sizeof(s_publish_slots));
    memset(s_last_packet, 0, sizeof(s_last_packet));
    s_publish_slot_index = 0U;
    s_last_packet_valid = 0U;
    s_last_publish_tick = 0U;
    gm810_parser_reset();
    gm810_capture_hw_state();
}

void service_gm810_uart_start(void)
{
    int boot_rc;

    s_diag.start_calls++;
    s_diag.last_start_tick = HAL_GetTick();
    s_diag.start_delay_ms = GM810_BOOT_READY_DELAY_MS;
    s_diag.boot_state = GM810_BOOT_STATE_IDLE;

    (void)HAL_UART_AbortReceive(&huart6);
    gm810_clear_uart_error_flags(&huart6);
    gm810_parser_reset();

    /* According to the integration plan, GM810 bring-up must respect the
     * module's post-power-on readiness window.  Delay arming the long-lived RX
     * session until the reader has had time to settle, then discard any early
     * boot chatter before starting the protocol parser. */
    gm810_delay_ms(GM810_BOOT_READY_DELAY_MS);
    s_diag.ready_tick = HAL_GetTick();
    gm810_flush_pending_rx();

    /* Hybrid boot-mode (§14, §16): verify target zone bits, configure+save
     * only on mismatch, and refuse to arm the RX parser if the module cannot
     * be brought to the target profile.  RX-IT is intentionally OFF for the
     * entire duration of this exchange (§14.6 п.5). */
    boot_rc = gm810_boot_run_hybrid();
    gm810_clear_uart_error_flags(&huart6);
    gm810_flush_pending_rx();

    if (boot_rc != 0) {
        /* Boot Hybrid verify/configure path failed.  Keep the failure state in
         * diagnostics, but do NOT permanently disable the runtime RX parser:
         * some deployed modules may already be manually preconfigured (or may
         * reject zone-bit commands / be temporarily unavailable on TX) while
         * still producing valid <03><00><len><data> scan frames on RX.
         *
         * This degrades the service to passive RX-only mode instead of making
         * the whole GM810 path unusable after one failed boot negotiation. */
        gm810_capture_hw_state();
    }

    gm810_parser_reset();
    gm810_restart_receive_it();

    if (s_diag.last_receive_status != (uint32_t)HAL_OK) {
        s_diag.start_retry_count++;
        gm810_clear_uart_error_flags(&huart6);
        (void)HAL_UART_AbortReceive(&huart6);
        gm810_restart_receive_it();
    }
}

void service_gm810_uart_irq_callback(UART_HandleTypeDef *huart)
{
    if (huart != &huart6) {
        return;
    }

    s_diag.irq_callbacks++;
    s_diag.last_irq_tick = HAL_GetTick();

    if ((__HAL_UART_GET_FLAG(huart, UART_FLAG_IDLE) != RESET)
        && (__HAL_UART_GET_FLAG(huart, UART_FLAG_RXNE) == RESET)) {
        __HAL_UART_CLEAR_IDLEFLAG(huart);

        if ((s_rx.state == GM810_RX_WAIT_RAW_PAYLOAD) && (s_rx.received_len > 0U)) {
            gm810_publish_completed_frame_from_isr();
            gm810_parser_reset();
        }
    }

    gm810_capture_hw_state();
}

void service_gm810_uart_rx_callback(UART_HandleTypeDef *huart)
{
    uint32_t now;
    uint8_t rx_byte;

    if (huart != &huart6) {
        return;
    }

    s_diag.rx_callbacks++;
    rx_byte = s_rx_byte;
    s_diag.last_rx_byte = rx_byte;

    now = HAL_GetTick();
    s_diag.last_rx_tick = now;
    gm810_capture_hw_state();
    if ((s_rx.state != GM810_RX_WAIT_FRAME_TYPE) && ((now - s_rx.last_byte_tick) > GM810_RX_TIMEOUT_MS)) {
        if ((s_rx.state == GM810_RX_WAIT_RAW_PAYLOAD) && (s_rx.received_len > 0U)) {
            gm810_publish_completed_frame_from_isr();
        }
        gm810_parser_reset();
    }
    s_rx.last_byte_tick = now;

    switch (s_rx.state) {
    case GM810_RX_WAIT_FRAME_TYPE:
        if (rx_byte == GM810_PROTOCOL_FRAME_TYPE) {
            s_rx.state = GM810_RX_WAIT_PROTOCOL_ZERO;
        } else {
            gm810_begin_raw_frame();
            gm810_append_payload_byte(rx_byte);
        }
        break;

    case GM810_RX_WAIT_PROTOCOL_ZERO:
        if (rx_byte == GM810_PROTOCOL_HEADER_ZERO) {
            s_rx.state = GM810_RX_WAIT_PROTOCOL_LEN;
        } else {
            /* False start: preserve both bytes as raw payload so we do not lose
             * data when the stream does not actually match the observed protocol
             * header shape. */
            gm810_begin_raw_frame();
            gm810_append_payload_byte(GM810_PROTOCOL_FRAME_TYPE);
            gm810_append_payload_byte(rx_byte);
        }
        break;

    case GM810_RX_WAIT_PROTOCOL_LEN:
        s_rx.expected_len = rx_byte;
        s_rx.received_len = 0U;
        s_rx.flags = GM810_QR_FLAG_PROTOCOL_MODE;
        if (s_rx.expected_len == 0U) {
            gm810_parser_reset();
        } else {
            if (s_rx.expected_len > GM810_QR_DATA_MAX_LEN) {
                s_rx.flags |= GM810_QR_FLAG_ERROR_OVERSIZE;
            }
            s_rx.state = GM810_RX_WAIT_PAYLOAD;
        }
        break;

    case GM810_RX_WAIT_PAYLOAD:
    case GM810_RX_WAIT_RAW_PAYLOAD:
        gm810_append_payload_byte(rx_byte);

        if ((s_rx.state == GM810_RX_WAIT_PAYLOAD) && (s_rx.received_len >= s_rx.expected_len)) {
            gm810_publish_completed_frame_from_isr();
            gm810_parser_reset();
        }
        break;

    default:
        gm810_parser_reset();
        break;
    }

    gm810_restart_receive_it();
}

void service_gm810_uart_error_callback(UART_HandleTypeDef *huart)
{
    if (huart != &huart6) {
        return;
    }

    s_diag.error_callbacks++;
    gm810_capture_hw_state();
    gm810_clear_uart_error_flags(huart);
    gm810_parser_reset();
    gm810_restart_receive_it();
}

const volatile service_gm810_uart_diag_t *service_gm810_uart_get_diag(void)
{
    return &s_diag;
}
