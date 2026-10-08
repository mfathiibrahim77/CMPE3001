/**
  ******************************************************************************
  * @file    panel.h
  * @brief   Four push buttons (T8 pins 1-4) and four LEDs (T7/T9 pins 1-4)
  *          on the base board.
  *
  *    Behaviour: pressing BUTTONn toggles LEDn, for n = 0..3.
  *
  *    Hardware notes taken from the base board schematic:
  *     - Each button B0..B3 has a 10k pull-up to the "BUTTONs VCC" net and
  *         connects to GND when pressed, so a press reads LOW. No internal
  *         pull-up is needed once T5 is powered.
  *     - Each LED L0..L3 has a 330R resistor in series; both of its terminals
  *       are brought out, to T7 (through the resistor) and to T9 (direct), so
  *         the polarity depends on how you wire them. See PANEL_LED_ACTIVE_HIGH
  *      at the top of panel.c.
  ******************************************************************************
  */

#ifndef INC_PANEL_H_
#define INC_PANEL_H_

/* These includes must come before every declaration below */
#include "stm32l4xx_hal.h"
#include <stdint.h>

/* ---- Mirror mode, for checking the button wiring ----
 *
 * Set to 1: every LED follows its own button's raw level - hold BUTTONn and
 * LEDn lights, release it and LEDn goes out. Toggling, debouncing and edge
 * detection are all bypassed.
 *
 * This separates "can the button be read at all" from "is the toggle logic
 * right":
 *   holding a button works       -> the hardware is fine, look at the logic
 *   nothing happens              -> no level change on the pin; check the T5
 *                                  supply, the wiring and the common ground
 *
 * Set back to 0 when done. */
#define PANEL_DEBUG_MIRROR    0

/* ---- Static mode: Panel_Task() returns immediately ----
 *
 * Use it to test LED polarity and continuity without interference from the
 * buttons. A floating button pin drifts, Panel_Task() reads the drift as a
 * press and toggles an LED, so you end up measuring noise instead of the
 * effect of Panel_SetLed().
 *
 * With this on, whatever Panel_SetLed() writes stays put, which is the only
 * way to tell cleanly whether an LED lights on a 1 or on a 0.
 *
 * The buttons do nothing in static mode; that is the point. */
#define PANEL_DEBUG_STATIC    0

/* ---- Internal pull-ups instead of the board's external ones ----
 *
 * The buttons normally rely on the 10k pull-ups fed from BUTTONs VCC (T5).
 * If T5 is unpowered, or wired to GND by mistake, the pins sit low forever
 * and pressing changes nothing.
 *
 * Set to 1 and Panel_Init() reconfigures the four button pins as Input plus
 * Pull-up, substituting the MCU's roughly 40k internal pull-up:
 *
 *   LEDs go out and light on a press -> MCU and switches are fine, the fault
 *                                       is in the external pull-up rail
 *   LEDs stay on                        -> a low impedance path holds the pins
 *                                         at ground; the external 10k beats
 *                                         the internal 40k. Check T5 polarity
 *
 * Once T5 is correctly wired to 3V3, set this back to 0 and use the board's
 * 10k, which is four times stronger and more noise tolerant. */
#define PANEL_USE_INTERNAL_PULLUP    0

#define PANEL_N              4       /* 4 buttons, 4 LEDs */
#define PANEL_TICK_MS       10u      /* Panel_Task() calling period */
#define PANEL_DEBOUNCE       3       /* consecutive equal samples (3 x 10 ms) */

/* ---- Debug watch points, for the Live Expressions view in CubeIDE ----
 *   g_panel_btn_raw      raw button levels, bit0..3, 1 = released, 0 = pressed
 *   g_panel_btn_stable   same after debouncing
 *   g_panel_press_cnt    number of press edges seen so far
 */
extern volatile uint8_t   g_panel_btn_raw;
extern volatile uint8_t   g_panel_btn_stable;
extern volatile uint16_t g_panel_press_cnt;

void Panel_Init(void);

/** Call every PANEL_TICK_MS: debounce, detect press edges, toggle LEDs. */
void Panel_Task(void);

/** Set one LED directly (0..3); a non-zero on lights it. */
void Panel_SetLed(uint8_t i, uint8_t on);

/** Current state of the four LEDs, bit0 = LED0. For debugging. */
uint8_t Panel_GetLeds(void);

#endif /* INC_PANEL_H_ */
