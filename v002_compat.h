/* CH32V002 TIM1 register-bit compatibility shim.
 *
 * video_textmode.c is written against ch32v003hw.h, which exposes the TIM
 * bit masks unprefixed (TIM_CC4E). The CH32V002 pulls in ch32x00xhw.h
 * instead (see ch32fun.h), which names the same bits, with the same values,
 * peripheral-prefixed (TIM1_CCER_CC4E).
 *
 * Force-included from the Makefile via EXTRA_CFLAGS so no project source
 * needs to carry per-target #ifdefs. Inert on CH32V003, which defines
 * neither CH32V002 nor CH32V00x.
 */
#ifndef V002_COMPAT_H
#define V002_COMPAT_H

#if defined(CH32V002) || defined(CH32V00x)

/* TIM1->INTFR */
#define TIM_UIF     TIM1_INTFR_UIF
#define TIM_CC3IF   TIM1_INTFR_CC3IF

/* TIM1->DMAINTENR */
#define TIM_UIE     TIM1_DMAINTENR_UIE
#define TIM_CC3IE   TIM1_DMAINTENR_CC3IE

/* TIM1->CCER */
#define TIM_CC4E    TIM1_CCER_CC4E
#define TIM_CC4P    TIM1_CCER_CC4P

/* TIM1->CHCTLR2 */
#define TIM_OC4PE   TIM1_CHCTLR2_OC4PE
#define TIM_OC4M_1  TIM1_CHCTLR2_OC4M_1
#define TIM_OC4M_2  TIM1_CHCTLR2_OC4M_2

/* TIM1->BDTR / CTLR1 / SWEVGR */
#define TIM_MOE     TIM1_BDTR_MOE
#define TIM_ARPE    TIM1_CTLR1_ARPE
#define TIM_CEN     TIM1_CTLR1_CEN
#define TIM_UG      TIM1_SWEVGR_UG

#endif /* CH32V002 || CH32V00x */
#endif /* V002_COMPAT_H */
