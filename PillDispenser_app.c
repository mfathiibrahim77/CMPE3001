/* =====================================================================
 *  CMPE3001 Design Assignment
 *  Smart Medication Dispenser  —  application code for main.c
 * ---------------------------------------------------------------------
 *  REUSED UNCHANGED FROM THE SUPPLIED SUPPORT MATERIALS:
 *      LCD1602_I2C.h/.c   16x2 LCD over I2C1 (hi2c1)          (§5)
 *      oximeter.h/.c      heart-rate / SpO2 sensor over I2C3  (§6)
 *      keypad.h/.c        3x4 matrix keypad (7 GPIOs)         (§7)
 *      panel.h/.c         4 push buttons + 4 LEDs             (§8)
 *  Copy those four driver pairs into Core/Inc and Core/Src exactly as given.
 *
 *  The heart-rate stability filter below (Hist / hist_push / hist_stable)
 *  is taken straight from the Support-Materials reference app (§9) — it
 *  rejects the PPG "doubling" artefact so a real 72 bpm never shows as 144.
 *
 *  ONLY CubeMX ADDITION on top of the §4 setup is the servo, as in Lab 3:
 *      TIM2 -> Internal Clock, Channel1 = PWM Generation CH1,
 *              Prescaler = 1599, Counter Period = 999, Pulse = 25.
 *      (PA0 / A0 is left free by the base board, so nothing else changes.)
 *
 *  Servo maths from Lab 3 (1 count = 20 us,  CCR = 25 + 0.556 * angle):
 *      CCR  25 = slot 1 (0 deg)      CCR  92 = slot 3 (~120 deg)
 *      CCR  58 = slot 2 (~60 deg)    CCR 125 = slot 4 (180 deg)
 *
 *  DESIGN:  3 inputs  = keypad, heart-rate sensor, confirm button
 *           4 outputs = LCD, servo carousel, GREEN LED, RED LED
 *
 *  Paste each block below into the matching USER CODE section of main.c.
 * ===================================================================== */


/* ============ USER CODE BEGIN Includes ============ */
#include "LCD1602_I2C.h"
#include "oximeter.h"
#include "keypad.h"
#include "panel.h"
#include <stdio.h>
#include <string.h>


/* ============ USER CODE BEGIN PD ============ */
/* ---- Servo carousel: one CCR value per compartment (from Lab 3) ---- */
#define SERVO_SLOT1       25u         /* 0   deg  - morning dose            */
#define SERVO_SLOT2       58u         /* ~60 deg  - noon dose               */
#define SERVO_SLOT3       92u         /* ~120deg  - evening dose            */
#define SERVO_SLOT4       125u        /* 180 deg  - night dose              */
#define SERVO_HOME        SERVO_SLOT1 /* resting position between doses     */
#define NUM_SLOTS         4u

/* ---- Timing (seconds). Short values so the demo runs quickly. -------- */
#define DOSE_INTERVAL_S   15u         /* gap between scheduled doses        */
#define CONFIRM_WINDOW_S  15u         /* time allowed to press "taken"      */

/* ---- Heart-rate safe band (bpm) -------------------------------------- */
#define HR_MIN            50
#define HR_MAX            120

/* ---- Vitals stability filter (same values as the §9 reference app) --- */
#define POLL_MS           250u        /* how often we re-read the sensor    */
#define HIST_N            5           /* readings that must agree           */
#define HR_SPREAD_MAX     12          /* max spread across them (bpm)       */
#define VITALS_TIMEOUT_MS 20000u      /* give up waiting for a finger (ms)  */

/* ---- Power-on PIN (same as the §9 reference app) --------------------- */
#define PASSWORD          "1234"
#define PIN_LEN           4
#define PIN_SCAN_MS       8u

/* ---- Confirm button = BUTTON0 = PB3 (active low, 10k pull-up on board)  */
#define CONFIRM_BTN_PORT  GPIOB
#define CONFIRM_BTN_PIN   GPIO_PIN_3

/* ---- Panel LED indices (T7-1 = PA1, T7-2 = PA4) ---------------------- */
#define LED_GREEN         0u          /* OK / armed / dose confirmed        */
#define LED_RED           1u          /* alert: bad vitals or missed dose   */


/* ============ USER CODE BEGIN 0 ============ */
/* ------------------------------ helpers ------------------------------ */

/* Pad a string to 16 chars so a shorter line leaves no stray characters. */
static void pad16(char *s)
{
    size_t n = strlen(s);
    while (n < 16u) { s[n++] = ' '; }
    s[16] = '\0';
}

/* Write two full 16-character lines to the LCD (uses the supplied driver). */
static void show2(const char *l1, const char *l2)
{
    char a[17], b[17];
    strncpy(a, l1, 16); a[16] = '\0'; pad16(a);
    strncpy(b, l2, 16); b[16] = '\0'; pad16(b);
    lcd_put_cur(0, 0); lcd_send_string(a);
    lcd_put_cur(1, 0); lcd_send_string(b);
}

/* ===== Heart-rate stability filter (from the §9 reference app) =====
 * A single reading cannot be trusted: just after a finger is placed, the
 * module's algorithm can count the dicrotic notch of the pulse as a second
 * beat and report exactly twice the true rate. A range check can't catch
 * that (140 is physiological). What catches it is agreement between
 * consecutive readings - the artefact comes and goes, while a real heart
 * rate settles into a narrow band. */
typedef struct {
    int32_t buf[HIST_N];
    uint8_t cnt;                        /* how many samples are filled in  */
    uint8_t idx;                        /* ring-buffer write index         */
} Hist;

static void hist_reset(Hist *h)
{
    h->cnt = 0;
    h->idx = 0;
}

static void hist_push(Hist *h, int32_t v)
{
    h->buf[h->idx] = v;
    h->idx = (uint8_t)((h->idx + 1u) % HIST_N);
    if (h->cnt < HIST_N) { h->cnt++; }
}

/* Full history AND spread within the limit -> return the median,
 * otherwise -1 meaning "not settled yet". */
static int32_t hist_stable(const Hist *h, int32_t spread_max)
{
    int32_t t[HIST_N];
    uint8_t i, j;

    if (h->cnt < HIST_N) { return -1; }

    for (i = 0; i < HIST_N; i++) { t[i] = h->buf[i]; }

    for (i = 1; i < HIST_N; i++) {              /* insertion sort, N = 5 */
        int32_t k = t[i];
        j = i;
        while (j > 0u && t[j - 1] > k) { t[j] = t[j - 1]; j--; }
        t[j] = k;
    }

    if ((t[HIST_N - 1] - t[0]) > spread_max) { return -1; }

    return t[HIST_N / 2];                         /* median */
}

/* Command the servo to a CCR value (same call as Lab 3). */
static void servo_set(uint16_t ccr)
{
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, ccr);
}

/* Return the CCR for a slot number 0..3. */
static uint16_t slot_ccr(uint8_t slot)
{
    switch (slot)
    {
        case 0:  return SERVO_SLOT1;
        case 1:  return SERVO_SLOT2;
        case 2:  return SERVO_SLOT3;
        default: return SERVO_SLOT4;
    }
}

/* Dispense: rotate the carousel so the due compartment lines up with the
 * drop hole, pause for the pill to fall, then return to the home position. */
static void dispense_from_slot(uint8_t slot)
{
    servo_set(slot_ccr(slot));
    HAL_Delay(1500);                 /* compartment aligned - pill drops   */
    servo_set(SERVO_HOME);
    HAL_Delay(400);                  /* re-seal                            */
}

/* Confirm button: 1 while held (active low). */
static uint8_t confirm_pressed(void)
{
    return (HAL_GPIO_ReadPin(CONFIRM_BTN_PORT, CONFIRM_BTN_PIN) == GPIO_PIN_RESET)
           ? 1u : 0u;
}

/* -------- power-on PIN gate (keypad) --------
 * Blocking by design: nothing proceeds until the correct PIN is entered.
 * Same logic as the §9 reference app's wait_for_password(). */
static void wait_for_password(void)
{
    char    entered[PIN_LEN + 1];
    char    masked[PIN_LEN + 1];
    uint8_t n = 0;

    show2("Enter PIN:", "____");

    for (;;)
    {
        char k = Keypad_Scan();                 /* supplied keypad driver  */

        if (k != 0)
        {
            if      (k >= '0' && k <= '9') { if (n < PIN_LEN) { entered[n++] = k; } }
            else if (k == '*')             { if (n > 0) { n--; } }   /* backspace */
            else if (k == '#')             { n = 0; }                /* clear     */

            {   /* echo entered digits as '*', the rest as '_' */
                uint8_t i;
                for (i = 0; i < PIN_LEN; i++) { masked[i] = (i < n) ? '*' : '_'; }
                masked[PIN_LEN] = '\0';
                lcd_put_cur(1, 0); lcd_send_string(masked);
            }

            if (n == PIN_LEN)                       /* verify automatically */
            {
                entered[n] = '\0';
                HAL_Delay(250);
                if (strncmp(entered, PASSWORD, PIN_LEN) == 0)
                {
                    Panel_SetLed(LED_GREEN, 1);
                    show2("PIN accepted", "");
                    HAL_Delay(900);
                    Panel_SetLed(LED_GREEN, 0);
                    return;                         /* the only way out */
                }
                Panel_SetLed(LED_RED, 1);
                show2("Wrong PIN", "Try again");
                HAL_Delay(1500);
                Panel_SetLed(LED_RED, 0);
                n = 0;
                show2("Enter PIN:", "____");
            }
        }
        HAL_Delay(PIN_SCAN_MS);
    }
}

/* -------- one vitals reading, stability-filtered --------
 * Waits for a finger and returns a SETTLED heart rate (the median of
 * HIST_N agreeing readings), or -1 on timeout / skip. *spo2_out receives
 * the matching SpO2 (or -1). Uses the §9 filter so the PPG doubling
 * artefact can't put a wrong value on the display. */
static int32_t measure_vitals(int32_t *spo2_out)
{
    uint32_t   start = HAL_GetTick();
    uint8_t    spin  = 0;
    OxiReading rd;
    Hist       hr_hist;
    int32_t    last_spo2 = -1;
    static const char *dots[4] = { "   ", ".  ", ".. ", "..." };

    hist_reset(&hr_hist);
    *spo2_out = -1;
    Panel_SetLed(LED_RED, 1);                       /* action needed       */
    show2("Dose due!", "Place finger...");

    while ((HAL_GetTick() - start) < VITALS_TIMEOUT_MS)
    {
        char l2[17];

        if (Oxi_Read(&rd) == HAL_OK && rd.heartbeat > 0)   /* supplied driver */
        {
            hist_push(&hr_hist, rd.heartbeat);
            if (rd.spo2 > 0) { last_spo2 = rd.spo2; }

            {   /* settled once HIST_N readings agree within HR_SPREAD_MAX */
                int32_t settled = hist_stable(&hr_hist, HR_SPREAD_MAX);
                if (settled > 0)
                {
                    *spo2_out = last_spo2;
                    Panel_SetLed(LED_RED, 0);
                    return settled;                 /* trustworthy bpm     */
                }
            }
            snprintf(l2, sizeof(l2), "Reading%s", dots[spin & 3u]);
        }
        else
        {
            snprintf(l2, sizeof(l2), "Place finger%s", dots[spin & 3u]);
        }

        show2("Dose due!", l2);
        spin++;

        if (Keypad_Scan() == '#') { return -1; }    /* '#' skips (demo)    */
        HAL_Delay(POLL_MS);
    }
    return -1;                                       /* timed out          */
}


/* ============ USER CODE BEGIN 2 ============ (runs once, after the MX_*_Init calls) */
/*
    lcd_init(&hi2c1);                            // supplied LCD driver
    show2("Pill Dispenser", "Starting...");
    HAL_Delay(800);

    Keypad_Init();                               // supplied keypad driver
    Panel_Init();                                // supplied panel driver (LEDs/buttons)
    Panel_SetLed(LED_GREEN, 0);
    Panel_SetLed(LED_RED,   0);

    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);    // servo (Lab 3)
    servo_set(SERVO_HOME);

    wait_for_password();                         // keypad PIN gate (blocks here)

    Oxi_Start(&hi2c3);                           // supplied oximeter driver
    HAL_Delay(500);
    show2("Dispenser ready", "Next dose: 15s");
*/


/* ============ USER CODE BEGIN WHILE ============ (declare before the while loop) */
/*
    typedef enum { ST_WAIT, ST_MEASURE, ST_DISPENSE, ST_CONFIRM } DispState;
    DispState  state       = ST_WAIT;
    uint32_t   last_tick   = HAL_GetTick();      // 1-second counter
    uint32_t   remaining   = DOSE_INTERVAL_S;    // seconds to next dose
    uint8_t    slot        = 0;                  // which compartment (0..3)
    uint32_t   doses_taken = 0;
    int32_t    hr = -1, spo2 = -1;
    uint32_t   confirm_start = 0;                // for the missed-dose timeout
*/


/* ============ USER CODE BEGIN 3 ============ (inside while(1)) */
/*
    switch (state)
    {
    // ---- 1. count down to the next scheduled dose ----
    case ST_WAIT:
        if ((HAL_GetTick() - last_tick) >= 1000u)
        {
            char l2[17];
            last_tick += 1000u;
            if (remaining > 0u) { remaining--; }
            snprintf(l2, sizeof(l2), "Slot %u in %lus",
                     (unsigned)(slot + 1u), (unsigned long)remaining);
            show2("Dispenser ready", l2);        // refreshed once per second
            if (remaining == 0u) { state = ST_MEASURE; }
        }
        break;

    // ---- 2. read the patient's vitals (input: HR sensor) ----
    case ST_MEASURE:
        hr = measure_vitals(&spo2);
        state = ST_DISPENSE;
        break;

    // ---- 3. decide, then rotate the carousel to the due slot ----
    case ST_DISPENSE:
    {
        char l1[17];

        if (hr < 0)                               // no reading taken
        {
            Panel_SetLed(LED_RED, 1);
            show2("No vitals read", "Dispensing...");
            HAL_Delay(1500);
        }
        else if (hr < HR_MIN || hr > HR_MAX)      // abnormal -> alert, still dose
        {
            Panel_SetLed(LED_RED, 1);
            snprintf(l1, sizeof(l1), "HR %ld ABNORMAL", (long)hr);
            show2(l1, "Contact carer!");
            HAL_Delay(2500);
        }
        else                                      // vitals OK
        {
            Panel_SetLed(LED_RED, 0);
            Panel_SetLed(LED_GREEN, 1);
            snprintf(l1, sizeof(l1), "HR %ld SpO2 %ld%%", (long)hr, (long)spo2);
            show2(l1, "Vitals OK");
            HAL_Delay(2000);
            Panel_SetLed(LED_GREEN, 0);
        }

        {
            char l1b[17];
            snprintf(l1b, sizeof(l1b), "Dispensing S%u", (unsigned)(slot + 1u));
            show2(l1b, "Please take it");
        }
        dispense_from_slot(slot);                 // servo carousel -> due slot

        show2("Press button", "to confirm dose");
        confirm_start = HAL_GetTick();            // start the missed-dose timer
        state = ST_CONFIRM;
        break;
    }

    // ---- 4. wait for the "taken" press, with a missed-dose timeout ----
    case ST_CONFIRM:
        if (confirm_pressed())                    // input: confirm button
        {
            doses_taken++;
            Panel_SetLed(LED_RED,   0);
            Panel_SetLed(LED_GREEN, 1);           // GREEN = confirmed
            show2("Dose confirmed", "Thank you");
            HAL_Delay(1500);
            Panel_SetLed(LED_GREEN, 0);

            slot      = (uint8_t)((slot + 1u) % NUM_SLOTS);  // advance carousel
            remaining = DOSE_INTERVAL_S;
            last_tick = HAL_GetTick();
            state     = ST_WAIT;
        }
        else if ((HAL_GetTick() - confirm_start) >= (CONFIRM_WINDOW_S * 1000u))
        {
            // MISSED DOSE: flash the RED LED and warn on the LCD
            uint8_t f;
            for (f = 0; f < 6u; f++)
            {
                Panel_SetLed(LED_RED, (uint8_t)(f & 1u));
                show2("DOSE MISSED", "Call carer!");
                HAL_Delay(300);
            }
            Panel_SetLed(LED_RED, 1);             // leave RED on as a flag
            HAL_Delay(1500);
            Panel_SetLed(LED_RED, 0);

            slot      = (uint8_t)((slot + 1u) % NUM_SLOTS);  // move on anyway
            remaining = DOSE_INTERVAL_S;
            last_tick = HAL_GetTick();
            state     = ST_WAIT;
        }
        break;
    }

    HAL_Delay(50);
*/
