#include "lisp_hw.h"

#include "ch32fun.h"
#include "video_textmode.h"

/* Map a Lisp pin number to a ch32fun pin (PA_n = n, PC_n = 32+n, PD_n = 48+n),
 * or -1 if the pin is not ours to touch. See lisp_hw.h for why port C and
 * PD1 are refused. */
static int16_t hw_map(int16_t pin)
{
    if (pin < 0 || pin > 23) {
        return -1;
    }
    if (pin >= 8 && pin <= 15) {
        return -1;                  /* port C is owned by the video ISR */
    }
    if (pin == 17) {
        return -1;                  /* PD1 is the debug console */
    }

    return (pin < 8) ? pin : (int16_t)(48 + (pin - 16));
}

/* ADC brought up by hand rather than with funAnalogInit()/funAnalogRead().
 * Those follow the classic CH32V003 sequence: they calibrate via RSTCAL/CAL
 * and start conversions with ADC_SWSTART. On the V002 the conversion then
 * never completes and the machine hangs in the wait loop. This follows
 * ch32fun's own CH32V00x example instead, which starts with ADC_FLAG_STRT
 * and does not calibrate. */
void hw_init(void)
{
    funGpioInitAll();

#if defined(CH32V002) || defined(CH32V00x)
    /* Read-modify-write: the system clock configuration in CFGR0 must not be
     * disturbed, or the video timing goes with it. DIV2 gives ADCCLK 24 MHz
     * at 48 MHz HCLK. */
    RCC->CFGR0 = (RCC->CFGR0 & ~(uint32_t)(RCC_ADCPRE | RCC_CFGR0_ADC_CLK_MODE
                                         | RCC_CFGR0_ADC_CLK_ADJ))
               | RCC_ADCPRE_DIV2;

    RCC->PB2PCENR |= RCC_ADCEN;
    RCC->PB2PRSTR |= RCC_ADC1RST;
    RCC->PB2PRSTR &= ~RCC_ADC1RST;

    ADC1->CTLR2 |= ADC_ADON;
    ADC1->RSQR1 = 0;
    ADC1->RSQR2 = 0;
    ADC1->CTLR2 = ADC1->CTLR2 & ~ADC_EXTSEL_SWSTART;
#else
    /* Other families (the CH32V003 this project started on) keep ch32fun's
     * generic path: the register names above are V00x-only. */
    funAnalogInit();
#endif
}

int8_t hw_mode(int16_t pin, int16_t mode)
{
    int16_t p = hw_map(pin);

    if (p < 0) {
        return -1;
    }

    if (mode == HW_OUT) {
        funPinMode(p, GPIO_CFGLR_OUT_10Mhz_PP);
    }
    else if (mode == HW_ANALOG) {
        funPinMode(p, GPIO_CFGLR_IN_ANALOG);
    }
    else if (mode == HW_IN_PU) {
        funPinMode(p, GPIO_CFGLR_IN_PUPD);
        funDigitalWrite(p, FUN_HIGH);       /* ODR selects pull-up */
    }
    else {
        funPinMode(p, GPIO_CFGLR_IN_FLOAT);
    }

    return 0;
}

int8_t hw_write(int16_t pin, int16_t value)
{
    int16_t p = hw_map(pin);

    if (p < 0) {
        return -1;
    }

    funDigitalWrite(p, value ? FUN_HIGH : FUN_LOW);
    return 0;
}

int16_t hw_read(int16_t pin)
{
    int16_t p = hw_map(pin);

    if (p < 0) {
        return -1;
    }

    return (int16_t)(funDigitalRead(p) ? 1 : 0);
}

int16_t hw_adc(int16_t channel)
{
    uint32_t spins = 100000;            /* ~ms at 48 MHz; a conversion is us */

    if (channel < 0 || channel > 9) {
        return -1;
    }

    ADC1->RSQR3 = (uint32_t)channel;
    ADC1->SAMPTR2 = 0x00249249;         /* sample time 3 for every channel */
#if defined(CH32V002) || defined(CH32V00x)
    ADC1->CTLR2 |= ADC_FLAG_STRT;
#else
    ADC1->CTLR2 |= ADC_SWSTART;
#endif

    /* Bounded, deliberately: an unconfigured or absent channel must return
     * an error, never wedge the interpreter. This is a home computer -- a
     * builtin that can hang it is worse than one that can fail. */
    while (!(ADC1->STATR & ADC_EOC)) {
        if (--spins == 0) {
            return -1;
        }
    }

    /* 10-bit result, so it always fits a 15-bit fixnum. */
    return (int16_t)(ADC1->RDATAR & 0x3FF);
}

/* Saving the screen to flash.
 *
 * The screen is the program (see screen mode), so persisting the framebuffer
 * is persisting the source. It goes in the last kilobyte of the 16 KB flash,
 * well past the ~11.9 KB the firmware occupies; hw_save_screen refuses if
 * the image has grown into the region rather than erasing itself.
 *
 * Programming uses the fast path: 64-byte pages, erased and then written a
 * page at a time through the FPEC buffer. One header page carries a magic
 * word so load can tell "nothing saved" from "saved a blank screen", and 13
 * further pages hold the 800 bytes of text.
 *
 * Interrupts are left enabled. The video handler will be stalled for the few
 * milliseconds each erase and program takes, which shows as a brief tear in
 * the picture -- acceptable for something the user asked for explicitly, and
 * far cheaper than the RAM a flash routine relocated into it would cost.
 */
#define SAVE_MAGIC   0x50534931u        /* "1ISP" little-endian */
#define SAVE_ADDR    0x08003C00u        /* last 1 KB of 16 KB flash */
#define SAVE_LIMIT   0x00003C00u        /* the same, in the boot-alias view */
#define SAVE_PAGE    64u
#define SAVE_WORDS   (SAVE_PAGE / 4u)
#define SCREEN_BYTES ((uint16_t)TEXT_COLS * TEXT_ROWS)
#define SCREEN_PAGES ((SCREEN_BYTES + SAVE_PAGE - 1u) / SAVE_PAGE)
#define SAVE_PAGES   (SCREEN_PAGES + 1u)      /* header + data */

extern uint32_t _etext;

static void flash_unlock(void)
{
    FLASH->KEYR = FLASH_KEY1;
    FLASH->KEYR = FLASH_KEY2;
    FLASH->MODEKEYR = FLASH_KEY1;
    FLASH->MODEKEYR = FLASH_KEY2;
}

/* Erase and program are kept separate on purpose.
 *
 * Doing them per page, as the obvious loop does, silently produced an empty
 * region: an erase clears more than the 64-byte page it names, so each page
 * wiped the ones before it. Erasing the whole region up front and only then
 * programming pages avoids depending on the erase granularity at all.
 *
 * CTLR is returned to zero after each operation. Leaving the previous mode
 * bits set was the other half of the failure. */
static void flash_erase(uint32_t addr)
{
    FLASH->CTLR = CR_PAGE_ER;
    FLASH->ADDR = addr;
    FLASH->CTLR = CR_STRT_Set | CR_PAGE_ER;
    while (FLASH->STATR & FLASH_STATR_BSY) {
    }
    FLASH->CTLR = 0;
}

static void flash_program(uint32_t addr, const uint32_t *src)
{
    volatile uint32_t *dst = (volatile uint32_t *)addr;
    uint8_t i;

    FLASH->CTLR = CR_PAGE_PG;
    FLASH->CTLR = CR_BUF_RST | CR_PAGE_PG;
    FLASH->ADDR = addr;

    for (i = 0; i < SAVE_WORDS; i++) {
        dst[i] = src[i];
        FLASH->CTLR = CR_PAGE_PG | CR_BUF_LOAD;
        while (FLASH->STATR & FLASH_STATR_BSY) {
        }
    }

    FLASH->CTLR = CR_PAGE_PG | CR_STRT_Set;
    while (FLASH->STATR & FLASH_STATR_BSY) {
    }
    FLASH->CTLR = 0;
}

int8_t hw_save_screen(void)
{
    uint32_t buf[SAVE_WORDS];
    uint16_t idx = 0;
    uint8_t page, w, b;

    /* Refuse rather than erase code: the region is fixed, but the firmware
     * that must stay clear of it is not. */
    if ((uint32_t)(uintptr_t)&_etext > SAVE_LIMIT) {
        return -1;
    }

    flash_unlock();
    if (FLASH->CTLR & 0x8080u) {
        return -1;                      /* still locked */
    }

    for (page = 0; page < SAVE_PAGES; page++) {
        flash_erase(SAVE_ADDR + SAVE_PAGE * page);
    }

    for (page = 0; page < SCREEN_PAGES; page++) {
        for (w = 0; w < SAVE_WORDS; w++) {
            uint32_t v = 0;

            for (b = 0; b < 4; b++) {
                uint8_t c = ' ';

                if (idx < SCREEN_BYTES) {
                    c = video_textmode_read_cell((uint8_t)(idx % TEXT_COLS),
                                                 (uint8_t)(idx / TEXT_COLS));
                    idx++;
                }
                v |= (uint32_t)c << (8 * b);
            }
            buf[w] = v;
        }
        flash_program(SAVE_ADDR + SAVE_PAGE * (page + 1u), buf);
    }

    /* The magic goes in last. If anything above fails or is interrupted the
     * header stays erased, so a half-written save reads as no save at all
     * rather than as a screenful of noise. */
    buf[0] = SAVE_MAGIC;
    for (w = 1; w < SAVE_WORDS; w++) {
        buf[w] = 0;
    }
    flash_program(SAVE_ADDR, buf);

    return (*(const volatile uint32_t *)SAVE_ADDR == SAVE_MAGIC) ? 0 : -1;
}

int8_t hw_load_screen(void)
{
    const uint8_t *src = (const uint8_t *)(SAVE_ADDR + SAVE_PAGE);
    uint16_t idx;

    if (*(const uint32_t *)SAVE_ADDR != SAVE_MAGIC) {
        return -1;                      /* nothing has been saved */
    }

    for (idx = 0; idx < SCREEN_BYTES; idx++) {
        uint8_t c = src[idx];

        if (c < ' ' || c > '~') {
            c = ' ';                    /* never write junk to the screen */
        }
        video_textmode_write_cell((uint8_t)(idx % TEXT_COLS),
                                  (uint8_t)(idx / TEXT_COLS), c);
    }

    return 0;
}

void hw_poll_input(void)
{
    /* Reaches handle_debug_input, which routes bytes to
     * lisp_handle_run_control_byte while a program is running -- so this
     * cannot re-enter the interpreter. */
    poll_input();
}

void hw_delay_ms(int16_t ms)
{
    if (ms <= 0) {
        return;
    }

    /* Blocks the interpreter but not the picture: video is interrupt driven
     * and keeps running throughout. */
    Delay_Ms((uint32_t)ms);
}
