#ifndef LISP_H
#define LISP_H

#include <stdint.h>

/* Value representation and tuning knobs. See LISP_DESIGN.md sections 2-3. */

/* Cell heap. 384 cells x 4 B = 1536 B. Raise toward 448-512 once the real
 * stack watermark has been measured under load (video ISRs nest on top of
 * the interpreter, and this project has a history of stack overflow). */
#ifndef NCELLS
#define NCELLS   384
#endif

/* Non-tail eval/print/read nesting allowed before the "deep" error.
 *
 * Sized from measured frames and then verified on hardware by painting the
 * unused stack and reading back the high-water mark. With NCELLS at 384 the
 * gap between the end of .bss and the top of stack is 1380 bytes; one level
 * of non-tail evaluation costs lisp_eval (36 B) plus lisp_eval_args (20 B),
 * and the video interrupt handlers (48 + 40 B) land on top of whatever the
 * interpreter is doing.
 *
 * Note this counts every eval entry, including the ones used to evaluate
 * arguments, so it is roughly a third larger than the depth of user
 * recursion it permits.
 *
 * The original value of 24 did not fit: it drove the stack into .bss, the
 * same failure the README reports for the BASIC runtime. Raising this
 * further means lowering NCELLS to buy the stack space back.
 */
#ifndef MAXDEPTH
#define MAXDEPTH 14
#endif

/* The collector treats a long symbol's {hi, lo} cell as raw data, but a
 * stale copy of a reference to one can still be picked up by the
 * conservative stack scan and traced as if it were a cons. Both halves are
 * always >= 1600, so the indices they would decode to (>= 400) must stay
 * outside the heap for that to be harmless. */
#if NCELLS > 400
#error "NCELLS > 400: raw SYM2 halves could decode to a valid cell index"
#endif

/* A val is a 16-bit tagged word:
 *   xxxxxxxxxxxxxxx1  fixnum, signed 15-bit payload    -16384..16383
 *   xxxxxxxxxxxxxx00  heap reference, cell index >> 2
 *   xxxxxxxxxxxxxx10  immediate (singleton / builtin / special form / marker)
 *
 * Cell 0 is never allocated, so NIL == 0: nil tests are a zero test and
 * zero-initialised memory is nil-safe.
 */
typedef uint16_t val;

typedef struct {
    val car;
    val cdr;
} cell;                                     /* 4 bytes */

#define NIL       ((val)0)

#define MKFIX(n)  ((val)((((uint16_t)(n)) << 1) | 1u))
#define FIXVAL(v) ((int16_t)(v) >> 1)       /* arithmetic shift keeps sign */
#define ISFIX(v)  ((v) & 1u)

#define MKREF(i)  ((val)((i) << 2))
#define REFIDX(v) ((v) >> 2)
#define ISREF(v)  ((((v) & 3u) == 0u) && (v) != NIL)

#define IMM(sub, pl) ((val)(((pl) << 4) | ((sub) << 2) | 2u))
#define ISIMM(v)     (((v) & 3u) == 2u)
#define IMMSUB(v)    (((v) >> 2) & 3u)
#define IMMPL(v)     ((v) >> 4)

/* Immediate subtypes */
#define SUB_SGL 0                           /* singletons                  */
#define SUB_FN  1                           /* builtin, payload = table idx */
#define SUB_SF  2                           /* special form, payload = idx  */
#define SUB_MK  3                           /* heap type markers            */

#define TEE      IMM(SUB_SGL, 0)
#define EOFV     IMM(SUB_SGL, 1)
#define MKFN(n)  IMM(SUB_FN, (n))
#define MKSF(n)  IMM(SUB_SF, (n))

/* Markers: the car of a heap cell that is not a cons */
#define SYM_MARK  IMM(SUB_MK, 0)            /* cdr = packed name, <= 3 chars */
#define CLO_MARK  IMM(SUB_MK, 1)            /* cdr = (params body . env)     */
#define FREE_MARK IMM(SUB_MK, 2)            /* cell is on the free list      */
#define SYM2_MARK IMM(SUB_MK, 3)            /* cdr -> {hi, lo}, 4..6 chars   */

void lisp_init(void);

/* Highest address the C stack occupies. The collector scans from its own
 * frame up to here looking for references, so this must be set once at
 * startup before any Lisp runs. */
void lisp_set_stack_top(void *top);

void lisp_print(val v);
void lisp_handle_input_line(const char *line);
void lisp_handle_screen(void);          /* Ctrl-R: run the whole screen */

#endif
