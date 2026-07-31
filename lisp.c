#include "lisp.h"

#include "console_textmode.h"
#include "video_textmode.h"
#include "lisp_hw.h"

static void lisp_error(const char *msg);

/* First error raised while handling the current line, or NULL. A flag rather
 * than setjmp: nothing needs unwinding, and setjmp stays out of the image. */
static const char *lisp_err;

/* Heap and globals (LISP_DESIGN.md section 2)
 *
 * Cell 0 is never allocated, so index 0 doubles as "none": MKREF(0) is NIL,
 * which the collector relies on for its parent-link sentinel. */
static cell heap[NCELLS];
static uint16_t free_list;          /* head of the free chain, 0 = empty */
static uint16_t free_count;
static val global_env = NIL;
static uint8_t eval_depth;

/* Forms read from the screen but not yet evaluated (section 5). Marked as a
 * root so a collection between forms cannot reclaim the rest of the
 * program. */
static val lisp_pending = NIL;

static void lisp_gc(val extra1, val extra2);

/* Screen mode (section 5): the framebuffer is the source buffer, so the
 * reader can take characters straight from video RAM instead of a line.
 * Rows are space-padded, so a row boundary reads as whitespace and a form
 * may span rows freely; a token split across the 32-column edge does not
 * survive, which matches how screen editors have always behaved. */
static uint8_t lisp_from_screen;
static uint8_t lisp_scr_x;
static uint8_t lisp_scr_y;

/* Break support.
 *
 * Nothing reads input while a program evaluates -- poll_input() runs in the
 * foreground loop, and during a run the foreground is inside eval -- so the
 * evaluator has to pump the input path itself. Without that, Esc can never
 * arrive, and a tail-recursive loop runs in constant stack and constant heap
 * and therefore forever: `(define l (lambda () (l)))` would need a reset.
 *
 * While lisp_running is set, main.c routes incoming bytes to
 * lisp_handle_run_control_byte instead of the console, so polling here
 * cannot re-enter the interpreter. */
static uint8_t lisp_running;
static volatile uint8_t lisp_break;
static uint8_t lisp_poll_tick;

uint8_t lisp_is_running(void)
{
    return lisp_running;
}

void lisp_handle_run_control_byte(uint8_t ch)
{
    if (ch == 0x1b) {                   /* Esc */
        lisp_break = 1;
    }
}

void lisp_init(void)
{
    uint16_t i;

    /* Thread every cell but 0 onto the free list, lowest index first. */
    free_list = 0;
    free_count = 0;
    for (i = NCELLS - 1; i >= 1; i--) {
        heap[i].car = FREE_MARK;
        heap[i].cdr = MKREF(free_list);
        free_list = i;
        free_count++;
    }

    global_env = NIL;
    eval_depth = 0;
    lisp_err = 0;
    lisp_pending = NIL;
    lisp_from_screen = 0;
    hw_init();
}

/* Allocation pops the free list, collecting first if it is empty.
 *
 * Unlike the previous copying collector there is no safepoint discipline: a
 * collection may happen at any allocation, because the mark phase scans the
 * C stack (see lisp_gc) and so finds every local a caller is holding --
 * including half-built structures such as the reader's list. */
static val lisp_cons(val a, val d)
{
    uint16_t i;

    if (free_list == 0) {
        /* a and d are live but not yet reachable from anything, so they are
         * marked explicitly rather than relied upon being spilled. */
        lisp_gc(a, d);

        if (free_list == 0) {
            /* Report exhaustion here rather than returning a bare NIL:
             * callers that missed the check would otherwise carry a broken
             * environment forward and fail later with a misleading "type"
             * error. */
            lisp_error("mem");
            return NIL;
        }
    }

    i = free_list;
    free_list = REFIDX(heap[i].cdr);
    free_count--;

    heap[i].car = a;
    heap[i].cdr = d;
    return MKREF(i);
}

/* A ref whose car is a type marker is a symbol/closure, not a cons. */
static uint8_t lisp_is_cons(val v)
{
    val a;

    if (!ISREF(v)) {
        return 0;
    }

    a = heap[REFIDX(v)].car;
    return !(ISIMM(a) && IMMSUB(a) == SUB_MK);
}

/* Symbols: radix-40 packed names (section 3)
 *
 * Three characters from a 40-character alphabet pack into 16 bits
 * (40^3 = 64000 <= 65536), so a symbol costs one cell, needs no string
 * storage or intern table, and eq is an integer compare. Code 0 is the pad
 * for short names; char40() never returns 0 for a real character, so the
 * encoding stays injective and "ab" cannot collide with "aba".
 */
static const char lisp_alphabet[] = "\0abcdefghijklmnopqrstuvwxyz0123456789-*$";

static int8_t char40(char c)
{
    if (c >= 'a' && c <= 'z') {
        return (int8_t)(c - 'a' + 1);
    }
    if (c >= '0' && c <= '9') {
        return (int8_t)(c - '0' + 27);
    }
    if (c == '-') {
        return 37;
    }
    if (c == '*') {
        return 38;
    }
    if (c == '$') {
        return 39;
    }
    return -1;
}

/* Pack a 1..3 character group, first character most significant. Returns the
 * 16-bit value, or -1 if the group is too long or uses a character outside
 * the alphabet. Telling numbers from symbols is the tokeniser's job. */
static int32_t lisp_pack3(const char *s, uint8_t len)
{
    uint16_t n = 0;
    int8_t d;

    if (len < 1 || len > 3) {
        return -1;
    }

    for (uint8_t i = 0; i < 3; i++) {
        d = (i < len) ? char40(s[i]) : 0;
        if (d < 0) {
            return -1;
        }
        n = (uint16_t)(n * 40 + (uint16_t)d);
    }

    return n;
}

/* Pad codes decode to '\0', so short names terminate themselves.
 *
 * The constant divisors here compile to __udivsi3/__umodsi3 calls, not to
 * multiplies: gcc only strength-reduces constant division at -O2, and
 * ch32fun builds at -Os (measured, see LISP_DESIGN.md 1.1a). That costs
 * nothing in flash because the firmware already links those routines, and
 * this is not a hot path. */
static void lisp_unpack3(uint16_t n, char out[4])
{
    out[0] = lisp_alphabet[n / 1600u];
    out[1] = lisp_alphabet[(n / 40u) % 40u];
    out[2] = lisp_alphabet[n % 40u];
    out[3] = '\0';
}

/* Names of 4..6 characters are split across two cells (section 3):
 *
 *   {SYM2_MARK, ref} -> {hi, lo}
 *
 * The second cell holds two raw packed groups, not tagged values. It is
 * reachable only through the SYM2 cell's cdr and is never handed to user
 * code, but the collector must copy it as a leaf rather than scanning its
 * fields as references.
 *
 * A group's first character is always a real character, never a pad, so both
 * hi and lo are >= 1600 and can never be mistaken for a marker immediate
 * (SYM_MARK is 14, the largest marker is 46). lo == 0 is therefore an
 * unambiguous "this name fits in three characters".
 */
static uint8_t lisp_pack_name(const char *s, uint8_t len, uint16_t *hi, uint16_t *lo)
{
    int32_t a, b;

    if (len < 1 || len > 6) {
        return 0;
    }

    a = lisp_pack3(s, (uint8_t)(len < 3 ? len : 3));
    if (a < 0) {
        return 0;
    }
    *hi = (uint16_t)a;

    if (len <= 3) {
        *lo = 0;
        return 1;
    }

    b = lisp_pack3(s + 3, (uint8_t)(len - 3));
    if (b < 0) {
        return 0;
    }
    *lo = (uint16_t)b;
    return 1;
}

/* The two halves of a symbol's name; lo is 0 for a short symbol. */
static uint16_t lisp_sym_hi(val s)
{
    val d = heap[REFIDX(s)].cdr;
    return (heap[REFIDX(s)].car == SYM_MARK) ? d : heap[REFIDX(d)].car;
}

static uint16_t lisp_sym_lo(val s)
{
    val d = heap[REFIDX(s)].cdr;
    return (heap[REFIDX(s)].car == SYM_MARK) ? 0 : heap[REFIDX(d)].cdr;
}

static val lisp_make_symbol(const char *s, uint8_t len)
{
    uint16_t hi, lo;
    val head, data;

    if (!lisp_pack_name(s, len, &hi, &lo)) {
        lisp_error("sym");
        return NIL;
    }

    if (lo == 0) {
        return lisp_cons(SYM_MARK, (val)hi);
    }

    /* Header first, then the raw {hi, lo} cell. Allocating the other way
     * round would leave a bare data cell live across an allocation, where a
     * collection would see two raw halves in the fields of what looks like
     * an ordinary cons. */
    head = lisp_cons(SYM2_MARK, NIL);
    if (head == NIL) {
        return NIL;
    }

    data = lisp_cons((val)hi, (val)lo);
    if (data == NIL) {
        return NIL;
    }

    heap[REFIDX(head)].cdr = data;
    return head;
}

/* Builtin names live in flash and are matched at read time, so they never
 * occupy a heap cell and are not limited to three characters. Order defines
 * the immediate payload: index i becomes MKSF(i) / MKFN(i). */
static const char *const lisp_special_names[] = {
    "quote", "if", "lambda", "define", "progn", "and", "or",
    "cond", "let",
};

static const char *const lisp_function_names[] = {
    "cons", "car", "cdr", "list",
    "eq", "atom", "consp", "numberp", "null", "not",
    "+", "-", "*", "/", "mod", "<", ">", "=",
    "print", "princ", "terpri", "room",
    "pin", "out", "in", "adc", "ms",
};

#define NSPECIALS (sizeof lisp_special_names / sizeof lisp_special_names[0])
#define NFUNCTIONS (sizeof lisp_function_names / sizeof lisp_function_names[0])

static void lisp_error(const char *msg)
{
    if (lisp_err == 0) {
        lisp_err = msg;
    }
}

/* Reader (section 5). One character of lookahead over a NUL-terminated line;
 * screen mode will later swap this source for a framebuffer scan.
 *
 * Note the list builder writes each new cell into the previous cell's cdr,
 * which is an old-to-new store and would break the copying collector's
 * invariant (section 4). It is safe only because no collection can happen
 * while reading: allocation never triggers a GC, safepoints are in eval and
 * at top level. Do not add one here without also rooting the partial form.
 */
static const char *lisp_src;

static char lisp_peek(void)
{
    if (lisp_from_screen) {
        if (lisp_scr_y >= TEXT_ROWS) {
            return '\0';
        }
        return (char)video_textmode_read_cell(lisp_scr_x, lisp_scr_y);
    }
    return *lisp_src;
}

static char lisp_next(void)
{
    char c = lisp_peek();

    if (c == '\0') {
        return '\0';
    }

    if (lisp_from_screen) {
        if (++lisp_scr_x >= TEXT_COLS) {
            lisp_scr_x = 0;
            lisp_scr_y++;
        }
    }
    else {
        lisp_src++;
    }

    return c;
}

static void lisp_skip_space(void)
{
    char c;

    while ((c = lisp_peek()) == ' ' || c == '\t') {
        lisp_next();
    }
}

static uint8_t lisp_is_delim(char c)
{
    return (uint8_t)(c == '\0' || c == ' ' || c == '\t' || c == '(' || c == ')');
}

static uint8_t lisp_name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a++ != *b++) {
            return 0;
        }
    }
    return (uint8_t)(*a == *b);
}

/* Resolve a token that is not a number, in this order: the two singletons,
 * the flash name tables, then a packed symbol of up to six characters. */
static val lisp_resolve_atom(const char *tok, uint8_t len)
{
    if (lisp_name_eq(tok, "nil")) {
        return NIL;
    }
    if (lisp_name_eq(tok, "t")) {
        return TEE;
    }

    for (uint8_t i = 0; i < NSPECIALS; i++) {
        if (lisp_name_eq(tok, lisp_special_names[i])) {
            return MKSF(i);
        }
    }
    for (uint8_t i = 0; i < NFUNCTIONS; i++) {
        if (lisp_name_eq(tok, lisp_function_names[i])) {
            return MKFN(i);
        }
    }

    return lisp_make_symbol(tok, len);
}

static val lisp_read_form(void);

/* Elements are appended at the tail, so a long list costs no C stack;
 * only nesting in the car direction recurses. */
static val lisp_read_list(void)
{
    val head = NIL;
    val tail = NIL;
    val v;

    for (;;) {
        lisp_skip_space();

        if (lisp_peek() == '\0') {
            lisp_error("eof");          /* unbalanced '(' */
            return NIL;
        }

        if (lisp_peek() == ')') {
            lisp_next();
            return head;
        }

        /* dotted tail: '.' only when it stands alone as a token */
        if (lisp_peek() == '.' && lisp_is_delim(lisp_src[1])) {
            lisp_next();
            if (head == NIL) {
                lisp_error("syn");      /* "( . x )" has no car */
                return NIL;
            }
            v = lisp_read_form();
            if (lisp_err) {
                return NIL;
            }
            heap[REFIDX(tail)].cdr = v;
            lisp_skip_space();
            if (lisp_next() != ')') {
                lisp_error("syn");
            }
            return head;
        }

        v = lisp_read_form();
        if (lisp_err) {
            return NIL;
        }

        v = lisp_cons(v, NIL);
        if (v == NIL) {
            lisp_error("mem");
            return NIL;
        }

        if (head == NIL) {
            head = v;
        }
        else {
            heap[REFIDX(tail)].cdr = v;
        }
        tail = v;
    }
}

static val lisp_read_form(void)
{
    char tok[10];
    uint8_t len = 0;
    char c;

    if (++eval_depth > MAXDEPTH) {
        lisp_error("deep");
        eval_depth--;
        return NIL;
    }

    lisp_skip_space();
    c = lisp_peek();

    if (c == '\0') {
        eval_depth--;
        return EOFV;
    }

    if (c == '(') {
        lisp_next();
        val v = lisp_read_list();
        eval_depth--;
        return v;
    }

    if (c == ')') {
        lisp_next();
        lisp_error("syn");
        eval_depth--;
        return NIL;
    }

    if (c == '\'') {                    /* 'x reads as (quote x) */
        lisp_next();
        val v = lisp_read_form();
        eval_depth--;
        if (lisp_err) {
            return NIL;
        }
        v = lisp_cons(v, NIL);
        v = lisp_cons(MKSF(0), v);      /* index 0 is "quote" */
        if (v == NIL) {
            lisp_error("mem");
        }
        return v;
    }

    /* a token: number if it starts with a digit, or '-' then a digit */
    while (!lisp_is_delim(lisp_peek())) {
        if (len < sizeof tok - 1) {
            tok[len++] = lisp_next();
        }
        else {
            lisp_next();
            lisp_error("sym");          /* token longer than any builtin */
        }
    }
    tok[len] = '\0';
    eval_depth--;

    if (lisp_err) {
        return NIL;
    }

    if ((tok[0] >= '0' && tok[0] <= '9')
     || (tok[0] == '-' && tok[1] >= '0' && tok[1] <= '9')) {
        int32_t n = 0;
        uint8_t i = (tok[0] == '-') ? 1 : 0;

        for (; i < len; i++) {
            if (tok[i] < '0' || tok[i] > '9') {
                lisp_error("num");
                return NIL;
            }
            n = n * 10 + (tok[i] - '0');
            if (n > 16384) {            /* bounded: cannot overflow int32 */
                lisp_error("num");
                return NIL;
            }
        }

        if (tok[0] == '-') {
            n = -n;
        }
        if (n > 16383 || n < -16384) {
            lisp_error("num");
            return NIL;
        }
        return MKFIX(n);
    }

    return lisp_resolve_atom(tok, len);
}

/* Evaluator (section 7)
 *
 * Environments are alists of (symbol . value). A call frame is consed onto
 * the front of the closure's captured environment, so every store points
 * from a newer cell to an older one and the copying collector's invariant
 * holds without any mutation. Argument lists are built by recursion for the
 * same reason: appending at the tail would be an old-to-new store.
 */

/* Special-form indices, matching lisp_special_names[] */
#define SF_QUOTE  0
#define SF_IF     1
#define SF_LAMBDA 2
#define SF_DEFINE 3
#define SF_PROGN  4
#define SF_AND    5
#define SF_OR     6
#define SF_COND   7
#define SF_LET    8

static val lisp_car(val v)
{
    return lisp_is_cons(v) ? heap[REFIDX(v)].car : NIL;
}

static val lisp_cdr(val v)
{
    return lisp_is_cons(v) ? heap[REFIDX(v)].cdr : NIL;
}

static uint8_t lisp_is_symbol(val v)
{
    val c;

    if (!ISREF(v)) {
        return 0;
    }
    c = heap[REFIDX(v)].car;
    return (uint8_t)(c == SYM_MARK || c == SYM2_MARK);
}

/* Return the (symbol . value) pair, or NIL when unbound. Symbols are not
 * interned, so identity is a comparison of both packed halves. */
static val lisp_assoc(val sym, val env)
{
    uint16_t hi = lisp_sym_hi(sym);
    uint16_t lo = lisp_sym_lo(sym);

    while (env != NIL) {
        val pair = lisp_car(env);
        val key = lisp_car(pair);

        if (lisp_is_symbol(key)
         && lisp_sym_hi(key) == hi && lisp_sym_lo(key) == lo) {
            return pair;
        }
        env = lisp_cdr(env);
    }

    return NIL;
}

/* Garbage collector (section 4): mark-sweep, with Schorr-Waite pointer
 * reversal for the mark phase and a conservative scan of the C stack for
 * roots.
 *
 * Why not the previous copying collector: copying needs somewhere to copy
 * to, so free cells had to be at least as many as live ones and barely half
 * the heap was usable. Nothing moves here, so every cell can hold live data.
 * Uniform 4-byte cells mean a non-moving collector cannot fragment: any free
 * cell serves any request.
 *
 * Marking cannot recurse -- the interpreter already spends most of a 1.36 KB
 * stack -- so it reverses pointers as it descends and restores them on the
 * way back, using one mark bit and one direction bit per cell and no stack
 * at all.
 *
 * Roots are the two globals, any values passed in explicitly, and whatever
 * the C stack happens to hold. Scanning the stack conservatively is sound
 * *because* nothing moves: a halfword that merely looks like a reference
 * retains one cell for one cycle, which is harmless, whereas a moving
 * collector would have to rewrite it and could not. It also means values
 * held by outer eval frames need no registration: a callee-saved register
 * live across a call is spilled by the callee's prologue, so it is on the
 * stack by the time a collection runs.
 */
static uint8_t gc_marks[(NCELLS + 7) / 8];
static uint8_t gc_dirs[(NCELLS + 7) / 8];

/* Top of the C stack, supplied by the platform (see lisp_set_stack_top). */
static char *gc_stack_top;

#define GC_MARKED(i)  (gc_marks[(i) >> 3] &   (uint8_t)(1u << ((i) & 7)))
#define GC_SETMARK(i) (gc_marks[(i) >> 3] |=  (uint8_t)(1u << ((i) & 7)))
#define GC_DIR(i)     (gc_dirs[(i) >> 3]  &   (uint8_t)(1u << ((i) & 7)))
#define GC_SETDIR(i)  (gc_dirs[(i) >> 3]  |=  (uint8_t)(1u << ((i) & 7)))
#define GC_CLRDIR(i)  (gc_dirs[(i) >> 3]  &= (uint8_t)~(1u << ((i) & 7)))

void lisp_set_stack_top(void *top)
{
    gc_stack_top = (char *)top;
}

/* Whether a field holds a reference the collector should descend into.
 *
 * These stay correct while marking is in progress. Only a cons ever has its
 * car reversed, and the parent link written there is a reference or NIL --
 * never a marker -- so the cell still reads as a cons on the way back up.
 * A symbol's cdr is a raw packed name and a SYM2 data cell holds two raw
 * halves; neither is ever traced. */
static uint8_t gc_traces_car(uint16_t i)
{
    val c = heap[i].car;

    if (ISIMM(c) && IMMSUB(c) == SUB_MK) {
        return 0;                       /* symbol or closure header */
    }
    return (uint8_t)ISREF(c);
}

static uint8_t gc_traces_cdr(uint16_t i)
{
    val c = heap[i].car;

    if (ISIMM(c) && IMMSUB(c) == SUB_MK) {
        return (uint8_t)(c == CLO_MARK && ISREF(heap[i].cdr));
    }
    return (uint8_t)ISREF(heap[i].cdr);
}

/* Schorr-Waite: descend by reversing the field just followed, and use the
 * direction bit to remember which field of a node the reversed link sits in.
 * Index 0 is the "no parent" sentinel, which works because cell 0 is never
 * allocated and MKREF(0) is NIL. */
static void gc_mark(val root)
{
    uint16_t p, q, t;

    if (!ISREF(root) || REFIDX(root) >= NCELLS) {
        return;
    }

    p = REFIDX(root);
    q = 0;

    for (;;) {
        if (p != 0 && !GC_MARKED(p)) {
            GC_SETMARK(p);
            GC_CLRDIR(p);

            /* A long symbol's data cell is raw: mark it so the sweep keeps
             * it, but never descend into it. */
            if (heap[p].car == SYM2_MARK && ISREF(heap[p].cdr)
             && REFIDX(heap[p].cdr) < NCELLS) {
                GC_SETMARK(REFIDX(heap[p].cdr));
            }

            if (gc_traces_car(p)) {
                t = REFIDX(heap[p].car);
                heap[p].car = MKREF(q);
                q = p;
                p = t;
                continue;
            }
            if (gc_traces_cdr(p)) {
                GC_SETDIR(p);
                t = REFIDX(heap[p].cdr);
                heap[p].cdr = MKREF(q);
                q = p;
                p = t;
                continue;
            }
            /* leaf: fall through and retreat */
        }

        for (;;) {
            if (q == 0) {
                return;
            }

            if (!GC_DIR(q)) {
                if (gc_traces_cdr(q)) {
                    /* finished the car, swing over to the cdr: the parent
                     * link moves across and the car is restored */
                    t = REFIDX(heap[q].cdr);
                    heap[q].cdr = heap[q].car;
                    heap[q].car = MKREF(p);
                    GC_SETDIR(q);
                    p = t;
                    break;
                }
                t = REFIDX(heap[q].car);        /* pop, link was in car */
                heap[q].car = MKREF(p);
            }
            else {
                t = REFIDX(heap[q].cdr);        /* pop, link was in cdr */
                heap[q].cdr = MKREF(p);
            }

            p = q;
            q = t;
        }
    }
}

/* Treat every halfword between here and the top of stack as a possible
 * reference. False positives cost one retained cell for one cycle. */
static void gc_scan_stack(void)
{
    volatile uint16_t here = 0;     /* only its address matters */
    uint16_t *p = (uint16_t *)(void *)&here;
    uint16_t *end = (uint16_t *)(void *)gc_stack_top;

    if (end == 0) {
        return;
    }

    while (p < end) {
        val v = *p;

        if (ISREF(v) && REFIDX(v) < NCELLS) {
            gc_mark(v);
        }
        p++;
    }
}

static void gc_sweep(void)
{
    uint16_t i;

    free_list = 0;
    free_count = 0;

    for (i = NCELLS - 1; i >= 1; i--) {
        if (!GC_MARKED(i)) {
            /* FREE_MARK is a type marker, so a conservative hit on a free
             * cell marks that one cell and stops: without it the collector
             * would follow the free chain and retain the whole of it. */
            heap[i].car = FREE_MARK;
            heap[i].cdr = MKREF(free_list);
            free_list = i;
            free_count++;
        }
    }
}

static void lisp_gc(val extra1, val extra2)
{
    uint16_t i;

    for (i = 0; i < sizeof gc_marks; i++) {
        gc_marks[i] = 0;
    }

    gc_mark(global_env);
    gc_mark(lisp_pending);
    gc_mark(extra1);
    gc_mark(extra2);
    gc_scan_stack();

    gc_sweep();
}



/* Builtins (section 6). Indices must match lisp_function_names[].
 *
 * Dispatch is a switch rather than the table of function pointers the design
 * sketched: 22 pointers would cost 88 B of flash plus a call per builtin,
 * and the names already live in a separate table for the reader.
 */
#define BI_CONS 0
#define BI_CAR  1
#define BI_CDR  2
#define BI_LIST 3
#define BI_EQ   4
#define BI_ATOM 5
#define BI_CONSP 6
#define BI_NUMP 7
#define BI_NULL 8
#define BI_NOT  9
#define BI_ADD  10
#define BI_SUB  11
#define BI_MUL  12
#define BI_DIV  13
#define BI_MOD  14
#define BI_LT   15
#define BI_GT   16
#define BI_NUMEQ 17
#define BI_PRINT 18
#define BI_PRINC 19
#define BI_TERPRI 20
#define BI_ROOM 21
#define BI_PIN  22
#define BI_OUT  23
#define BI_IN   24
#define BI_ADC  25
#define BI_MS   26

static int32_t lisp_fixarg(val v)
{
    if (!ISFIX(v)) {
        lisp_error("type");
        return 0;
    }
    return FIXVAL(v);
}

/* Fixnums are 15-bit; anything outside that is an error rather than a
 * silent wrap, so a runaway computation is reported instead of corrupting
 * results. */
static val lisp_fixval(int32_t n)
{
    if (n > 16383 || n < -16384) {
        lisp_error("num");
        return NIL;
    }
    return MKFIX((int16_t)n);
}

/* Identity: symbols compare by packed name because they are not interned;
 * everything else compares by value, which for a cons means the same cell. */
static val lisp_eq(val a, val b)
{
    if (lisp_is_symbol(a) && lisp_is_symbol(b)) {
        return (lisp_sym_hi(a) == lisp_sym_hi(b)
             && lisp_sym_lo(a) == lisp_sym_lo(b)) ? TEE : NIL;
    }
    return (a == b) ? TEE : NIL;
}

static val lisp_builtin(uint8_t idx, val args)
{
    val a = lisp_car(args);
    val b = lisp_car(lisp_cdr(args));
    int32_t x, y;

    switch (idx) {
    case BI_CONS: {
        val v = lisp_cons(a, b);
        if (v == NIL) {
            lisp_error("mem");
        }
        return v;
    }

    case BI_CAR:  return lisp_car(a);
    case BI_CDR:  return lisp_cdr(a);
    case BI_LIST: return args;          /* already an evaluated list */

    case BI_EQ:    return lisp_eq(a, b);
    case BI_ATOM:  return lisp_is_cons(a) ? NIL : TEE;
    case BI_CONSP: return lisp_is_cons(a) ? TEE : NIL;
    case BI_NUMP:  return ISFIX(a) ? TEE : NIL;
    case BI_NULL:
    case BI_NOT:   return (a == NIL) ? TEE : NIL;

    case BI_ADD: {
        int32_t acc = 0;
        while (lisp_is_cons(args)) {
            acc += lisp_fixarg(lisp_car(args));
            if (lisp_err) {
                return NIL;
            }
            args = lisp_cdr(args);
        }
        return lisp_fixval(acc);
    }

    case BI_SUB: {
        int32_t acc;
        if (!lisp_is_cons(args)) {
            lisp_error("args");
            return NIL;
        }
        acc = lisp_fixarg(a);
        if (lisp_err) {
            return NIL;
        }
        args = lisp_cdr(args);
        if (!lisp_is_cons(args)) {
            return lisp_fixval(-acc);           /* unary negation */
        }
        while (lisp_is_cons(args)) {
            acc -= lisp_fixarg(lisp_car(args));
            if (lisp_err) {
                return NIL;
            }
            args = lisp_cdr(args);
        }
        return lisp_fixval(acc);
    }

    case BI_MUL: {
        int32_t acc = 1;
        while (lisp_is_cons(args)) {
            acc *= lisp_fixarg(lisp_car(args));
            if (lisp_err) {
                return NIL;
            }
            if (acc > 16383 || acc < -16384) {  /* checked each step: operands
                                                 * are bounded, so the int32
                                                 * product cannot overflow */
                lisp_error("num");
                return NIL;
            }
            args = lisp_cdr(args);
        }
        return MKFIX((int16_t)acc);
    }

    case BI_DIV:
    case BI_MOD:
        x = lisp_fixarg(a);
        y = lisp_fixarg(b);
        if (lisp_err) {
            return NIL;
        }
        if (y == 0) {
            lisp_error("div");
            return NIL;
        }
        return lisp_fixval(idx == BI_DIV ? x / y : x % y);

    case BI_LT:
    case BI_GT:
    case BI_NUMEQ:
        x = lisp_fixarg(a);
        y = lisp_fixarg(b);
        if (lisp_err) {
            return NIL;
        }
        if (idx == BI_LT) {
            return (x < y) ? TEE : NIL;
        }
        if (idx == BI_GT) {
            return (x > y) ? TEE : NIL;
        }
        return (x == y) ? TEE : NIL;

    case BI_PRINT:
        lisp_print(a);
        console_print_char('\n');
        return a;

    case BI_PRINC:
        lisp_print(a);
        return a;

    case BI_TERPRI:
        console_print_char('\n');
        return NIL;

    /* Hardware (lisp_hw.h). A refused pin is an error rather than a silent
     * no-op: pins 8..15 are port C, which the video interrupt owns, and 17
     * is the debug console. */
    case BI_PIN:
        x = lisp_fixarg(a);
        if (lisp_err) {
            return NIL;
        }
        /* second argument is a mode number; nil reads as input */
        y = ISFIX(b) ? FIXVAL(b) : (b == NIL ? HW_IN : HW_OUT);
        if (hw_mode((int16_t)x, (int16_t)y) < 0) {
            lisp_error("pin");
            return NIL;
        }
        return TEE;

    case BI_OUT:
        x = lisp_fixarg(a);
        if (lisp_err) {
            return NIL;
        }
        if (hw_write((int16_t)x, (int16_t)(b != NIL && b != MKFIX(0))) < 0) {
            lisp_error("pin");
            return NIL;
        }
        return b;

    case BI_IN:
        x = lisp_fixarg(a);
        if (lisp_err) {
            return NIL;
        }
        y = hw_read((int16_t)x);
        if (y < 0) {
            lisp_error("pin");
            return NIL;
        }
        /* t/nil rather than 1/0: only nil is false here, so a fixnum result
         * would make (if (in p) ...) always take the true branch. */
        return y ? TEE : NIL;

    case BI_ADC:
        x = lisp_fixarg(a);
        if (lisp_err) {
            return NIL;
        }
        y = hw_adc((int16_t)x);
        if (y < 0) {
            lisp_error("pin");
            return NIL;
        }
        return MKFIX((int16_t)y);

    case BI_MS:
        x = lisp_fixarg(a);
        if (lisp_err) {
            return NIL;
        }
        hw_delay_ms((int16_t)x);
        return NIL;

    case BI_ROOM:
    default:
        /* Collect first: without a collection the free count only says how
         * much was left when the last allocation happened to run short,
         * which tells the user nothing about how much they can still use. */
        lisp_gc(NIL, NIL);
        return MKFIX((int16_t)free_count);
    }
}

static val lisp_eval(val x, val env);

/* Evaluate an argument list. Recursive so that each cell is created after
 * the cells it points at; the C stack cost is one frame per argument and is
 * covered by the MAXDEPTH guard. */
static val lisp_eval_args(val args, val env)
{
    val head, rest;

    if (!lisp_is_cons(args)) {
        return NIL;
    }

    head = lisp_eval(lisp_car(args), env);
    if (lisp_err) {
        return NIL;
    }

    rest = lisp_eval_args(lisp_cdr(args), env);
    if (lisp_err) {
        return NIL;
    }

    return lisp_cons(head, rest);
}

/* Bind parameters to arguments, consing the frame onto the front of env. */
static val lisp_bind(val params, val args, val env)
{
    while (lisp_is_cons(params)) {
        val pair = lisp_cons(lisp_car(params), lisp_car(args));

        if (pair == NIL) {
            lisp_error("mem");
            return env;
        }

        env = lisp_cons(pair, env);
        if (env == NIL) {
            lisp_error("mem");
            return NIL;
        }

        params = lisp_cdr(params);
        args = lisp_cdr(args);
    }

    return env;
}

static val lisp_eval(val x, val env)
{
    val f, args, v;

    if (++eval_depth > MAXDEPTH) {
        lisp_error("deep");
        eval_depth--;
        return NIL;
    }

    for (;;) {
        if (lisp_err) {
            x = NIL;
            goto done;
        }

        /* Pump the input path occasionally so Esc can stop a runaway
         * program. Every 64th trip keeps this responsive -- a loop turns
         * over far faster than anyone can type -- while leaving the cost
         * well under a percent. */
        if ((++lisp_poll_tick & 63u) == 0u) {
            hw_poll_input();

            if (lisp_break) {
                lisp_break = 0;
                lisp_error("brk");
                x = NIL;
                goto done;
            }
        }

        /* Fixnums, nil, t, builtins and special forms evaluate to themselves;
         * so does a closure cell, which is a marker-headed heap object. */
        if (!ISREF(x)) {
            goto done;
        }

        if (lisp_is_symbol(x)) {
            v = lisp_assoc(x, env);
            if (v == NIL) {
                v = lisp_assoc(x, global_env);
            }
            if (v == NIL) {
                lisp_error("unb");
                x = NIL;
                goto done;
            }
            x = heap[REFIDX(v)].cdr;
            goto done;
        }

        if (!lisp_is_cons(x)) {
            goto done;              /* closure prints/returns as itself */
        }

        f = lisp_car(x);
        args = lisp_cdr(x);

        if (ISIMM(f) && IMMSUB(f) == SUB_SF) {
            switch (IMMPL(f)) {
            case SF_QUOTE:
                x = lisp_car(args);
                goto done;

            case SF_IF:
                v = lisp_eval(lisp_car(args), env);
                if (lisp_err) {
                    x = NIL;
                    goto done;
                }
                x = (v != NIL) ? lisp_car(lisp_cdr(args))
                               : lisp_car(lisp_cdr(lisp_cdr(args)));
                continue;                                   /* tail call */

            case SF_LAMBDA:
                /* closure cell -> (params . (body . env)) */
                v = lisp_cons(lisp_car(lisp_cdr(args)), env);
                v = lisp_cons(lisp_car(args), v);
                x = lisp_cons(CLO_MARK, v);
                if (x == NIL) {
                    lisp_error("mem");
                }
                goto done;

            case SF_DEFINE:
                v = lisp_eval(lisp_car(lisp_cdr(args)), env);
                if (lisp_err) {
                    x = NIL;
                    goto done;
                }
                x = lisp_car(args);
                if (!lisp_is_symbol(x)) {
                    lisp_error("syn");
                    x = NIL;
                    goto done;
                }
                v = lisp_cons(x, v);
                v = lisp_cons(v, global_env);
                if (v == NIL) {
                    lisp_error("mem");
                    x = NIL;
                    goto done;
                }
                /* global_env is a variable, not a heap field: no old-to-new
                 * store. It must be a root at every GC safepoint. */
                global_env = v;
                goto done;

            case SF_PROGN:
                if (!lisp_is_cons(args)) {
                    x = NIL;
                    goto done;
                }
                while (lisp_is_cons(lisp_cdr(args))) {
                    lisp_eval(lisp_car(args), env);
                    if (lisp_err) {
                        x = NIL;
                        goto done;
                    }
                    args = lisp_cdr(args);
                }
                x = lisp_car(args);
                continue;                                   /* tail call */

            case SF_AND:
                if (!lisp_is_cons(args)) {
                    x = TEE;
                    goto done;
                }
                while (lisp_is_cons(lisp_cdr(args))) {
                    v = lisp_eval(lisp_car(args), env);
                    if (lisp_err) {
                        x = NIL;
                        goto done;
                    }
                    if (v == NIL) {
                        x = NIL;
                        goto done;
                    }
                    args = lisp_cdr(args);
                }
                x = lisp_car(args);
                continue;                                   /* tail call */

            case SF_OR:
                if (!lisp_is_cons(args)) {
                    x = NIL;
                    goto done;
                }
                while (lisp_is_cons(lisp_cdr(args))) {
                    v = lisp_eval(lisp_car(args), env);
                    if (lisp_err) {
                        x = NIL;
                        goto done;
                    }
                    if (v != NIL) {
                        x = v;
                        goto done;
                    }
                    args = lisp_cdr(args);
                }
                x = lisp_car(args);
                continue;                                   /* tail call */

            /* (cond (test body...) (test body...) ...)
             *
             * The chosen clause's last expression is left in x so the loop
             * takes it as a tail call: a cond arm costs no C stack, which
             * matters because cond is how loops get written once nested if
             * runs out of columns. */
            case SF_COND: {
                val clause = NIL;
                val body;
                uint8_t taken = 0;

                while (lisp_is_cons(args)) {
                    clause = lisp_car(args);
                    v = lisp_eval(lisp_car(clause), env);
                    if (lisp_err) {
                        x = NIL;
                        goto done;
                    }
                    if (v != NIL) {
                        taken = 1;
                        break;
                    }
                    args = lisp_cdr(args);
                }

                if (!taken) {
                    x = NIL;                    /* no clause matched */
                    goto done;
                }

                body = lisp_cdr(clause);
                if (!lisp_is_cons(body)) {
                    x = v;                      /* (cond (test)) yields test */
                    goto done;
                }
                while (lisp_is_cons(lisp_cdr(body))) {
                    lisp_eval(lisp_car(body), env);
                    if (lisp_err) {
                        x = NIL;
                        goto done;
                    }
                    body = lisp_cdr(body);
                }
                x = lisp_car(body);
                continue;                                   /* tail call */
            }

            /* (let ((name init) ...) body...)
             *
             * Initialisers are evaluated in the outer environment, so the
             * bindings are parallel rather than sequential -- this is `let`,
             * not `let*`, and one binding cannot see another.
             *
             * The part-built environment lives only in a C local, which is
             * safe because the collector scans the stack; under the previous
             * copying collector this would have needed rooting. */
            case SF_LET: {
                val binds = lisp_car(args);
                val body = lisp_cdr(args);
                val newenv = env;
                val name;

                while (lisp_is_cons(binds)) {
                    val bind = lisp_car(binds);

                    name = lisp_car(bind);
                    if (!lisp_is_symbol(name)) {
                        lisp_error("syn");
                        x = NIL;
                        goto done;
                    }

                    v = lisp_eval(lisp_car(lisp_cdr(bind)), env);
                    if (lisp_err) {
                        x = NIL;
                        goto done;
                    }

                    v = lisp_cons(name, v);
                    newenv = lisp_cons(v, newenv);
                    if (lisp_err) {
                        x = NIL;
                        goto done;
                    }

                    binds = lisp_cdr(binds);
                }

                env = newenv;

                if (!lisp_is_cons(body)) {
                    x = NIL;
                    goto done;
                }
                while (lisp_is_cons(lisp_cdr(body))) {
                    lisp_eval(lisp_car(body), env);
                    if (lisp_err) {
                        x = NIL;
                        goto done;
                    }
                    body = lisp_cdr(body);
                }
                x = lisp_car(body);
                continue;                                   /* tail call */
            }

            default:
                lisp_error("syn");
                x = NIL;
                goto done;
            }
        }

        /* Application */
        f = lisp_eval(f, env);
        if (lisp_err) {
            x = NIL;
            goto done;
        }

        args = lisp_eval_args(args, env);
        if (lisp_err) {
            x = NIL;
            goto done;
        }

        if (ISIMM(f) && IMMSUB(f) == SUB_FN) {
            x = lisp_builtin((uint8_t)IMMPL(f), args);
            goto done;
        }

        if (ISREF(f) && heap[REFIDX(f)].car == CLO_MARK) {
            val spec = heap[REFIDX(f)].cdr;         /* (params . (body . env)) */
            val params = lisp_car(spec);
            val body = lisp_car(lisp_cdr(spec));
            val cenv = lisp_cdr(lisp_cdr(spec));

            env = lisp_bind(params, args, cenv);
            if (lisp_err) {
                x = NIL;
                goto done;
            }
            x = body;
            continue;                                       /* tail call */
        }

        lisp_error("call");             /* not applicable */
        x = NIL;
        goto done;
    }

done:
    eval_depth--;
    return x;
}

/* Printer (section 5). Output goes through the console, so it lands on the
 * framebuffer and scrolls like any other output. */
static void lisp_print_int(int32_t k)
{
    char digits[5];                 /* 16383 is five digits; sign printed first */
    uint8_t n = 0;

    if (k < 0) {
        console_print_char('-');
        k = -k;                     /* widened to 32 bits: -(-16384) overflows 16 */
    }

    do {
        digits[n++] = (char)('0' + (k % 10));
        k /= 10;
    } while (k != 0);

    while (n > 0) {
        console_print_char(digits[--n]);
    }
}

void lisp_print(val v)
{
    if (++eval_depth > MAXDEPTH) {
        console_print_string("...");
        eval_depth--;
        return;
    }

    if (ISFIX(v)) {
        lisp_print_int(FIXVAL(v));
    }
    else if (v == NIL) {
        console_print_string("nil");
    }
    else if (v == TEE) {
        console_print_string("t");
    }
    else if (ISIMM(v)) {
        if (IMMSUB(v) == SUB_FN && IMMPL(v) < NFUNCTIONS) {
            console_print_string(lisp_function_names[IMMPL(v)]);
        }
        else if (IMMSUB(v) == SUB_SF && IMMPL(v) < NSPECIALS) {
            console_print_string(lisp_special_names[IMMPL(v)]);
        }
        else {
            console_print_string("?");
        }
    }
    else if (lisp_is_symbol(v)) {
        char name[4];
        lisp_unpack3(lisp_sym_hi(v), name);
        console_print_string(name);
        if (lisp_sym_lo(v) != 0) {
            lisp_unpack3(lisp_sym_lo(v), name);
            console_print_string(name);
        }
    }
    else if (heap[REFIDX(v)].car == CLO_MARK) {
        console_print_string("#clo");
    }
    else {
        /* Cons: recurse on car, iterate on cdr, so only car nesting costs
         * C stack. A long list prints in constant stack. */
        cell *c = &heap[REFIDX(v)];

        console_print_char('(');
        for (;;) {
            lisp_print(c->car);
            if (c->cdr == NIL) {
                break;
            }
            if (!lisp_is_cons(c->cdr)) {
                console_print_string(" . ");
                lisp_print(c->cdr);
                break;
            }
            console_print_char(' ');
            c = &heap[REFIDX(c->cdr)];
        }
        console_print_char(')');
    }

    eval_depth--;
}

/* REPL entry point, called once per submitted line: read, evaluate and print
 * each form on the line.
 *
 * There is no explicit collection here any more. Allocation collects when it
 * runs out, and a form's garbage simply stops being reachable once the form
 * has been printed. A failed form needs only its definitions rolled back:
 * the cells it allocated become unreachable by the same argument.
 */
void lisp_handle_input_line(const char *line)
{
    val v;
    val saved_env;

    lisp_src = line;
    lisp_err = 0;
    eval_depth = 0;
    lisp_break = 0;

    for (;;) {
        lisp_skip_space();
        if (lisp_peek() == '\0') {
            break;
        }

        saved_env = global_env;

        v = lisp_read_form();
        if (!lisp_err && v != EOFV) {
            lisp_running = 1;
            v = lisp_eval(v, NIL);
            lisp_running = 0;
        }

        if (lisp_err) {
            console_print_string("? ");
            console_print_string(lisp_err);
            console_print_char('\n');
            global_env = saved_env;
            return;
        }

        if (v == EOFV) {
            break;
        }

        lisp_print(v);
        console_print_char('\n');
    }
}

static void lisp_report_error(void)
{
    console_print_string("? ");
    console_print_string(lisp_err);
    console_print_char('\n');
}

/* Screen mode (section 5): read the whole framebuffer as source, then run it.
 *
 * The two passes are not an accident. Evaluating a form can print, printing
 * scrolls the screen, and the screen is the source -- so reading and
 * evaluating cannot be interleaved without the program rewriting itself
 * underneath the reader. Pass one therefore consumes the entire screen into
 * a list of forms, and only then does pass two evaluate them.
 *
 * The form list is held in lisp_pending so that a collection during pass two
 * keeps the part of the program still to run. Everything else the collector
 * needs it finds on the C stack. The whole program must still fit in the
 * heap as cells; one too large reports "mem".
 */
void lisp_handle_screen(void)
{
    val head = NIL;
    val tail = NIL;
    val saved_env;
    val v;

    lisp_err = 0;
    eval_depth = 0;
    lisp_break = 0;
    lisp_pending = NIL;
    saved_env = global_env;

    /* pass 1: read every form on the screen */
    lisp_scr_x = 0;
    lisp_scr_y = 0;
    lisp_from_screen = 1;

    for (;;) {
        lisp_skip_space();
        if (lisp_peek() == '\0') {
            break;
        }

        v = lisp_read_form();
        if (lisp_err || v == EOFV) {
            break;
        }

        v = lisp_cons(v, NIL);
        if (lisp_err) {
            break;
        }

        if (head == NIL) {
            head = v;
        }
        else {
            heap[REFIDX(tail)].cdr = v;
        }
        tail = v;
    }

    lisp_from_screen = 0;
    console_prepare_input_row();

    if (lisp_err) {
        lisp_report_error();
        lisp_pending = NIL;
        return;
    }

    /* pass 2: evaluate each form, printing as we go */
    lisp_pending = head;

    while (lisp_is_cons(lisp_pending)) {
        saved_env = global_env;

        lisp_running = 1;
        v = lisp_eval(lisp_car(lisp_pending), NIL);
        lisp_running = 0;

        if (lisp_err) {
            lisp_report_error();
            global_env = saved_env;
            lisp_pending = NIL;
            return;
        }

        lisp_print(v);
        console_print_char('\n');

        lisp_pending = lisp_cdr(lisp_pending);
    }

    /* Dropping the last reference is all that is needed: the program text
     * becomes unreachable and the next allocation that runs short reclaims
     * it. */
    lisp_pending = NIL;
}
