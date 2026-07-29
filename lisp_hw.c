#include "lisp_hw.h"

#include "ch32fun.h"

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

void hw_delay_ms(int16_t ms)
{
    if (ms <= 0) {
        return;
    }

    /* Blocks the interpreter but not the picture: video is interrupt driven
     * and keeps running throughout. */
    Delay_Ms((uint32_t)ms);
}
