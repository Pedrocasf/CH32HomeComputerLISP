#ifndef LISP_HW_H
#define LISP_HW_H

#include <stdint.h>

/* Hardware primitives behind a platform-independent interface, so lisp.c
 * stays free of ch32fun and can be unit tested on the host.
 *
 * Lisp pin numbering is flat:
 *
 *     0..7    PA0..PA7
 *     8..15   PC0..PC7   -- refused, see below
 *     16..23  PD0..PD7
 *
 * Port C is refused in full. The video interrupt rewrites the whole of
 * GPIOC->CFGLR on every scanline (video_textmode.c), so any user
 * configuration there is silently overwritten within microseconds; PC4 and
 * PC6 additionally carry sync and the pixel stream. PD1 is refused because
 * it is the single-wire debug line the console runs on.
 */

#define HW_IN     0     /* input, floating       */
#define HW_OUT    1     /* output, push-pull     */
#define HW_IN_PU  2     /* input, pull-up        */
#define HW_ANALOG 3     /* analog in, for hw_adc */

void    hw_init(void);

/* Pump the input path from inside a running program, so a break can be
 * noticed. Nothing else in the interpreter reads input while evaluating. */
void    hw_poll_input(void);

int8_t  hw_mode(int16_t pin, int16_t mode);   /* 0 ok, -1 unusable pin */
int8_t  hw_write(int16_t pin, int16_t value); /* 0 ok, -1 unusable pin */
int16_t hw_read(int16_t pin);                 /* 0, 1, or -1 unusable  */
int16_t hw_adc(int16_t channel);              /* 0..1023, or -1        */
void    hw_delay_ms(int16_t ms);

#endif
