#include "panel.h"

/* ==========================================================================
 * LED polarity
 *
 *    Both terminals of every LED are brought out: one through a 330R resistor
 *    to T7, the other straight to T9. Either wiring works:
 *
 *     A) T9 all to GND, T7 to the GPIOs       ->    a high output lights the LED
 *                                                   PANEL_LED_ACTIVE_HIGH 1
 *     B) T9 all to 3V3, T7 to the GPIOs       ->    a low output lights the LED
 *                                                   PANEL_LED_ACTIVE_HIGH 0
 *
 *    A is recommended. If the buttons respond but nothing lights up, do not
 *    suspect the code first: flip this macro and rebuild, or move the T9 row
 *    from GND to 3V3.
 * ========================================================================== */
#define PANEL_LED_ACTIVE_HIGH     1

/* ==========================================================================
 *    Pin assignment
 *
 *    Every one of them is on the inner Arduino sockets, where the holes are
 *    labelled, so no counting on the morpho headers:
 *
 *       LED0..LED3     -> A1    A2      A3    D13
 *       BUTTON0..3     -> D3    D10     D11   D12
 *
 *    Pins that are already taken and must not be reused:
 *        PA0            the servo of Lab 3 (TIM2_CH1, A0). Deliberately avoided
 *                       so a board set up for Lab 3 works here unchanged
 *       PB8/PB9         I2C1, LCD (D15/D14)
 *       PC0/PC1         I2C3, heart rate sensor (A5/A4)
 *       PA8/PA9/PA10    keypad T3 (D7/D8/D2)
 *       PB4/PB5/PB10    keypad T3 (D5/D4/D6)
 *       PC7             keypad T3 (D9)
 *       PA13/PA14       SWD debug port
 *       PA2/PA3         ST-LINK virtual COM port, not routed to the headers
 *
 *    Two side effects worth knowing:
 *       D13 = PA5 also drives the on-board green LD2, so LD2 follows LED3.
 *       D3    = PB3 is SWO after reset. It is free as a plain GPIO while Debug
 *               is set to Serial Wire, but do NOT enable SWV or Trace in the
 *              IDE or it will fight BUTTON0. Putting a button (an input) here
 *              rather than an LED is deliberate: on a clash an input merely
 *              reads the wrong value, it never drives against another output.
 * ========================================================================== */
static GPIO_TypeDef *const LED_PORT[PANEL_N] = {
    GPIOA,   /* LED0 -> A1    PA1 */
    GPIOA,   /* LED1 -> A2    PA4 */
    GPIOB,   /* LED2 -> A3    PB0 */
    GPIOA,   /* LED3 -> D13 PA5, shared with the on-board LD2 */
};
static const uint16_t LED_MASK[PANEL_N] = {
    GPIO_PIN_1, GPIO_PIN_4, GPIO_PIN_0, GPIO_PIN_5,
};

static GPIO_TypeDef *const BTN_PORT[PANEL_N] = {
    GPIOB,   /* BUTTON0 -> D3 PB3 */
    GPIOB,   /* BUTTON1 -> D10 PB6 */
    GPIOA,   /* BUTTON2 -> D11 PA7 */
    GPIOA,   /* BUTTON3 -> D12 PA6 */
};
static const uint16_t BTN_MASK[PANEL_N] = {
    GPIO_PIN_3, GPIO_PIN_6, GPIO_PIN_7, GPIO_PIN_6,
};

/* ---- Debug: expose the raw button levels as globals ----
 *
 * Type g_panel_btn_raw into the Live Expressions view of STM32CubeIDE; the
 * value refreshes while the board keeps running, no breakpoint needed.
 * Pressing BUTTONn should clear the matching bit from 1 to 0.
 *
 *   bit0 = BUTTON0(PB3)      bit1 = BUTTON1(PB6)
 *   bit2 = BUTTON2(PA7)      bit3 = BUTTON3(PA6)
 *
 * They must be volatile, or the debugger may read a stale value after
 * optimisation. A few bytes, so they can stay in for good.
 */
volatile uint8_t   g_panel_btn_raw     = 0x0F;     /* raw levels, 1 = released */
volatile uint8_t   g_panel_btn_stable = 0x0F;      /* after debouncing */
volatile uint16_t g_panel_press_cnt    = 0;        /* press edges seen so far */

/* Debounce state per button */
static uint8_t s_stable[PANEL_N];      /* confirmed level: 1 released, 0 pressed */
static uint8_t s_count[PANEL_N];       /* how many equal samples in a row */
static uint8_t s_cand[PANEL_N];        /* candidate level */
static uint8_t s_leds;                 /* LED states, bit0 = LED0 */

static void led_write(uint8_t i, uint8_t on)
{
#if PANEL_LED_ACTIVE_HIGH
    HAL_GPIO_WritePin(LED_PORT[i], LED_MASK[i], on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#else
    HAL_GPIO_WritePin(LED_PORT[i], LED_MASK[i], on ? GPIO_PIN_RESET : GPIO_PIN_SET);
#endif
}

void Panel_SetLed(uint8_t i, uint8_t on)
{
    if (i >= PANEL_N) { return; }

    if (on) { s_leds = (uint8_t)(s_leds | (uint8_t)(1u << i)); }
    else    { s_leds = (uint8_t)(s_leds & (uint8_t)~(1u << i)); }

    led_write(i, on);
}

uint8_t Panel_GetLeds(void)
{
    return s_leds;                /* only the low four bits are meaningful */
}

void Panel_Init(void)
{
    uint8_t i;

#if PANEL_USE_INTERNAL_PULLUP
    /* Reconfigure the four button pins as Input plus Pull-up, overriding the
     * NOPULL that CubeMX generated. Doing it here rather than in CubeMX keeps
     * the choice in one macro and survives the next GENERATE CODE. */
    {
        GPIO_InitTypeDef gi = {0};

        gi.Mode = GPIO_MODE_INPUT;
        gi.Pull = GPIO_PULLUP;

        for (i = 0; i < PANEL_N; i++)
        {
            gi.Pin = BTN_MASK[i];
            HAL_GPIO_Init(BTN_PORT[i], &gi);
        }
    }
#endif

    s_leds = 0;

    for (i = 0; i < PANEL_N; i++)
    {
        led_write(i, 0u);             /* all off */

        /* Start from "released" (high): with the board's 10k pull-ups in
         * place, an untouched button reads 1. */
        s_stable[i] = 1u;
        s_cand[i]   = 1u;
        s_count[i]  = 0u;
    }
}

void Panel_Task(void)
{
#if PANEL_DEBUG_STATIC
    /* Static mode: no button scanning, no LED changes. The pin tables are
     * unused on this path, so reference them to keep the compiler quiet. */
    (void)BTN_PORT;
    (void)BTN_MASK;
#else
    uint8_t i;

    for (i = 0; i < PANEL_N; i++)
    {
        uint8_t now = (HAL_GPIO_ReadPin(BTN_PORT[i], BTN_MASK[i]) == GPIO_PIN_RESET)
                        ? (uint8_t)0u    /* low = pressed */
                        : (uint8_t)1u;

        /* Debug: record the raw level for the Live Expressions view */
        if (now) { g_panel_btn_raw = (uint8_t)(g_panel_btn_raw |   (uint8_t)(1u << i)); }
        else    { g_panel_btn_raw = (uint8_t)(g_panel_btn_raw & (uint8_t)~(1u << i)); }

#if PANEL_DEBUG_MIRROR
        /* Mirror mode: the LED follows the raw level, with no debouncing,
         * no edge detection and no toggling. Shows whether the pin level
         * changes at all. */
        Panel_SetLed(i, (uint8_t)(now ? 0u : 1u));
        continue;
#endif

        if (now != s_cand[i])
        {
            s_cand[i]  = now;           /* level changed, restart the count */
            s_count[i] = 0u;
            continue;
        }

        if (s_count[i] < PANEL_DEBOUNCE)
        {
            s_count[i]++;

            if (s_count[i] == PANEL_DEBOUNCE && now != s_stable[i])
            {
                s_stable[i] = now;       /* stable for PANEL_DEBOUNCE ticks */

                /* Debug: the debounced level */
                if (now) { g_panel_btn_stable = (uint8_t)(g_panel_btn_stable | (uint8_t)(1u << i)); }
                else     { g_panel_btn_stable = (uint8_t)(g_panel_btn_stable & (uint8_t)~(1u << i)); }

                if (now == 0u)           /* act on the press edge only */
                {
                    uint8_t on = (uint8_t)((s_leds & (uint8_t)(1u << i)) ? 0u : 1u);

                    g_panel_press_cnt++;   /* a good line for a breakpoint */
                    Panel_SetLed(i, on);
                }
            }
        }
    }
#endif
}
