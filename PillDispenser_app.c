/* =====================================================================
 *  CMPE3001 Design Assignment
 *  Smart Medication Dispenser  —  FINAL application code for main.c
 * ---------------------------------------------------------------------
 *  REUSED UNCHANGED FROM THE SUPPLIED SUPPORT MATERIALS (§5-§8):
 *      LCD1602_I2C.h/.c   16x2 LCD over I2C1 (hi2c1)
 *      oximeter.h/.c      heart-rate / SpO2 sensor over I2C3 (hi2c3)
 *      keypad.h/.c        3x4 matrix keypad (7 GPIOs)
 *      panel.h/.c         4 push buttons + 4 LEDs
 *  The HR stability filter (Hist / hist_stable) is lifted from §9.
 *
 *  ONLY CubeMX addition on top of §4 is the servo (Lab 3):
 *      TIM2 internal clock, CH1 PWM, PSC 1599, ARR 999, Pulse 25, PA0.
 *
 *  DESIGN  (every part does real work - nothing bolted on):
 *    INPUTS  (3): keypad (PIN), HR/SpO2 sensor (vitals gate), buttons.
 *    OUTPUTS (4): LCD, servo 4-slot carousel, GREEN LD2 (OK), RED LED (alert).
 *
 *  FEATURES:
 *    - First-run PIN enrolment (set + confirm), then log in to use it.
 *    - Change PIN on a button, old PIN required.  (PIN kept in RAM.)
 *    - Vitals SAFETY GATE: normal -> dispense; HR low/high (or no reading)
 *      -> hold the dose, a carer must press OVERRIDE to release it.
 *    - 4-slot carousel (morning/noon/evening/night).
 *    - Missed-dose alert if the "taken" button isn't pressed in time.
 *
 *  BUTTONS:  BUTTON0 PB3 = confirm "taken"   BUTTON1 PB6 = change PIN
 *            BUTTON2 PA7 = carer override     BUTTON3 PA6 = spare
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
#define SERVO_SLOT1       25u
#define SERVO_SLOT2       58u
#define SERVO_SLOT3       92u
#define SERVO_SLOT4       125u
#define SERVO_HOME        SERVO_SLOT1
#define NUM_SLOTS         4u

/* ---- Timing (seconds). Short values so the demo runs quickly. ---- */
#define DOSE_INTERVAL_S   15u
#define CONFIRM_WINDOW_S  15u
#define OVERRIDE_WINDOW_S 20u

/* ---- Heart-rate safe band (bpm) ---- */
#define HR_MIN            50
#define HR_MAX            120

/* ---- Vitals stability filter (same values as the §9 reference app) ---- */
#define POLL_MS           250u
#define HIST_N            5
#define HR_SPREAD_MAX     12
#define VITALS_TIMEOUT_MS 20000u

/* ---- PIN ---- */
#define PIN_LEN           4
#define PIN_SCAN_MS       8u

/* ---- Panel LED indices (base LEDs all RED; LD2 is the on-board GREEN) ---- */
#define LED_OK            3u          /* PA5 -> on-board GREEN LD2: OK/confirmed */
#define LED_ALERT         0u          /* PA1 -> base-board RED LED: alert        */

/* ---- Buttons (active low; board has 10k pull-ups) ---- */
#define CONFIRM_PORT      GPIOB
#define CONFIRM_PIN       GPIO_PIN_3  /* BUTTON0: dose taken        */
#define CHANGE_PORT       GPIOB
#define CHANGE_PIN        GPIO_PIN_6  /* BUTTON1: change PIN         */
#define OVERRIDE_PORT     GPIOA
#define OVERRIDE_PIN      GPIO_PIN_7  /* BUTTON2: carer override     */


/* ============ USER CODE BEGIN 0 ============ */
/* ---- the live PIN, held in RAM (set fresh at every power-up) ---- */
static char current_pin[PIN_LEN + 1] = "";

/* -------- small helpers -------- */

static void pad16(char *s)
{
    size_t n = strlen(s);
    while (n < 16u) { s[n++] = ' '; }
    s[16] = '\0';
}

static void show2(const char *l1, const char *l2)
{
    char a[17], b[17];
    strncpy(a, l1, 16); a[16] = '\0'; pad16(a);
    strncpy(b, l2, 16); b[16] = '\0'; pad16(b);
    lcd_put_cur(0, 0); lcd_send_string(a);
    lcd_put_cur(1, 0); lcd_send_string(b);
}

/* A button reads LOW when pressed. */
static uint8_t btn_down(GPIO_TypeDef *port, uint16_t pin)
{
    return (HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_RESET) ? 1u : 0u;
}

/* ---- servo ---- */
static void servo_set(uint16_t ccr)
{
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, ccr);
}

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

static void dispense_from_slot(uint8_t slot)
{
    servo_set(slot_ccr(slot));
    HAL_Delay(1500);                 /* compartment aligned - pill drops */
    servo_set(SERVO_HOME);
    HAL_Delay(400);                  /* re-seal */
}

/* ===== PIN entry / management (keypad) ===== */

/* Collect exactly PIN_LEN digits into out[PIN_LEN+1]. '*' = backspace,
 * '#' = clear. Blocks until PIN_LEN digits are in. Echoes '*' per digit. */
static void enter_pin(const char *prompt, char *out)
{
    uint8_t n = 0;
    char    masked[PIN_LEN + 1];

    show2(prompt, "____");

    for (;;)
    {
        char k = Keypad_Scan();

        if (k != 0)
        {
            if      (k >= '0' && k <= '9') { if (n < PIN_LEN) { out[n++] = k; } }
            else if (k == '*')             { if (n > 0) { n--; } }
            else if (k == '#')             { n = 0; }

            {
                uint8_t i;
                for (i = 0; i < PIN_LEN; i++) { masked[i] = (i < n) ? '*' : '_'; }
                masked[PIN_LEN] = '\0';
                lcd_put_cur(1, 0); lcd_send_string(masked);
            }

            if (n == PIN_LEN)
            {
                out[PIN_LEN] = '\0';
                HAL_Delay(250);          /* let the last '*' be seen */
                return;
            }
        }
        HAL_Delay(PIN_SCAN_MS);
    }
}

/* Set a new PIN into dest, requiring the two entries to match. */
static void set_new_pin(char *dest)
{
    char a[PIN_LEN + 1], b[PIN_LEN + 1];

    for (;;)
    {
        enter_pin("Set new PIN:", a);
        enter_pin("Confirm PIN:", b);

        if (strncmp(a, b, PIN_LEN) == 0)
        {
            strncpy(dest, a, PIN_LEN + 1);
            Panel_SetLed(LED_OK, 1);
            show2("PIN set", "");
            HAL_Delay(900);
            Panel_SetLed(LED_OK, 0);
            return;
        }

        Panel_SetLed(LED_ALERT, 1);
        show2("PIN mismatch", "Try again");
        HAL_Delay(1500);
        Panel_SetLed(LED_ALERT, 0);
    }
}

/* Ask for the PIN and compare it to current_pin: 1 = match. */
static uint8_t verify_pin(const char *prompt)
{
    char e[PIN_LEN + 1];
    enter_pin(prompt, e);
    return (strncmp(e, current_pin, PIN_LEN) == 0) ? 1u : 0u;
}

/* Log in: keep asking until the PIN is right. */
static void unlock(void)
{
    for (;;)
    {
        if (verify_pin("Enter PIN:"))
        {
            Panel_SetLed(LED_OK, 1);
            show2("Unlocked", "");
            HAL_Delay(900);
            Panel_SetLed(LED_OK, 0);
            return;
        }
        Panel_SetLed(LED_ALERT, 1);
        show2("Wrong PIN", "Try again");
        HAL_Delay(1500);
        Panel_SetLed(LED_ALERT, 0);
    }
}

/* Change the PIN - old PIN must be entered first. */
static void change_pin(void)
{
    if (verify_pin("Old PIN:"))
    {
        set_new_pin(current_pin);
    }
    else
    {
        Panel_SetLed(LED_ALERT, 1);
        show2("Wrong PIN", "Not changed");
        HAL_Delay(1800);
        Panel_SetLed(LED_ALERT, 0);
    }
}

/* ===== Heart-rate stability filter (from the §9 reference app) =====
 * Rejects the PPG doubling artefact: a reading is only trusted once
 * HIST_N consecutive values agree within HR_SPREAD_MAX bpm. */
typedef struct {
    int32_t buf[HIST_N];
    uint8_t cnt;
    uint8_t idx;
} Hist;

static void hist_reset(Hist *h) { h->cnt = 0; h->idx = 0; }

static void hist_push(Hist *h, int32_t v)
{
    h->buf[h->idx] = v;
    h->idx = (uint8_t)((h->idx + 1u) % HIST_N);
    if (h->cnt < HIST_N) { h->cnt++; }
}

static int32_t hist_stable(const Hist *h, int32_t spread_max)
{
    int32_t t[HIST_N];
    uint8_t i, j;

    if (h->cnt < HIST_N) { return -1; }
    for (i = 0; i < HIST_N; i++) { t[i] = h->buf[i]; }
    for (i = 1; i < HIST_N; i++) {
        int32_t k = t[i]; j = i;
        while (j > 0u && t[j - 1] > k) { t[j] = t[j - 1]; j--; }
        t[j] = k;
    }
    if ((t[HIST_N - 1] - t[0]) > spread_max) { return -1; }
    return t[HIST_N / 2];
}

/* One stability-filtered vitals reading. Returns a settled HR (bpm), or
 * -1 on timeout / skip. *spo2_out gets the matching SpO2 (or -1). */
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
    Panel_SetLed(LED_ALERT, 1);
    show2("Dose due!", "Place finger...");

    while ((HAL_GetTick() - start) < VITALS_TIMEOUT_MS)
    {
        char l2[17];

        if (Oxi_Read(&rd) == HAL_OK && rd.heartbeat > 0)
        {
            hist_push(&hr_hist, rd.heartbeat);
            if (rd.spo2 > 0) { last_spo2 = rd.spo2; }

            {
                int32_t settled = hist_stable(&hr_hist, HR_SPREAD_MAX);
                if (settled > 0)
                {
                    *spo2_out = last_spo2;
                    Panel_SetLed(LED_ALERT, 0);
                    return settled;
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

        if (Keypad_Scan() == '#') { return -1; }    /* '#' skips (demo) */
        HAL_Delay(POLL_MS);
    }
    return -1;
}

/* Carer safety gate: flash red and wait for the OVERRIDE button.
 * Returns 1 if a carer authorised, 0 on timeout (dose withheld). */
static uint8_t wait_for_override(void)
{
    uint32_t start = HAL_GetTick();
    uint8_t  spin  = 0;

    while ((HAL_GetTick() - start) < (OVERRIDE_WINDOW_S * 1000u))
    {
        Panel_SetLed(LED_ALERT, (uint8_t)(spin & 1u));   /* flash */
        show2("Carer authorise?", "Press OVERRIDE");

        if (btn_down(OVERRIDE_PORT, OVERRIDE_PIN))
        {
            Panel_SetLed(LED_ALERT, 0);
            return 1u;
        }
        spin++;
        HAL_Delay(300);
    }
    Panel_SetLed(LED_ALERT, 0);
    return 0u;                                           /* withhold */
}


/* ============ USER CODE BEGIN 2 ============ (runs once, after the MX_*_Init calls) */
/*
    lcd_init(&hi2c1);
    show2("Med Dispenser", "Starting...");
    HAL_Delay(800);

    Keypad_Init();
    Panel_Init();
    Panel_SetLed(LED_OK, 0);
    Panel_SetLed(LED_ALERT, 0);

    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);   // servo
    servo_set(SERVO_HOME);

    set_new_pin(current_pin);                   // first-run PIN enrolment (set + confirm)
    unlock();                                   // log in with that PIN

    Oxi_Start(&hi2c3);                          // start heart-rate acquisition
    HAL_Delay(500);
    show2("Dispenser ready", "Next dose: 15s");
*/


/* ============ USER CODE BEGIN WHILE ============ (declare before the while loop) */
/*
    typedef enum { ST_WAIT, ST_MEASURE, ST_DISPENSE, ST_CONFIRM } DispState;
    DispState  state       = ST_WAIT;
    uint32_t   last_tick   = HAL_GetTick();
    uint32_t   remaining   = DOSE_INTERVAL_S;
    uint8_t    slot        = 0;
    uint32_t   doses_taken = 0;
    int32_t    hr = -1, spo2 = -1;
    uint8_t    authorised = 0;
    uint32_t   confirm_start = 0;
*/


/* ============ USER CODE BEGIN 3 ============ (inside while(1)) */
/*
    switch (state)
    {
    // ---- 1. idle: count down, and allow a PIN change ----
    case ST_WAIT:
        if (btn_down(CHANGE_PORT, CHANGE_PIN))      // BUTTON1 -> change PIN
        {
            change_pin();
            while (btn_down(CHANGE_PORT, CHANGE_PIN)) { HAL_Delay(10); }  // wait release
            last_tick = HAL_GetTick();
            show2("Dispenser ready", "Next dose: 15s");
            break;
        }
        if ((HAL_GetTick() - last_tick) >= 1000u)
        {
            char l2[17];
            last_tick += 1000u;
            if (remaining > 0u) { remaining--; }
            snprintf(l2, sizeof(l2), "Slot %u in %lus",
                     (unsigned)(slot + 1u), (unsigned long)remaining);
            show2("Dispenser ready", l2);
            if (remaining == 0u) { state = ST_MEASURE; }
        }
        break;

    // ---- 2. read vitals ----
    case ST_MEASURE:
        hr = measure_vitals(&spo2);
        state = ST_DISPENSE;
        break;

    // ---- 3. vitals SAFETY GATE, then dispense ----
    case ST_DISPENSE:
    {
        char l1[17];
        uint8_t safe = (uint8_t)(hr >= HR_MIN && hr <= HR_MAX);

        authorised = 1;                             // assume OK until proven otherwise

        if (hr < 0)                                 // no reading taken
        {
            Panel_SetLed(LED_ALERT, 1);
            show2("No vitals read", "Carer needed");
            HAL_Delay(1500);
            authorised = wait_for_override();
        }
        else if (!safe)                             // HR low or high -> hold
        {
            Panel_SetLed(LED_ALERT, 1);
            if (hr < HR_MIN) { snprintf(l1, sizeof(l1), "HR %ld LOW", (long)hr); }
            else             { snprintf(l1, sizeof(l1), "HR %ld HIGH", (long)hr); }
            show2(l1, "Dose HELD");
            HAL_Delay(2000);
            authorised = wait_for_override();       // carer must release it
        }
        else                                        // vitals OK -> go
        {
            Panel_SetLed(LED_ALERT, 0);
            Panel_SetLed(LED_OK, 1);
            snprintf(l1, sizeof(l1), "HR %ld SpO2 %ld%%", (long)hr, (long)spo2);
            show2(l1, "Vitals OK");
            HAL_Delay(2000);
            Panel_SetLed(LED_OK, 0);
        }

        if (!authorised)                            // withheld -> skip this dose
        {
            Panel_SetLed(LED_ALERT, 1);
            show2("Dose WITHHELD", "No authorise");
            HAL_Delay(2000);
            Panel_SetLed(LED_ALERT, 0);
            slot      = (uint8_t)((slot + 1u) % NUM_SLOTS);
            remaining = DOSE_INTERVAL_S;
            last_tick = HAL_GetTick();
            state     = ST_WAIT;
            break;
        }

        {
            char l1b[17];
            snprintf(l1b, sizeof(l1b), "Dispensing S%u", (unsigned)(slot + 1u));
            show2(l1b, "Please take it");
        }
        dispense_from_slot(slot);

        show2("Press button", "to confirm dose");
        confirm_start = HAL_GetTick();
        state = ST_CONFIRM;
        break;
    }

    // ---- 4. wait for "taken", with a missed-dose timeout ----
    case ST_CONFIRM:
        if (btn_down(CONFIRM_PORT, CONFIRM_PIN))    // BUTTON0 -> taken
        {
            doses_taken++;
            Panel_SetLed(LED_ALERT, 0);
            Panel_SetLed(LED_OK, 1);
            show2("Dose confirmed", "Thank you");
            HAL_Delay(1500);
            Panel_SetLed(LED_OK, 0);

            slot      = (uint8_t)((slot + 1u) % NUM_SLOTS);
            remaining = DOSE_INTERVAL_S;
            last_tick = HAL_GetTick();
            state     = ST_WAIT;
        }
        else if ((HAL_GetTick() - confirm_start) >= (CONFIRM_WINDOW_S * 1000u))
        {
            uint8_t f;
            for (f = 0; f < 6u; f++)
            {
                Panel_SetLed(LED_ALERT, (uint8_t)(f & 1u));
                show2("DOSE MISSED", "Call carer!");
                HAL_Delay(300);
            }
            Panel_SetLed(LED_ALERT, 0);

            slot      = (uint8_t)((slot + 1u) % NUM_SLOTS);
            remaining = DOSE_INTERVAL_S;
            last_tick = HAL_GetTick();
            state     = ST_WAIT;
        }
        break;
    }

    HAL_Delay(50);
*/
