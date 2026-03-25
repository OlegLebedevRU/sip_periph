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

/* ---- internal state ---------------------------------------------------- */
static uint8_t s_hw_time[8] = {0};
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
    status = HAL_I2C_Master_Transmit(&hi2c2, DS3231_I2C_ADDR, &reg, 1, 5);
    if (status == HAL_OK) {
        status = HAL_I2C_Master_Receive(&hi2c2, DS3231_I2C_ADDR, buf, I2C_TIME_SYNC_WRITE_LEN, 5);
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
    status = HAL_I2C_Master_Transmit(&hi2c2, DS3231_I2C_ADDR, payload, sizeof(payload), 5);
    osMutexRelease(i2c2_MutexHandle);
    return status == HAL_OK;
}

/* ---- one-time DS3231 init (moved from StartDefaultTask, step 14) ------- */

void service_time_sync_init(void)
{
    /* Disable INTCN → enable SQW 1Hz output on DS3231M.
     * Control register 0x0E, bit2 = INTCN:
     *   INTCN=0 → SQW pin outputs 1Hz square wave (default RS1=RS2=0 → 1Hz).
     *   INTCN=1 (power-on default) → interrupt output from alarms. */
    uint8_t cntrl = 0;
    if (osMutexWait(i2c2_MutexHandle, 100) != osOK) {
        return;
    }
    HAL_I2C_Mem_Read(&hi2c2, DS3231_I2C_ADDR,
                     (uint16_t)DS3231_REG_CONTROL, I2C_MEMADD_SIZE_8BIT,
                     &cntrl, 1, 100);
    cntrl = cntrl & (~0x04U);   /* clear INTCN bit */
    uint8_t p[2] = { DS3231_REG_CONTROL, cntrl };
    HAL_I2C_Master_Transmit(&hi2c2, DS3231_I2C_ADDR, p, 2, 5);
    osMutexRelease(i2c2_MutexHandle);
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

bool service_time_sync_on_tick(uint8_t *ram)
{
    if (ram == NULL) {
        return false;
    }
    if (!ds3231_read_time(s_hw_time)) {
        return false;
    }

    memcpy(&ram[0x60], s_hw_time, I2C_TIME_SYNC_WRITE_LEN);

    {
        I2cPacketToMaster_t pckt = {
            .payload = s_hw_time,
            .len     = I2C_TIME_SYNC_WRITE_LEN,
            .type    = PACKET_TIME,
            .ttl     = 1000U,
        };
        xQueueSend(myQueueToMasterHandle, &pckt, 0);
    }
    return true;
}

void service_time_sync_from_master(const uint8_t *master_bcd7, uint8_t rx_count, uint8_t *ram)
{
    uint8_t rtc_time[I2C_TIME_SYNC_WRITE_LEN] = {0};
    uint32_t master_sec;
    uint32_t rtc_sec;
    uint32_t delta;

    if (!service_time_sync_validate_packet(master_bcd7, rx_count)) {
        return;
    }

    memcpy(&ram[I2C_REG_HW_TIME_SET_ADDR], master_bcd7, I2C_TIME_SYNC_WRITE_LEN);

    if (!ds3231_read_time(rtc_time) || !service_time_sync_validate_packet(rtc_time, I2C_TIME_SYNC_WRITE_LEN)) {
        (void)ds3231_write_time(master_bcd7);
        memcpy(&ram[0x60], master_bcd7, I2C_TIME_SYNC_WRITE_LEN);
        return;
    }

    master_sec = rtc_bcd_to_seconds(master_bcd7);
    rtc_sec    = rtc_bcd_to_seconds(rtc_time);
    delta      = (master_sec > rtc_sec) ? (master_sec - rtc_sec) : (rtc_sec - master_sec);

    if (delta > TIME_SYNC_DRIFT_SEC) {
        (void)ds3231_write_time(master_bcd7);
        memcpy(&ram[0x60], master_bcd7, I2C_TIME_SYNC_WRITE_LEN);
    }
}

void service_time_sync_datetimepack(const uint8_t *ram)
{
    uint8_t tmp1, tmp2, tmp3;

    if (ram == NULL) {
        s_date_time[0] = 0;
        return;
    }

    tmp1 = bcd_to_dec(ram[0x64]);
    s_date_time[0] = (char)(((tmp1 / 10U) % 10U) + '0');
    s_date_time[1] = (char)((tmp1 % 10U) + '0');
    s_date_time[2] = '.';

    tmp2 = bcd_to_dec(ram[0x65]);
    s_date_time[3] = (char)(((tmp2 / 10U) % 10U) + '0');
    s_date_time[4] = (char)((tmp2 % 10U) + '0');
    s_date_time[5] = '.';

    tmp3 = bcd_to_dec(ram[0x66]);
    s_date_time[6] = (char)(((tmp3 / 10U) % 10U) + '0');
    s_date_time[7] = (char)((tmp3 % 10U) + '0');
    s_date_time[8] = '-';

    tmp1 = bcd_to_dec(ram[0x62]);
    s_date_time[9]  = (char)(((tmp1 / 10U) % 10U) + '0');
    s_date_time[10] = (char)((tmp1 % 10U) + '0');
    s_date_time[11] = ':';

    tmp2 = bcd_to_dec(ram[0x61]);
    s_date_time[12] = (char)(((tmp2 / 10U) % 10U) + '0');
    s_date_time[13] = (char)((tmp2 % 10U) + '0');
    s_date_time[14] = ':';

    tmp3 = bcd_to_dec(ram[0x60]);
    s_date_time[15] = (char)(((tmp3 / 10U) % 10U) + '0');
    s_date_time[16] = (char)((tmp3 % 10U) + '0');
    s_date_time[17] = 0;
}

const char *service_time_sync_get_datetime_str(void)
{
    return s_date_time;
}