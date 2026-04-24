#include <string.h>
#include "main.h"
#include "cmsis_os.h"
#include "service_gm810_uart.h"
#include "service_time_sync.h"

extern UART_HandleTypeDef huart6;
extern osMessageQId myQueueToMasterHandle;

#define GM810_FRAME_HEADER_BYTE      0x03U
#define GM810_RX_TIMEOUT_MS          100U
#define GM810_DEDUP_WINDOW_MS        1500U
#define GM810_PUBLISH_SLOT_COUNT     16U

#if defined(__GNUC__)
#define GM810_NOINLINE __attribute__((noinline))
#else
#define GM810_NOINLINE
#endif

typedef enum {
    GM810_RX_WAIT_HEADER = 0,
    GM810_RX_WAIT_LEN,
    GM810_RX_WAIT_PAYLOAD,
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

static void gm810_parser_reset(void)
{
    memset(&s_rx, 0, sizeof(s_rx));
    s_rx.state = GM810_RX_WAIT_HEADER;
}

static GM810_NOINLINE void gm810_restart_receive_it(void)
{
    s_diag.restart_calls++;
    s_diag.last_uart_error = huart6.ErrorCode;
    s_diag.last_receive_status = (uint32_t)HAL_UART_Receive_IT(&huart6, &s_rx_byte, 1U);
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

    if (s_rx.expected_len == 0U) {
        return;
    }

    window = s_publish_slots[s_publish_slot_index];
    s_publish_slot_index = (uint8_t)((s_publish_slot_index + 1U) % GM810_PUBLISH_SLOT_COUNT);

    memset(window, 0, I2C_PACKET_QR_GM810_LEN);
    /* data_len reports bytes actually placed into the fixed diagnostic/data window. */
    window[0] = (s_rx.received_len > GM810_QR_DATA_MAX_LEN) ? GM810_QR_DATA_MAX_LEN : s_rx.received_len;
    window[1] = (uint8_t)(GM810_QR_FLAG_PROTOCOL_MODE | s_rx.flags);
    window[2] = 0U;
    window[3] = 1U;

    diag_len = window[0];
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
}

void service_gm810_uart_start(void)
{
    s_diag.start_calls++;
    s_diag.last_uart_error = huart6.ErrorCode;
    gm810_parser_reset();
    gm810_restart_receive_it();
}

void service_gm810_uart_rx_callback(UART_HandleTypeDef *huart)
{
    uint32_t now;

    if (huart != &huart6) {
        return;
    }

    s_diag.rx_callbacks++;
    s_diag.last_rx_byte = s_rx_byte;

    now = HAL_GetTick();
    s_diag.last_rx_tick = now;
    s_diag.last_uart_error = huart->ErrorCode;
    if ((s_rx.state != GM810_RX_WAIT_HEADER) && ((now - s_rx.last_byte_tick) > GM810_RX_TIMEOUT_MS)) {
        gm810_parser_reset();
    }
    s_rx.last_byte_tick = now;

    if (s_rx.state == GM810_RX_WAIT_HEADER) {
        if (s_rx_byte == GM810_FRAME_HEADER_BYTE) {
            s_rx.state = GM810_RX_WAIT_LEN;
        }
        gm810_restart_receive_it();
        return;
    }

    if (s_rx.state == GM810_RX_WAIT_LEN) {
        s_rx.expected_len = s_rx_byte;
        s_rx.received_len = 0U;
        s_rx.flags = 0U;
        if (s_rx.expected_len == 0U) {
            gm810_parser_reset();
        } else {
            if (s_rx.expected_len > GM810_QR_DATA_MAX_LEN) {
                s_rx.flags |= GM810_QR_FLAG_ERROR_OVERSIZE;
            }
            s_rx.state = GM810_RX_WAIT_PAYLOAD;
        }
        gm810_restart_receive_it();
        return;
    }

    if (s_rx.received_len < GM810_QR_DATA_MAX_LEN) {
        s_rx.payload[s_rx.received_len] = s_rx_byte;
    }
    if (gm810_is_printable_ascii(s_rx_byte) == 0U) {
        s_rx.flags |= GM810_QR_FLAG_ERROR_NON_ASCII;
    }
    s_rx.received_len++;

    if (s_rx.received_len >= s_rx.expected_len) {
        gm810_publish_completed_frame_from_isr();
        gm810_parser_reset();
    }

    gm810_restart_receive_it();
}

void service_gm810_uart_error_callback(UART_HandleTypeDef *huart)
{
    if (huart != &huart6) {
        return;
    }

    s_diag.error_callbacks++;
    s_diag.last_uart_error = huart->ErrorCode;

    __HAL_UART_CLEAR_OREFLAG(huart);
    gm810_parser_reset();
    gm810_restart_receive_it();
}

const volatile service_gm810_uart_diag_t *service_gm810_uart_get_diag(void)
{
    return &s_diag;
}
