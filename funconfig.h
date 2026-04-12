#ifndef _FUNCONFIG_H
#define _FUNCONFIG_H

// CH32V003 PAL composite video + Tiny BASIC project
// System clock: 48 MHz (default)
// Enable single-wire ch32fun debug monitor input/output.
#define FUNCONF_USE_DEBUGPRINTF 1
#define FUNCONF_USE_UARTPRINTF  0

#define FUNCONF_USE_HSE 1
#define FUNCONF_USE_HSI 0
#define FUNCONF_USE_PLL 1

#endif
