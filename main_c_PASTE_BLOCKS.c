// ======================================================================
//  main.c PASTE BLOCKS  -  CMPE3001 Medication Dispenser (FINAL v2)
//  5 blocks. For each: find the matching BEGIN marker line in your
//  generated main.c, DELETE what you pasted there before, and paste the
//  new code UNDER the BEGIN marker (above its END marker).
//  Banners are // comments - harmless if one gets copied.
//
//  BUTTONS:  B0 = dose taken (patient)
//            B1 = change PIN (needs old PIN)
//            B2 = carer override (needs PIN) - give or skip a dose
//            B3 = restart from scratch (demo) - no PIN
//  Two PINs (must be different):
//    CARER PIN   - override (B2), refill, change own PIN
//    PATIENT PIN - unlock at start-up, change own PIN
// ======================================================================

// ==================================================================
// BLOCK 1 of 5  -  includes
// SECTION Includes  ->  paste UNDER its BEGIN marker line
//                   and ABOVE its END marker line
// ==================================================================
#include "LCD1602_I2C.h"
#include "oximeter.h"
#include "keypad.h"
#include "panel.h"
#include <stdio.h>
#include <string.h>


// ==================================================================
// BLOCK 2 of 5  -  defines
// SECTION PD  ->  paste UNDER its BEGIN marker line
//                   and ABOVE its END marker line
// ==================================================================
/* ---- Servo carousel (Lab 3 maths: CCR = 25 + 0.556 x angle) ---- */
#define SERVO_SLOT1       25u         /* 0 deg    - morning  */
#define SERVO_SLOT2       58u         /* ~60 deg  - noon     */
#define SERVO_SLOT3       92u         /* ~120 deg - evening  */
#define SERVO_SLOT4       125u        /* 180 deg  - night    */
#define SERVO_HOME        75u         /* 90 deg - rest between compartments */
#define NUM_SLOTS         4u

/* ---- Timing (seconds). Short values so the demo runs quickly. ---- */
#define DOSE_INTERVAL_S   5u          /* gap between doses                  */
#define CONFIRM_WINDOW_S  10u         /* time to press "taken" (B0)         */
#define HELD_TIMEOUT_S    30u         /* carer has this long, then withheld */

/* ---- Heart-rate safe band (bpm) ---- */
#define HR_MIN            50
#define HR_MAX            120

/* ---- Vitals ----
 * The sensor needs ~4-8 s to work out a pulse, then gives a new result about
 * every 4 s and HOLDS the last one in between - even for a few seconds after
 * the finger is lifted. We sample once a second and accept a reading once it
 * is steady AND has stayed valid long enough:
 *   QUICK_HOLD_MS   - normal reading that appeared after we started
 *   CONFIRM_HOLD_MS - a number was already showing when we started (could be
 *                     a leftover), or the value is abnormal (could be the
 *                     pulse "doubling" glitch): wait past one sensor update. */
#define POLL_MS           250u
#define SAMPLE_MS         1000u       /* read the sensor once a second      */
#define HIST_N            2           /* samples that must agree            */
#define HR_SPREAD_MAX     12          /* max spread across them (bpm)       */
#define QUICK_HOLD_MS     1000u
#define CONFIRM_HOLD_MS   6000u
#define VITALS_TIMEOUT_MS 20000u      /* give up after 20 s -> "No vitals"  */

/* ---- PIN ---- */
#define PIN_LEN           4
#define PIN_SCAN_MS       8u

/* ---- LEDs (base LEDs all RED; LD2 is the on-board GREEN) ---- */
#define LED_OK            3u          /* PA5 -> on-board GREEN LD2: OK      */
#define LED_ALERT         0u          /* PA1 -> base-board RED LED: alert   */

/* ---- Buttons (active low; board has 10k pull-ups) ---- */
#define CONFIRM_PORT      GPIOB
#define CONFIRM_PIN       GPIO_PIN_3  /* B0 (D3):  dose taken           */
#define CHANGE_PORT       GPIOB
#define CHANGE_PIN        GPIO_PIN_6  /* B1 (D10): change PIN           */
#define OVERRIDE_PORT     GPIOA
#define OVERRIDE_PIN      GPIO_PIN_7  /* B2 (D11): carer override       */
#define RESTART_PORT      GPIOA
#define RESTART_PIN       GPIO_PIN_6  /* B3 (D12): restart (demo)       */


// ==================================================================
// BLOCK 3 of 5  -  helper functions (outside main)
// SECTION 0  ->  paste UNDER its BEGIN marker line
//                   and ABOVE its END marker line
// ==================================================================
/* ---- two PINs, held in RAM (set fresh at every power-up) ----
 * CARER PIN  : carer override, refill, change own PIN
 * PATIENT PIN: unlock the dispenser at start-up, change own PIN
 * They must be different, so knowing one never gives the other's rights. */
static char carer_pin[PIN_LEN + 1]   = "";
static char patient_pin[PIN_LEN + 1] = "";

#define ROLE_NONE         0u
#define ROLE_PATIENT      1u
#define ROLE_CARER        2u

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

/* B3 = restart the whole machine from scratch (no PIN). Called from every
 * waiting loop so it works on any screen. A full MCU reset clears the PIN
 * (it lives in RAM), so the device comes back to "Set new PIN". */
static void poll_restart(void)
{
    if (btn_down(RESTART_PORT, RESTART_PIN))
    {
        Panel_SetLed(LED_ALERT, 0);
        Panel_SetLed(LED_OK, 0);
        show2("Restarting...", "");
        HAL_Delay(500);
        HAL_NVIC_SystemReset();
    }
}

/* ---- servo ---- */
static void servo_set(uint16_t ccr)
{
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, ccr);
}

/* Servo PWM set-up done in code, so the servo works even if the CubeMX
 * clock or TIM2 settings are off: PA0 = TIM2_CH1, PWM mode 1, and a
 * 50 Hz frame with 20 us per count, worked out from the real timer clock.
 * (At 80 MHz this gives exactly the Lab 3 values: PSC 1599, ARR 999.) */
static void servo_init(void)
{
    TIM_OC_InitTypeDef oc = {0};
    GPIO_InitTypeDef   g  = {0};
    uint32_t timclk = HAL_RCC_GetPCLK1Freq();

    if ((RCC->CFGR & RCC_CFGR_PPRE1_2) != 0u) { timclk *= 2u; }  /* APB1 divided -> timer clock x2 */

    oc.OCMode     = TIM_OCMODE_PWM1;
    oc.Pulse      = SERVO_HOME;
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;
    HAL_TIM_PWM_ConfigChannel(&htim2, &oc, TIM_CHANNEL_1);

    __HAL_TIM_SET_PRESCALER(&htim2, (timclk / 50000u) - 1u);   /* 50 kHz tick = 20 us */
    __HAL_TIM_SET_AUTORELOAD(&htim2, 999u);                     /* 1000 ticks = 20 ms  */
    htim2.Instance->EGR = TIM_EGR_UG;                           /* load the new values */

    __HAL_RCC_GPIOA_CLK_ENABLE();                               /* PA0 -> TIM2_CH1     */
    g.Pin       = GPIO_PIN_0;
    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_LOW;
    g.Alternate = GPIO_AF1_TIM2;
    HAL_GPIO_Init(GPIOA, &g);

    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);
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

/* Rotate the due compartment over the drop hole, pause, return to rest. */
static void dispense_from_slot(uint8_t slot)
{
    servo_set(slot_ccr(slot));
    HAL_Delay(1500);                 /* compartment aligned - pill drops */
    servo_set(SERVO_HOME);
    HAL_Delay(400);                  /* back to rest */
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

        poll_restart();

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

/* Red LED + two-line warning for 1.5 s, then LED off. */
static void pin_warn(const char *l1, const char *l2)
{
    Panel_SetLed(LED_ALERT, 1);
    show2(l1, l2);
    HAL_Delay(1500);
    Panel_SetLed(LED_ALERT, 0);
}

/* Set a new PIN into dest (typed twice). It may not equal the old value
 * of dest, nor the other person's PIN. */
static void set_new_pin(char *dest, const char *other, const char *prompt)
{
    char a[PIN_LEN + 1], b[PIN_LEN + 1];

    for (;;)
    {
        enter_pin(prompt, a);

        if (dest[0] != '\0' && strncmp(a, dest, PIN_LEN) == 0)
        {
            pin_warn("Same as old PIN", "Choose another");
            continue;
        }
        if (other[0] != '\0' && strncmp(a, other, PIN_LEN) == 0)
        {
            pin_warn("PIN already used", "Choose another");
            continue;
        }

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
        pin_warn("PIN mismatch", "Try again");
    }
}

/* Ask for a PIN and work out whose it is. */
static uint8_t check_pin(const char *prompt)
{
    char e[PIN_LEN + 1];
    enter_pin(prompt, e);
    if (strncmp(e, carer_pin,   PIN_LEN) == 0) { return ROLE_CARER;   }
    if (strncmp(e, patient_pin, PIN_LEN) == 0) { return ROLE_PATIENT; }
    return ROLE_NONE;
}

/* Unlock at start-up: the patient (or carer) PIN. */
static void login(void)
{
    for (;;)
    {
        if (check_pin("Enter PIN:") != ROLE_NONE)
        {
            Panel_SetLed(LED_OK, 1);
            show2("Unlocked", "");
            HAL_Delay(900);
            Panel_SetLed(LED_OK, 0);
            return;
        }
        pin_warn("Wrong PIN", "Try again");
    }
}

/* Keep asking until the CARER PIN is entered (patient PIN not accepted). */
static void require_carer(const char *prompt, const char *ok_msg)
{
    for (;;)
    {
        if (check_pin(prompt) == ROLE_CARER)
        {
            Panel_SetLed(LED_OK, 1);
            show2(ok_msg, "");
            HAL_Delay(900);
            Panel_SetLed(LED_OK, 0);
            return;
        }
        pin_warn("Carer PIN only", "Try again");
    }
}

/* B1: change a PIN. The old PIN says whose PIN is being changed. */
static void change_pin(void)
{
    uint8_t who = check_pin("Old PIN:");

    if      (who == ROLE_CARER)   { set_new_pin(carer_pin,   patient_pin, "New carer PIN:");   }
    else if (who == ROLE_PATIENT) { set_new_pin(patient_pin, carer_pin,   "New patient PIN:"); }
    else                          { pin_warn("Wrong PIN", "Not changed"); }
}

/* Carer override. Only the CARER PIN is accepted, so the patient cannot
 * override it themselves. Returns 1 = give, 0 = skip, -1 = not authorised. */
static int8_t carer_decision(void)
{
    if (check_pin("Carer PIN:") != ROLE_CARER)
    {
        pin_warn("Not authorised", "Carer PIN only");
        return -1;
    }

    show2("Carer: 1 = Give", "        2 = Skip");
    for (;;)
    {
        char k = Keypad_Scan();
        poll_restart();
        if (k == '1') { return 1; }
        if (k == '2') { return 0; }
        HAL_Delay(PIN_SCAN_MS);
    }
}

/* All compartments used: show the log, then only the CARER can confirm
 * the refill and restart the cycle. */
static void refill_gate(uint32_t taken)
{
    char l2[17];

    servo_set(SERVO_HOME);
    snprintf(l2, sizeof(l2), "Taken %lu of %u", (unsigned long)taken, (unsigned)NUM_SLOTS);
    show2("All doses done", l2);
    HAL_Delay(3000);

    require_carer("Carer PIN:", "Refilled");
}

/* ===== Heart-rate stability filter (idea from the §9 reference app) =====
 * A reading is only trusted once HIST_N fresh values agree within
 * HR_SPREAD_MAX bpm - this rejects the PPG doubling artefact. */
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

/* One vitals measurement. Returns a settled HR (bpm), or -1 on timeout or
 * '#' skip. *spo2_out gets the matching SpO2 (or -1). See the Vitals notes
 * in the defines for why the hold times differ. */
static int32_t measure_vitals(int32_t *spo2_out)
{
    uint32_t   start       = HAL_GetTick();
    uint32_t   last_sample = 0;
    uint32_t   valid_since = 0;
    uint8_t    valid       = 0;      /* sensor currently giving a reading   */
    uint8_t    leftover    = 0;      /* a number was showing when we began  */
    uint8_t    first       = 1;
    uint8_t    spin        = 0;
    OxiReading rd;
    Hist       hr_hist;
    int32_t    last_spo2   = -1;
    static const char *dots[4] = { "   ", ".  ", ".. ", "..." };

    hist_reset(&hr_hist);
    *spo2_out = -1;

    if (Oxi_Read(&rd) == HAL_OK && rd.heartbeat > 0) { leftover = 1u; }

    show2("Dose due!", "Place finger...");

    while ((HAL_GetTick() - start) < VITALS_TIMEOUT_MS)
    {
        uint32_t now = HAL_GetTick();
        char     l2[17];

        if (first || (now - last_sample) >= SAMPLE_MS)      /* once a second */
        {
            first       = 0u;
            last_sample = now;
            if (Oxi_Read(&rd) == HAL_OK && rd.heartbeat > 0)
            {
                if (!valid) { valid = 1u; valid_since = now; }
                hist_push(&hr_hist, rd.heartbeat);
                if (rd.spo2 > 0) { last_spo2 = rd.spo2; }
            }
            else                                 /* no finger / no result yet */
            {
                valid    = 0u;
                leftover = 0u;                   /* anything after this is new */
                hist_reset(&hr_hist);
            }
        }

        if (valid)
        {
            int32_t settled = hist_stable(&hr_hist, HR_SPREAD_MAX);
            if (settled > 0)
            {
                uint8_t  normal = (uint8_t)(settled >= HR_MIN && settled <= HR_MAX);
                uint32_t need   = (leftover || !normal) ? CONFIRM_HOLD_MS : QUICK_HOLD_MS;
                if ((now - valid_since) >= need)
                {
                    *spo2_out = last_spo2;
                    return settled;
                }
            }
            snprintf(l2, sizeof(l2), "Hold still%s", dots[spin & 3u]);
        }
        else
        {
            snprintf(l2, sizeof(l2), "Place finger%s", dots[spin & 3u]);
        }

        show2("Dose due!", l2);
        spin++;

        {   /* wait POLL_MS in 10 ms slices so '#' and B3 respond quickly */
            uint16_t t;
            for (t = 0; t < (POLL_MS / 10u); t++)
            {
                poll_restart();
                if (Keypad_Scan() == '#') { return -1; }
                HAL_Delay(10);
            }
        }
    }
    return -1;
}


// ==================================================================
// BLOCK 4 of 5  -  startup + loop variables (inside main, runs once)
// SECTION 2  ->  paste UNDER its BEGIN marker line
//                   and ABOVE its END marker line
// ==================================================================
    /* if B3 is still held from a restart, wait for it to be let go */
    while (btn_down(RESTART_PORT, RESTART_PIN)) { HAL_Delay(10); }

    lcd_init(&hi2c1);
    show2("Med Dispenser", "Starting...");
    HAL_Delay(800);

    Keypad_Init();
    Panel_Init();
    Panel_SetLed(LED_OK, 0);
    Panel_SetLed(LED_ALERT, 0);

    servo_init();                               // servo PWM on PA0 (TIM2_CH1, 50 Hz)

    /* Servo self-test: visit all 4 compartments, then rest at 90 deg.
     * If the servo does not move HERE, check TIM2 / clock / wiring. */
    show2("Servo check", "Slots 1 - 4");
    {
        uint8_t s;
        for (s = 0; s < NUM_SLOTS; s++) { servo_set(slot_ccr(s)); HAL_Delay(700); }
    }
    servo_set(SERVO_HOME);
    HAL_Delay(700);

    set_new_pin(carer_pin,   patient_pin, "New carer PIN:");    // carer sets up first
    set_new_pin(patient_pin, carer_pin,   "New patient PIN:");  // then the patient's PIN
    login();                                                    // unlock with a PIN

    Oxi_Start(&hi2c3);                          // start heart-rate acquisition
    HAL_Delay(500);
    show2("Dispenser ready", "Next dose soon");

    /* ---- loop variables ---- */
    typedef enum { ST_WAIT, ST_MEASURE, ST_CHECK, ST_HELD, ST_GIVE,
                   ST_CONFIRM, ST_NEXT, ST_REFILL } DispState;
    DispState  state       = ST_WAIT;
    uint32_t   last_tick   = HAL_GetTick();
    uint32_t   remaining   = DOSE_INTERVAL_S;
    uint8_t    slot        = 0;
    uint32_t   doses_taken = 0;
    int32_t    hr = -1, spo2 = -1;
    uint32_t   confirm_start = 0;
    char       held_msg[17] = "";
    uint32_t   held_start = 0;
    uint32_t   held_shown = 0xFFFFFFFFu;


// ==================================================================
// BLOCK 5 of 5  -  main loop body (inside while(1))
// SECTION 3  ->  paste UNDER its BEGIN marker line
//                   and ABOVE its END marker line
// ==================================================================
// !! WARNING: CubeMX puts the  }  that closes while(1) INSIDE this
// !! section, just above the END 3 marker. KEEP IT. After pasting,
// !! the bottom must read:   HAL_Delay(50);  then  }  then END 3.
    poll_restart();                              // B3 works on every screen

    switch (state)
    {
    // ---- 1. idle: count down. B1 = change PIN, B2 = carer override ----
    case ST_WAIT:
        if (btn_down(CHANGE_PORT, CHANGE_PIN))
        {
            change_pin();
            while (btn_down(CHANGE_PORT, CHANGE_PIN)) { HAL_Delay(10); }
            last_tick = HAL_GetTick();
            show2("Dispenser ready", "Next dose soon");
            break;
        }
        if (btn_down(OVERRIDE_PORT, OVERRIDE_PIN))   // carer: give/skip now
        {
            int8_t d;
            while (btn_down(OVERRIDE_PORT, OVERRIDE_PIN)) { HAL_Delay(10); }
            d = carer_decision();
            if      (d == 1) { state = ST_GIVE; }
            else if (d == 0) { show2("Dose skipped", "by carer"); HAL_Delay(1500); state = ST_NEXT; }
            else             { last_tick = HAL_GetTick(); show2("Dispenser ready", ""); }
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
        state = ST_CHECK;
        break;

    // ---- 3. vitals SAFETY GATE ----
    case ST_CHECK:
        if (hr >= HR_MIN && hr <= HR_MAX)            // vitals OK -> give
        {
            char l1[17];
            Panel_SetLed(LED_OK, 1);
            snprintf(l1, sizeof(l1), "HR %ld SpO2 %ld%%", (long)hr, (long)spo2);
            show2(l1, "Vitals OK");
            HAL_Delay(2000);
            Panel_SetLed(LED_OK, 0);
            state = ST_GIVE;
            break;
        }

        if (hr < 0)          { strcpy(held_msg, "No vitals"); }
        else if (hr < HR_MIN) { snprintf(held_msg, sizeof(held_msg), "HR %ld LOW", (long)hr); }
        else                  { snprintf(held_msg, sizeof(held_msg), "HR %ld HIGH", (long)hr); }

        Panel_SetLed(LED_ALERT, 1);
        show2(held_msg, "Call carer (B2)");
        held_start = HAL_GetTick();                   // start the carer window
        held_shown = 0xFFFFFFFFu;
        state = ST_HELD;                              // dose is HELD
        break;

    // ---- 4. dose held: carer has HELD_TIMEOUT_S to override (B2 + PIN) ----
    case ST_HELD:
    {
        uint32_t gone = (HAL_GetTick() - held_start) / 1000u;

        Panel_SetLed(LED_ALERT, 1);

        if (gone >= HELD_TIMEOUT_S)                   // nobody came -> withhold
        {
            show2("Dose withheld", "Carer alerted");
            HAL_Delay(2000);
            Panel_SetLed(LED_ALERT, 0);
            state = ST_NEXT;
            break;
        }

        if (gone != held_shown)                       // countdown, once a second
        {
            char l1[17];
            snprintf(l1, sizeof(l1), "%-12.12s%2us", held_msg,
                     (unsigned)((HELD_TIMEOUT_S - gone) % 100u));
            show2(l1, "Call carer (B2)");
            held_shown = gone;
        }

        if (btn_down(OVERRIDE_PORT, OVERRIDE_PIN))
        {
            int8_t d;
            while (btn_down(OVERRIDE_PORT, OVERRIDE_PIN)) { HAL_Delay(10); }
            d = carer_decision();
            Panel_SetLed(LED_ALERT, 0);
            if      (d == 1) { state = ST_GIVE; }
            else if (d == 0) { show2("Dose skipped", "by carer"); HAL_Delay(1500); state = ST_NEXT; }
            else             { held_shown = 0xFFFFFFFFu; }   // wrong PIN: still held
        }
        break;
    }

    // ---- 5. dispense from the due compartment ----
    case ST_GIVE:
    {
        char l1[17];
        Panel_SetLed(LED_ALERT, 0);
        snprintf(l1, sizeof(l1), "Dispensing S%u", (unsigned)(slot + 1u));
        show2(l1, "Please take it");
        dispense_from_slot(slot);
        show2("Press B0 when", "dose is taken");
        confirm_start = HAL_GetTick();
        state = ST_CONFIRM;
        break;
    }

    // ---- 6. wait for "taken" (B0), else dose missed ----
    case ST_CONFIRM:
        if (btn_down(CONFIRM_PORT, CONFIRM_PIN))
        {
            doses_taken++;
            Panel_SetLed(LED_OK, 1);
            show2("Dose confirmed", "Thank you");
            HAL_Delay(1500);
            Panel_SetLed(LED_OK, 0);
            state = ST_NEXT;
        }
        else if ((HAL_GetTick() - confirm_start) >= (CONFIRM_WINDOW_S * 1000u))
        {
            uint8_t f;
            show2("Dose missed", "");
            for (f = 0; f < 6u; f++)                 // flash red 3 times
            {
                Panel_SetLed(LED_ALERT, (uint8_t)((f & 1u) == 0u));
                HAL_Delay(300);
            }
            Panel_SetLed(LED_ALERT, 0);              // always ends OFF
            state = ST_NEXT;
        }
        break;

    // ---- 7. move to the next compartment (or refill after the 4th) ----
    case ST_NEXT:
        slot      = (uint8_t)((slot + 1u) % NUM_SLOTS);
        remaining = DOSE_INTERVAL_S;
        last_tick = HAL_GetTick();
        if (slot == 0u)
        {
            state = ST_REFILL;
        }
        else
        {
            show2("Dispenser ready", "Next dose soon");
            state = ST_WAIT;
        }
        break;

    // ---- 8. all 4 compartments used -> refill (PIN required) ----
    case ST_REFILL:
        refill_gate(doses_taken);
        doses_taken = 0;
        slot        = 0;
        remaining   = DOSE_INTERVAL_S;
        last_tick   = HAL_GetTick();
        show2("Dispenser ready", "Next dose soon");
        state       = ST_WAIT;
        break;
    }

    HAL_Delay(50);
