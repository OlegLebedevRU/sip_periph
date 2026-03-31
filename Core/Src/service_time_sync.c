/*
 * service_time_sync.c
 *
 * DS3231M RTC service + BCD validation + master time sync.
 * Источник периодического события: TCA P0 (DS3231 1Hz SQW).
 */

#include <string.h>
#include "main.h"
#include "cmsis_os.h"
#include "service_time_sync.h"

/* ---- extern handles/queues defined in main.c --------------------------- */
extern I2C_HandleTypeDef hi2c2;
extern osMessageQId myQueueToMasterHandle;
extern osMutexId i2c2_MutexHandle;

/* ---- call to HMI ------------------------------------------------------- */
void hmi_notify_1hz_tick(void);

/* ---- internal state ---------------------------------------------------- */
static uint8_t s_hw_time[8] = {0};
static uint8_t s_cached_time[8] = {0}; /* 0x60..0x66 equivalent */
static char    s_date_time[19] = {0}; /* DD.MM.YY-HH:MM:SS\0 */

/* ---- internal helpers -------------------------------------------------- */
static uint8_t bcd_to_dec(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10U) + (value & 0x0FU));
}

static bool is_bcd_valid(uint8_t value, uint8_t max_dec)
{
    const uint8_t hi = (uint8_t)(value >> 4);
    const uint8_t lo = (uint8_t)(value & 0x0FU);
    if ((hi > 9U) || (lo > 9U)) {
        return false;
    }
    return bcd_to_dec(value) <= max_dec;
}

static uint32_t rtc_bcd_to_seconds(const uint8_t *buf)
{
    const uint32_t year  = 2000U + bcd_to_dec(buf[6]);
    const uint32_t month = bcd_to_dec(buf[5]);
    const uint32_t day   = bcd_to_dec(buf[4]);
    const uint32_t hour  = bcd_to_dec(buf[2]);
    const uint32_t min   = bcd_to_dec(buf[1]);
    const uint32_t sec   = bcd_to_dec(buf[0]);
    const uint32_t days  = ((year * 372U) + (month * 31U) + day);
    return (((days * 24U) + hour) * 60U + min) * 60U + sec;
}

static bool ds3231_read_time(uint8_t *buf)
{
    HAL_StatusTypeDef status = HAL_OK;
    uint8_t reg = DS3231_REG_TIME_BASE;
    if (buf == NULL) {
        return false;
    }
    if (osMutexWait(i2c2_MutexHandle, 100) != osOK) {
        return false;
    }
    status = HAL_I2C_Master_Transmit(&hi2c2, DS3231_I2C_ADDR, &reg, 1, 10);
    if (status == HAL_OK) {
        status = HAL_I2C_Master_Receive(&hi2c2, DS3231_I2C_ADDR, buf, I2C_TIME_SYNC_WRITE_LEN, 10);
    }
    osMutexRelease(i2c2_MutexHandle);
    return status == HAL_OK;
}

static bool ds3231_write_time(const uint8_t *buf)
{
    HAL_StatusTypeDef status = HAL_OK;
    uint8_t payload[I2C_TIME_SYNC_WRITE_LEN + 1U] = { DS3231_REG_TIME_BASE, 0 };
    if (buf == NULL) {
        return false;
    }
    memcpy(&payload[1], buf, I2C_TIME_SYNC_WRITE_LEN);
    if (osMutexWait(i2c2_MutexHandle, 100) != osOK) {
        return false;
    }
    status = HAL_I2C_Master_Transmit(&hi2c2, DS3231_I2C_ADDR, payload, sizeof(payload), 10);
    osMutexRelease(i2c2_MutexHandle);
    return status == HAL_OK;
}

/* ---- one-time DS3231 init (moved from StartDefaultTask, step 14) ------- */

#define DS3231_INIT_RETRY_COUNT  3U   /* max attempts to configure DS3231 */

void service_time_sync_init(void)
{
    /* Disable INTCN → enable SQW 1Hz output on DS3231M.
     * Control register 0x0E, bit2 = INTCN:
     *   INTCN=0 → SQW pin outputs 1Hz square wave (default RS1=RS2=0 → 1Hz).
     *   INTCN=1 (power-on default) → interrupt output from alarms.
     *
     * Retry up to DS3231_INIT_RETRY_COUNT times to handle transient I2C
     * errors or startup BUSY conditions. */
    uint8_t retries;
    for (retries = 0U; retries < DS3231_INIT_RETRY_COUNT; retries++) {
        uint8_t cntrl = 0U;
        HAL_StatusTypeDef rd;

        if (retries > 0U) {
            osDelay(20U);
        }
        if (osMutexWait(i2c2_MutexHandle, 200U) != osOK) {
            continue;
        }
        rd = HAL_I2C_Mem_Read(&hi2c2, DS3231_I2C_ADDR,
                              (uint16_t)DS3231_REG_CONTROL, I2C_MEMADD_SIZE_8BIT,
                              &cntrl, 1U, 50U);
        if (rd == HAL_OK) {
            cntrl = cntrl & (~0x04U);   /* clear INTCN bit */
            uint8_t p[2] = { DS3231_REG_CONTROL, cntrl };
            (void)HAL_I2C_Master_Transmit(&hi2c2, DS3231_I2C_ADDR, p, 2U, 10U);
        }
        osMutexRelease(i2c2_MutexHandle);
        if (rd == HAL_OK) {
            break;
        }
    }
}

/* ---- public API -------------------------------------------------------- */
bool service_time_sync_validate_packet(const uint8_t *buf, uint8_t len)
{
    uint8_t weekday;
    uint8_t day;
    uint8_t month;
    uint8_t year;

    if ((buf == NULL) || (len != I2C_TIME_SYNC_WRITE_LEN)) {
        return false;
    }

    if (!is_bcd_valid(buf[0], 59U) || !is_bcd_valid(buf[1], 59U) || !is_bcd_valid(buf[2], 23U)
            || !is_bcd_valid(buf[3], 7U) || !is_bcd_valid(buf[4], 31U) || !is_bcd_valid(buf[5], 12U)
            || !is_bcd_valid(buf[6], 99U)) {
        return false;
    }

    weekday = bcd_to_dec(buf[3]);
    day     = bcd_to_dec(buf[4]);
    month   = bcd_to_dec(buf[5]);
    year    = bcd_to_dec(buf[6]);

    if ((weekday < 1U) || (weekday > 7U) || (day < 1U) || (month < 1U)) {
        return false;
    }
    if (year < TIME_SYNC_MIN_YEAR) {
        return false;
    }
    if ((year == TIME_SYNC_MIN_YEAR) && (month < TIME_SYNC_MIN_MONTH)) {
        return false;
    }
    if ((year == TIME_SYNC_MIN_YEAR) && (month == TIME_SYNC_MIN_MONTH) && (day < TIME_SYNC_MIN_DAY)) {
        return false;
    }
    return true;
}

bool service_time_sync_on_tick(void)
{
    /* 1Hz HMI tick must fire unconditionally — it drives the console
     * countdown timer (s_console_remain).  Tying it to DS3231 success
     * would freeze the console if the RTC bus is unhealthy. */
    hmi_notify_1hz_tick();

    if (!ds3231_read_time(s_hw_time)) {
        return false;
    }

    memcpy(s_cached_time, s_hw_time, I2C_TIME_SYNC_WRITE_LEN);

    /* Format the human-readable string immediately so that all consumers
     * (HMI page0_time_widget, OLED, console) see up-to-date time without
     * depending on a separate task to call datetimepack(). */
    service_time_sync_datetimepack();

    {
        I2cPacketToMaster_t pckt = {
            .payload = s_hw_time,
            .len     = I2C_TIME_SYNC_WRITE_LEN,
            .type    = PACKET_TIME,
            .ttl     = 1000U,
        };
        xQueueSend(myQueueToMasterHandle, &pckt, 0);

        return true;
    }
    return false;
}

void service_time_sync_from_master(const uint8_t *master_bcd7, uint8_t rx_count)
{
    uint8_t rtc_time[I2C_TIME_SYNC_WRITE_LEN] = {0};
    uint32_t master_sec;
    uint32_t rtc_sec;
    uint32_t delta;

    if (!service_time_sync_validate_packet(master_bcd7, rx_count)) {
        return;
    }

    if (!ds3231_read_time(rtc_time) || !service_time_sync_validate_packet(rtc_time, I2C_TIME_SYNC_WRITE_LEN)) {
        (void)ds3231_write_time(master_bcd7);
        memcpy(s_cached_time, master_bcd7, I2C_TIME_SYNC_WRITE_LEN);
        return;
    }

    master_sec = rtc_bcd_to_seconds(master_bcd7);
    rtc_sec    = rtc_bcd_to_seconds(rtc_time);
    delta      = (master_sec > rtc_sec) ? (master_sec - rtc_sec) : (rtc_sec - master_sec);

    if (delta > TIME_SYNC_DRIFT_SEC) {
        (void)ds3231_write_time(master_bcd7);
        memcpy(s_cached_time, master_bcd7, I2C_TIME_SYNC_WRITE_LEN);
    }
}

void service_time_sync_datetimepack(void)
{
    uint8_t tmp1, tmp2, tmp3;

    tmp1 = bcd_to_dec(s_cached_time[4]); // 0x64 equivalent day
    s_date_time[0] = (char)(((tmp1 / 10U) % 10U) + '0');
    s_date_time[1] = (char)((tmp1 % 10U) + '0');
    s_date_time[2] = '.';

    tmp2 = bcd_to_dec(s_cached_time[5]); // 0x65 equivalent month
    s_date_time[3] = (char)(((tmp2 / 10U) % 10U) + '0');
    s_date_time[4] = (char)((tmp2 % 10U) + '0');
    s_date_time[5] = '.';

    tmp3 = bcd_to_dec(s_cached_time[6]); // 0x66 equivalent year
    s_date_time[6] = (char)(((tmp3 / 10U) % 10U) + '0');
    s_date_time[7] = (char)((tmp3 % 10U) + '0');
    s_date_time[8] = '-';

    tmp1 = bcd_to_dec(s_cached_time[2]); // 0x62 equivalent hour
    s_date_time[9]  = (char)(((tmp1 / 10U) % 10U) + '0');
    s_date_time[10] = (char)((tmp1 % 10U) + '0');
    s_date_time[11] = ':';

    tmp2 = bcd_to_dec(s_cached_time[1]); // 0x61 equivalent min
    s_date_time[12] = (char)(((tmp2 / 10U) % 10U) + '0');
    s_date_time[13] = (char)((tmp2 % 10U) + '0');
    s_date_time[14] = ':';

    tmp3 = bcd_to_dec(s_cached_time[0]); // 0x60 equivalent sec
    s_date_time[15] = (char)(((tmp3 / 10U) % 10U) + '0');
    s_date_time[16] = (char)((tmp3 % 10U) + '0');
    s_date_time[17] = 0;
}

const char *service_time_sync_get_datetime_str(void)
{
    return s_date_time;
}
