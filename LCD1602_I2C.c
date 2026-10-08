#include "LCD1602_I2C.h"

/* ---- How the PCF8574's 8 outputs are wired to the LCD on every common
 *      backpack. This is the only place to edit if yours differs. ----
 *
 *      P0 -> RS      P4 -> D4
 *      P1 -> RW      P5 -> D5
 *      P2 -> E       P6 -> D6
 *      P3 -> backlight transistor          P7 -> D7
 *
 * RW is driven low in software here (the driver never reads), so the byte
 * we push out is: D7 D6 D5 D4 | BL E RW RS
 */
#define LCD_RS   0x01u
#define LCD_RW   0x02u
#define LCD_EN   0x04u
#define LCD_BL   0x08u

static I2C_HandleTypeDef *s_hi2c = 0;
static uint8_t s_backlight = LCD_BL;     /* remembered between writes */

/* ---- Push one nibble, framed by a rising and falling edge on E ----
 *
 * The three bytes are sent inside a single I2C transaction. The PCF8574
 * latches each byte as it is acknowledged, so the sequence
 *      E low -> E high -> E low
 * appears on the LCD's enable pin all by itself.
 *
 * Note what has disappeared compared with the direct-GPIO driver: there is
 * no microsecond delay anywhere. At 100 kHz one byte on the bus takes about
 * 90 us, so E stays high far longer than the 450 ns the HD44780 needs, and
 * the whole three-byte transfer outlasts the 37 us an instruction takes to
 * execute. The bus timing does the waiting for us, which is why this version
 * needs no hardware timer at all.
 */
static void lcd_write4(uint8_t nibble, uint8_t rs)
{
    uint8_t base = (uint8_t)(((nibble & 0x0Fu) << 4) | rs | s_backlight);
    uint8_t seq[3];

    seq[0] = base;                         /* data set up, E low   */
    seq[1] = (uint8_t)(base | LCD_EN);     /* E high               */
    seq[2] = base;                         /* E low -> latched     */

    HAL_I2C_Master_Transmit(s_hi2c, LCD_I2C_ADDR, seq, 3, 50);
}

void lcd_send_cmd(char cmd)
{
    lcd_write4((uint8_t)((uint8_t)cmd >> 4), 0);
    lcd_write4((uint8_t)((uint8_t)cmd & 0x0Fu), 0);
}

void lcd_send_data(char data)
{
    lcd_write4((uint8_t)((uint8_t)data >> 4), LCD_RS);
    lcd_write4((uint8_t)((uint8_t)data & 0x0Fu), LCD_RS);
}

void lcd_send_string(const char *str)
{
    while (*str) { lcd_send_data(*str++); }
}

void lcd_clear(void)
{
    lcd_send_cmd(0x01);
    HAL_Delay(2);         /* clear takes 1.52 ms; the busy flag is not polled */
}

/* Row 0 starts at DDRAM address 0x00, row 1 at 0x40 */
void lcd_put_cur(int row, int col)
{
    switch (row)
    {
        case 0: col |= 0x80; break;
        case 1: col |= 0xC0; break;
        default: return;
    }
    lcd_send_cmd((char)col);
}

void lcd_backlight(int on)
{
    uint8_t b;
    s_backlight = on ? LCD_BL : 0u;
    b = s_backlight;
    HAL_I2C_Master_Transmit(s_hi2c, LCD_I2C_ADDR, &b, 1, 50);
}

uint8_t lcd_scan_bus(I2C_HandleTypeDef *hi2c)
{
    for (uint8_t a = 0x02u; a < 0xFEu; a = (uint8_t)(a + 2u))
    {
        if (HAL_I2C_IsDeviceReady(hi2c, a, 2, 10) == HAL_OK)
        {
            return a;      /* 8-bit form, ready to pass to the HAL */
        }
    }
    return 0u;
}

/* ---- Power-on initialisation (same datasheet sequence as the GPIO driver) ----
 * During wake-up the LCD is still in 8-bit mode and latches a whole byte per
 * strobe, so the three 0x03 wake-ups must go out as single nibbles. */
void lcd_init(I2C_HandleTypeDef *hi2c)
{
    s_hi2c = hi2c;
    s_backlight = LCD_BL;

    HAL_Delay(50);               /* wait > 40 ms after power-up */

    lcd_write4(0x03, 0);
    HAL_Delay(5);                /* > 4.1 ms */
    lcd_write4(0x03, 0);
    HAL_Delay(1);                /* > 100 us */
    lcd_write4(0x03, 0);
    HAL_Delay(1);
    lcd_write4(0x02, 0);         /* switch to 4-bit mode */
    HAL_Delay(1);

    lcd_send_cmd(0x28);          /* 4-bit, 2 lines, 5x8 font */
    lcd_send_cmd(0x08);          /* display off */
    lcd_send_cmd(0x01);          /* clear */
    HAL_Delay(2);
    lcd_send_cmd(0x06);          /* cursor auto-increment */
    lcd_send_cmd(0x0C);          /* display on, no cursor, no blink */
}
