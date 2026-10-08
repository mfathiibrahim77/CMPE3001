#include "keypad.h"

/* ==========================================================================
 * Wiring: which seven GPIOs the T3 wires go to
 *
 *    All seven are on the inner Arduino sockets, and none of them clashes with
 *    the peripherals already in use:
 *       I2C1 = PB8/PB9 (D15/D14)   LCD
 *       I2C3 = PC0/PC1 (A5/A4)     heart rate sensor
 *       SWD   = PA13/PA14          never take these
 *
 *    In CubeMX all seven must be configured as:
 *       GPIO mode               = Output Open Drain
 *       GPIO Pull-up/Pull-down = Pull-up
 *       Maximum output speed   = Low
 *
 *    Open drain can only pull low, never high; writing 1 leaves the pin in
 *    high impedance and the internal pull-up holds it high. If the user presses
 *    several keys at once and shorts wires together, the worst case is that
 *    they all go low - no pin ever drives against another. Push-pull outputs in
 *    a matrix scan are a common and damaging mistake.
 *
 *    In open drain mode IDR still reflects the true pin level, so the same pin
 *    serves as both output and input without reconfiguring it at run time.
 * ========================================================================== */
static GPIO_TypeDef *const PIN_PORT[KEYPAD_PINS] = {
    GPIOA,    /* T3-1 -> D2         PA10 */
    GPIOB,    /* T3-2 -> D4         PB5 */
    GPIOB,    /* T3-3 -> D5         PB4   */
    GPIOB,    /* T3-4 -> D6         PB10 */
    GPIOA,    /* T3-5 -> D7         PA8 */
    GPIOA,    /* T3-6 -> D8         PA9   */
    GPIOC,    /* T3-7 -> D9         PC7   */
};
static const uint16_t PIN_MASK[KEYPAD_PINS] = {
    GPIO_PIN_10,
    GPIO_PIN_5,
    GPIO_PIN_4,
    GPIO_PIN_10,
    GPIO_PIN_8,
    GPIO_PIN_9,
    GPIO_PIN_7,
};

/* ==========================================================================
 *    Pin pair -> key character
 *
 *    Measured on the actual hardware with KEYPAD_LEARN_MODE, then cross checked:
 *    the twelve pairs resolve to exactly one row/column assignment, and mapping
 *    that assignment back reproduces all twelve measurements.
 *
 *       rows       R1 R2 R3 R4 = T3 pins 2, 7, 6, 4
 *       columns C1 C2 C3             = T3 pins 3, 1, 5
 *
 *                  C1=3       C2=1       C3=5
 *       R1=2   |    1     |    2     |     3    |
 *       R2=7   |    4     |    5     |     6    |
 *       R3=6   |    7     |    8     |     9    |
 *       R4=4   |    *     |    0     |     #    |
 *
 *    Note this is NOT the common "pins 1-4 are rows, 5-7 are columns" layout:
 *    rows and columns are interleaved on T3. That is exactly why the scan
 *    identifies shorted pairs instead of assuming a row/column split - however
 *    odd the wiring, only this table changes.
 *
 *    To recalibrate for a different keypad: set KEYPAD_LEARN_MODE to 1, record
 *    the pin pair for each key, copy them in below, set it back to 0. The order
 *    of a and b does not matter; lookup compares them unordered.
 * ========================================================================== */
typedef struct { uint8_t a; uint8_t b; char key; } KeyEntry;

static const KeyEntry KEYMAP[] = {
    /*   C1=3          C2=1                         C3=5      */
    { 2, 3, '1' }, { 1, 2, '2' }, { 2, 5, '3' },                  /* R1 = 2 */
    { 3, 7, '4' }, { 1, 7, '5' }, { 5, 7, '6' },                  /* R2 = 7 */
    { 3, 6, '7' }, { 1, 6, '8' }, { 5, 6, '9' },                  /* R3 = 6 */
    { 3, 4, '*' }, { 1, 4, '0' }, { 4, 5, '#' },                  /* R4 = 4 */
};
#define KEYMAP_N     (sizeof(KEYMAP) / sizeof(KEYMAP[0]))

/* Debounce state */
static uint8_t  s_stable_a = 0, s_stable_b = 0;     /* confirmed pair, 0 = none */
static uint8_t  s_cand_a   = 0, s_cand_b   = 0;     /* pair awaiting confirmation */
static uint32_t s_cand_tick = 0;
static uint8_t  s_last_a   = 0, s_last_b   = 0;     /* for Keypad_LastPair */

static void pin_release(uint8_t i)                  /* open drain write 1 = high impedance */
{
    HAL_GPIO_WritePin(PIN_PORT[i], PIN_MASK[i], GPIO_PIN_SET);
}

static void pin_pull_low(uint8_t i)
{
    HAL_GPIO_WritePin(PIN_PORT[i], PIN_MASK[i], GPIO_PIN_RESET);
}

static uint8_t pin_read(uint8_t i)
{
    return (HAL_GPIO_ReadPin(PIN_PORT[i], PIN_MASK[i]) == GPIO_PIN_RESET)
            ? (uint8_t)0u : (uint8_t)1u;
}

void Keypad_Init(void)
{
    uint8_t i;

    /* CubeMX configures the pins as Output Open Drain + Pull-up in
     * MX_GPIO_Init(); this only releases all seven to high impedance. */
    for (i = 0; i < KEYPAD_PINS; i++) { pin_release(i); }

    s_stable_a = s_stable_b = 0;
    s_cand_a   = s_cand_b   = 0;
    s_last_a   = s_last_b   = 0;
}

/* Pull each wire low in turn and read the others to find the shorted pair.
 * Returns 0/0 when no key is down; with several keys down, the first pair
 * found wins. */
static void scan_raw(uint8_t *pa, uint8_t *pb)
{
    uint8_t i, j;

    *pa = 0;
    *pb = 0;

    for (i = 0; i < KEYPAD_PINS; i++)
    {
        pin_pull_low(i);

        /* Open drain plus pull-up has slow edges, so let the level settle.
         * This runs inside a few-millisecond task, so HAL_Delay() must never
         * be used here - a busy loop it is. */
        for (volatile int d = 0; d < 200; d++) { __NOP(); }

        for (j = 0; j < KEYPAD_PINS; j++)
        {
            if (j == i) { continue; }
            if (pin_read(j) == 0u)          /* j went low too -> i and j shorted */
            {
                *pa = (uint8_t)(i + 1u);    /* numbering starts at 1 outside */
                *pb = (uint8_t)(j + 1u);
                break;
            }
        }

        pin_release(i);

        if (*pa != 0u) { break; }
    }
}

static char lookup(uint8_t a, uint8_t b)
{
    uint8_t k;

    for (k = 0; k < KEYMAP_N; k++)
    {
        /* Unordered compare: (2,6) and (6,2) are the same key */
        if ((KEYMAP[k].a == a && KEYMAP[k].b == b) ||
            (KEYMAP[k].a == b && KEYMAP[k].b == a))
        {
            return KEYMAP[k].key;
        }
    }
    return '?';                                /* not in the table yet */
}

char Keypad_Scan(void)
{
    uint8_t a, b;
    uint32_t now = HAL_GetTick();
    char      event = 0;

    scan_raw(&a, &b);

    if (a != s_cand_a || b != s_cand_b)
    {
        s_cand_a = a;                          /* level changed, restart timing */
        s_cand_b = b;
        s_cand_tick = now;
    }
    else if ((a != s_stable_a || b != s_stable_b) &&
             (now - s_cand_tick) >= KEYPAD_DEBOUNCE_MS)
    {
        s_stable_a = a;                        /* stable long enough -> confirm */
        s_stable_b = b;

        if (a != 0u)                           /* report on press only */
        {
            s_last_a = a;
            s_last_b = b;
            event = lookup(a, b);

            /* Learn mode looks the pair up as well, so the LCD can show the
             * pair and the character side by side while you calibrate. An
             * unmapped pair yields '?', which is still non-zero, so the
             * "a key was pressed" event is never lost. */
        }
    }

    return event;
}

void Keypad_LastPair(uint8_t *a, uint8_t *b)
{
    *a = s_last_a;
    *b = s_last_b;
}

uint8_t Keypad_SelfTest(uint8_t *a, uint8_t *b)
{
    uint8_t x = 0, y = 0;
    uint8_t i;

    /* Scan a few times so a not yet settled level right after reset does not
     * raise a false alarm. */
    for (i = 0; i < 5u; i++)
    {
        scan_raw(&x, &y);
        if (x == 0u) { break; }       /* one clean pass is enough */
    }

    *a = x;
    *b = y;
    return (x != 0u) ? 1u : 0u;
}
