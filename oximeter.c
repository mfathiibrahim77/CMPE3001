#include "oximeter.h"

static I2C_HandleTypeDef *s_hi2c = 0;

/* ---- Register read: write the 1-byte address (with STOP), then read ----
 *
 * Two separate transfers on purpose, rather than HAL_I2C_Mem_Read().
 * HAL_I2C_Mem_Read() issues a repeated START after the address, while the
 * vendor's Arduino library uses endTransmission() + requestFrom(), i.e. a
 * STOP in between. The module's algorithm MCU follows the latter, so a
 * repeated START may return nothing. Suspect this first if the bus scan
 * finds the address but every byte reads back as zero.
 */
static HAL_StatusTypeDef oxi_read_reg(uint8_t reg, uint8_t *buf, uint16_t len)
{
    HAL_StatusTypeDef st;

    st = HAL_I2C_Master_Transmit(s_hi2c, OXI_I2C_ADDR, &reg, 1, 100);
    if (st != HAL_OK) { return st; }

    return HAL_I2C_Master_Receive(s_hi2c, OXI_I2C_ADDR, buf, len, 100);
}

/* ---- Register write: address plus data in a single transfer ---- */
static HAL_StatusTypeDef oxi_write_reg(uint8_t reg, const uint8_t *data, uint16_t len)
{
    uint8_t buf[8];
    uint16_t i;

    if (len > (sizeof(buf) - 1u)) { return HAL_ERROR; }

    buf[0] = reg;
    for (i = 0; i < len; i++) { buf[i + 1u] = data[i]; }

    return HAL_I2C_Master_Transmit(s_hi2c, OXI_I2C_ADDR, buf, (uint16_t)(len + 1u), 100);
}

uint8_t Oxi_IsPresent(I2C_HandleTypeDef *hi2c)
{
    return (HAL_I2C_IsDeviceReady(hi2c, OXI_I2C_ADDR, 3, 50) == HAL_OK) ? 1u : 0u;
}

void Oxi_Start(I2C_HandleTypeDef *hi2c)
{
    uint8_t cmd[2] = { 0x00u, 0x01u };    /* 0x0001 = start acquisition */
    s_hi2c = hi2c;
    (void)oxi_write_reg(OXI_REG_COLLECT, cmd, 2);
}

void Oxi_Stop(void)
{
    uint8_t cmd[2] = { 0x00u, 0x02u };   /* 0x0002 = stop acquisition */
    (void)oxi_write_reg(OXI_REG_COLLECT, cmd, 2);
}

HAL_StatusTypeDef Oxi_Read(OxiReading *out)
{
    uint8_t r[8];
    HAL_StatusTypeDef st;

    out->spo2 = -1;
    out->heartbeat = -1;

    st = oxi_read_reg(OXI_REG_DATA, r, 8);
    if (st != HAL_OK) { return st; }

    /* Byte 0 is SpO2 in percent; bytes 2..5 are the heart rate, 32-bit
     * big endian. The module reports 0 for "nothing measured this time",
     * which is turned into -1 for the caller.
     *
     * A range check is applied on top: right after a finger is placed the
     * algorithm has not converged and the module occasionally emits wildly
     * wrong values. Rejecting them here keeps 500 bpm off the display. */
    if (r[0] != 0u && r[0] <= OXI_SPO2_MAX)
    {
        out->spo2 = (int16_t)r[0];
    }

    {
        uint32_t hb = ((uint32_t)r[2] << 24) | ((uint32_t)r[3] << 16)
                    | ((uint32_t)r[4] << 8) | ((uint32_t)r[5]);
        if (hb != 0u && hb <= OXI_HR_MAX)
        {
            out->heartbeat = (int32_t)hb;
        }
    }

    return HAL_OK;
}

float Oxi_ReadTemperature(void)
{
    uint8_t t[2];

    if (oxi_read_reg(OXI_REG_TEMP, t, 2) != HAL_OK) { return -100.0f; }

    /* Byte 0 is the integer part, byte 1 the hundredths of a degree */
    return (float)t[0] + ((float)t[1] / 100.0f);
}

uint8_t Oxi_ScanBus(I2C_HandleTypeDef *hi2c)
{
    for (uint8_t a = 0x02u; a < 0xFEu; a = (uint8_t)(a + 2u))
    {
        if (HAL_I2C_IsDeviceReady(hi2c, a, 2, 10) == HAL_OK)
        {
            return a;
        }
    }
    return 0u;
}
