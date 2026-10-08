/**
  ******************************************************************************
  * @file    keypad.h
  * @brief   3x4 membrane matrix keypad on the base board's T3 terminal (7 wires)
  *
  *    This driver does NOT assume which wires are rows and which are columns.
  *
  *    Pressing a key simply shorts two wires together. The seven T3 wires are
  *    therefore numbered 1..7 and treated alike: the scan pulls each one low in
  *    turn and reads the others, yielding "which two wires are shorted". The
  *    KEYMAP table in keypad.c maps that pair to a character - after measuring a
  *    real keypad you edit only that table, never the scan logic.
  ******************************************************************************
  */

#ifndef INC_KEYPAD_H_
#define INC_KEYPAD_H_

/* These includes must come before the declarations below, otherwise the
 * compiler reports "unknown type name 'uint8_t'". */
#include "stm32l4xx_hal.h"
#include <stdint.h>

/* Set to 1 to report the detected pin pair instead of a character, for
 * calibrating KEYMAP against a real keypad. Set back to 0 for normal use. */
#define KEYPAD_LEARN_MODE    0

#define KEYPAD_PINS          7      /* T3 is a 7-pin terminal */
#define KEYPAD_DEBOUNCE_MS   25u

void Keypad_Init(void);

/** Call every 5-10 ms. Returns the key character on a new press, else 0.
 * A held key is reported once. Pairs absent from KEYMAP return '?'. */
char Keypad_Scan(void);

/** Last detected pin pair (1..7), 0 when no key is down. Used together with
 *    KEYPAD_LEARN_MODE to calibrate KEYMAP. */
void Keypad_LastPair(uint8_t *a, uint8_t *b);

/** Self test, to be called with no key pressed: detects two wires that are
 * permanently shorted. Such a fault makes the scan always return that same
 *    pair, so every key appears dead while the program looks healthy.
 * @return 1 if a stuck pair was found (reported through a and b), else 0
 */
uint8_t Keypad_SelfTest(uint8_t *a, uint8_t *b);

#endif /* INC_KEYPAD_H_ */
