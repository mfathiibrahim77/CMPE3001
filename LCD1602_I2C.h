/**
  ******************************************************************************
  * @file    LCD1602_I2C.h
  * @brief   HD44780 16x2 LCD driven through a PCF8574 I2C backpack.
  *
  *          The public API is identical to the direct-GPIO driver of Lab 2,
  *          apart from lcd_init() taking an I2C handle and the extra
  *          lcd_backlight() call. Application code written for Lab 2 runs
  *          unchanged: only the transport underneath has changed.
  ******************************************************************************
  */

#ifndef INC_LCD1602_I2C_H_
#define INC_LCD1602_I2C_H_

/* These includes must come before every declaration below: uint8_t is defined
 * in <stdint.h>, and I2C_HandleTypeDef comes from the HAL headers. Putting
 * them after the declarations gives "unknown type name 'uint8_t'". */
#include "stm32l4xx_hal.h"
#include <stdint.h>

/* 7-bit address of the backpack, already shifted left by one for the HAL.
 *   PCF8574   -> 0x27 << 1 = 0x4E   (most common)
 *   PCF8574A -> 0x3F << 1 = 0x7E
 * Run lcd_scan_bus() once if you are not sure which one you have. */
#define LCD_I2C_ADDR   (0x27 << 1)

void lcd_init(I2C_HandleTypeDef *hi2c);
void lcd_send_cmd(char cmd);
void lcd_send_data(char data);
void lcd_send_string(const char *str);
void lcd_put_cur(int row, int col);
void lcd_clear(void);
void lcd_backlight(int on);

/** Probe every address on the bus. Returns the first 8-bit address that
 * answers, or 0 if nothing does. Call it before lcd_init() when a new
 * module refuses to display anything. */
uint8_t lcd_scan_bus(I2C_HandleTypeDef *hi2c);

#endif /* INC_LCD1602_I2C_H_ */
