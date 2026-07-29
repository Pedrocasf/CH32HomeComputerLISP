#include "lisp.h"

#include "console_textmode.h"
#include "video_textmode.h"

static void lisp_error(const char *msg);

/* First error raised while handling the current line, or NULL. A flag rather
 * than setjmp: nothing needs unwinding, and setjmp stays out of the image. */
static const char *lisp_err;

/* Heap and globals (LISP_DESIGN.md section 2) */
static cell heap[NCELLS];
static uint16_t heap_top = 1;       /* allocation frontier; cell 0 is NIL */
static uint16_t heap_mark = 1;      /* watermark after last good top form */
static val global_env = NIL;
static uint8_t eval_depth;

/* Forms read from the screen but not yet evaluated (section 5). A root in
 * its own right: the top-level safepoint runs between forms, and its
 * watermark predates the whole screen read, so without this the remaining
 * program would be collected out from under the loop. */
static val lisp_pending = NIL;

/* Screen mode (section 5): the framebuffer is the source buffer, so the
 * reader can take characters straight from video RAM instead of a line.
 * Rows are space-padded, so a row boundary reads as whitespace and a form
 * may span rows freely; a token split across the 32-column edge does not
 * survive, which matches how screen editors have always behaved. */
static uint8_t lisp_from_screen;
static uint8_t lisp_scr_x;
static uint8_t lisp_scr_y;

void lisp_init(void)
{
    heap_top = 1;
    heap_mark = 1;
    global_env = NIL;
    eval_depth = 0;
    lisp_err = 0;
    lisp_pending = NIL;
    lisp_from_screen = 0;
}

/* Allocation is a frontier bump: the copying collector (section 4) has no
 * free list, and every cell at or above heap_top is free. */
static val lisp_cons(val a, val d)
{
    if (heap_top >= NCELLS) {
        /* Report exhaustion here rather than returning a bare NIL: callers
         * that missed the check would otherwise carry a broken environment
         * forward and fail later with a misleading "type" error. */
        lisp_error("mem");
        return NIL;
    }

    heap[heap_top].car = a;
    heap[heap_top].cdr = d;
    return MKREF(heap_top++);
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
    val data;

    if (!lisp_pack_name(s, len, &hi, &lo)) {
        lisp_error("sym");
        return NIL;
    }

    if (lo == 0) {
        return lisp_cons(SYM_MARK, (val)hi);
    }

    data = lisp_cons((val)hi, (val)lo);
    if (data == NIL) {
        lisp_error("mem");
        return NIL;
    }

    return lisp_cons(SYM2_MARK, data);
}

/* Builtin names live in flash and are matched at read time, so they never
 * occupy a heap cell and are not limited to three characters. Order defines
 * the immediate payload: index i becomes MKSF(i) / MKFN(i). */
static const char *const lisp_special_names[] = {
    "quote", "if", "lambda", "define", "progn", "and", "or",
};

static const char *const lisp_function_names[] = {
    "cons", "car", "cdr", "list",
    "eq", "atom", "consp", "numberp", "null", "not",
    "+", "-", "*", "/", "mod", "<", ">", "=",
    "print", "princ", "terpri", "room",
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

/* Garbage collector (section 4): watermark copying, iterative Cheney scan,
 * then a slide back down to the watermark.
 *
 * The language creates no old-to-new heap stores (environments and argument
 * lists are built front-first or by recursion, and `define` updates a C
 * variable), so nothing below the watermark can reference anything at or
 * above it. That is what lets the collector ignore everything older than the
 * watermark instead of tracing the whole heap.
 *
 * The one exception is the reader, which appends at the tail of the list it
 * is building. No collection can occur while reading, so that is safe -- see
 * the note above lisp_read_list.
 *
 * Interrupts are never masked: the video ISR keeps running throughout,
 * including during the slide.
 */
static uint16_t gc_from;            /* watermark: cells below never move */
static uint16_t gc_to;              /* start of to-space */

static val lisp_evacuate(val v)
{
    uint16_t i, n;
    cell c;

    if (!ISREF(v)) {
        return v;
    }

    i = REFIDX(v);
    if (i < gc_from) {
        return v;                   /* older than the watermark: pinned */
    }

    c = heap[i];
    if (c.car == FWD_MARK) {
        return c.cdr;               /* already copied; keep sharing intact */
    }

    n = heap_top;

    if (c.car == SYM2_MARK) {
        /* A long symbol is copied as a unit: header followed immediately by
         * its {hi, lo} data cell, which holds raw packed groups and must be
         * copied as a leaf rather than scanned as a pair of references. The
         * scan below relies on that adjacency to step over it. Copying the
         * data cell unconditionally can duplicate one below the watermark,
         * which costs a cell but keeps the layout invariant. */
        if (n + 2 > NCELLS) {
            lisp_error("mem");
            return v;
        }
        heap[n].car = SYM2_MARK;
        heap[n].cdr = MKREF(n + 1);
        heap[n + 1] = heap[REFIDX(c.cdr)];
        heap_top = n + 2;
    }
    else {
        if (n + 1 > NCELLS) {
            lisp_error("mem");
            return v;
        }
        heap[n] = c;
        heap_top = n + 1;
    }

    heap[i].car = FWD_MARK;
    heap[i].cdr = MKREF(n);
    return MKREF(n);
}

static val lisp_slide(val v, uint16_t delta)
{
    if (ISREF(v) && REFIDX(v) >= gc_to) {
        return (val)(v - (val)(delta << 2));
    }
    return v;
}

static void lisp_gc(uint16_t watermark, val **roots, uint8_t nroots)
{
    uint16_t scan, size, delta;
    uint8_t k;
    val a;

    gc_from = watermark;
    gc_to = heap_top;

    for (k = 0; k < nroots; k++) {
        *roots[k] = lisp_evacuate(*roots[k]);
    }

    /* Breadth-first scan of to-space. Evacuating appends to heap_top, so the
     * loop reaches newly copied cells without any recursion or stack. */
    scan = gc_to;
    while (scan < heap_top && !lisp_err) {
        a = heap[scan].car;

        if (a == SYM_MARK) {
            scan += 1;                              /* cdr is a raw name */
        }
        else if (a == SYM2_MARK) {
            scan += 2;                              /* skip the data cell */
        }
        else if (a == CLO_MARK) {
            heap[scan].cdr = lisp_evacuate(heap[scan].cdr);
            scan += 1;
        }
        else {
            heap[scan].car = lisp_evacuate(heap[scan].car);
            heap[scan].cdr = lisp_evacuate(heap[scan].cdr);
            scan += 1;
        }
    }

    if (lisp_err) {
        return;                     /* to-space overflowed; caller reports */
    }

    /* Slide the live block down onto the watermark. Destination is below
     * source, so a forward copy is safe. */
    size = heap_top - gc_to;
    delta = gc_to - gc_from;

    for (scan = 0; scan < size; scan++) {
        heap[gc_from + scan] = heap[gc_to + scan];
    }
    heap_top = gc_from + size;

    /* Patch references into the moved block by the constant slide delta;
     * references below the watermark are already correct. */
    scan = gc_from;
    while (scan < heap_top) {
        a = heap[scan].car;

        if (a == SYM_MARK) {
            scan += 1;
        }
        else if (a == SYM2_MARK) {
            heap[scan].cdr = lisp_slide(heap[scan].cdr, delta);
            scan += 2;
        }
        else if (a == CLO_MARK) {
            heap[scan].cdr = lisp_slide(heap[scan].cdr, delta);
            scan += 1;
        }
        else {
            heap[scan].car = lisp_slide(heap[scan].car, delta);
            heap[scan].cdr = lisp_slide(heap[scan].cdr, delta);
            scan += 1;
        }
    }

    for (k = 0; k < nroots; k++) {
        *roots[k] = lisp_slide(*roots[k], delta);
    }
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

    case BI_ROOM:
    default:
        return MKFIX((int16_t)(NCELLS - heap_top));
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
    uint16_t entry_top = heap_top;

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

        /* Safepoint (section 4). The watermark is this frame's entry
         * frontier, so everything an outer C frame holds was allocated
         * earlier and is pinned -- which is why no C code registers roots.
         * Only x, env and the global environment are live here: f, args and
         * v are dead at the loop head, and a tail call has already stored
         * its results into x and env.
         *
         * The trigger guarantees the copy fits. Live data cannot exceed the
         * region since the watermark, so collecting once that region has
         * grown to the size of the remaining free space means to-space is
         * always large enough. It also makes a tail-recursive loop collect
         * at a steady interval rather than at a fixed occupancy.
         */
        if (heap_top > entry_top
         && (uint16_t)(heap_top - entry_top) >= (uint16_t)(NCELLS - heap_top)) {
            /* Static, not automatic: this array would otherwise add 16 bytes
             * to every eval frame, and eval frames are what bound recursion
             * depth on a 1.4 KB stack. Safe because a collection never runs
             * inside another one. */
            static val *roots[4];

            roots[0] = &x;
            roots[1] = &env;
            roots[2] = &global_env;
            roots[3] = &lisp_pending;
            lisp_gc(entry_top, roots, 4);

            if (lisp_err) {
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
            default:
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
 * Each form ends at the top-level safepoint (section 4): the watermark is the
 * frontier from before the form was read, and the global environment is the
 * only root, so everything the form allocated is reclaimed except what a
 * definition retained. The printed result does not survive, because it has
 * already been printed.
 *
 * Error recovery is the design's `fp = W0`, plus restoring the global
 * environment head: a form that defines something and then fails must not
 * leave the global alist pointing at cells the rewind has freed.
 */
void lisp_handle_input_line(const char *line)
{
    val v;
    val saved_env;

    lisp_src = line;
    lisp_err = 0;
    eval_depth = 0;

    for (;;) {
        lisp_skip_space();
        if (lisp_peek() == '\0') {
            break;
        }

        saved_env = global_env;

        v = lisp_read_form();
        if (!lisp_err && v != EOFV) {
            v = lisp_eval(v, NIL);
        }

        if (lisp_err) {
            console_print_string("? ");
            console_print_string(lisp_err);
            console_print_char('\n');
            global_env = saved_env;
            heap_top = heap_mark;
            return;
        }

        if (v == EOFV) {
            break;
        }

        lisp_print(v);
        console_print_char('\n');

        {
            val *roots[1];

            roots[0] = &global_env;
            lisp_gc(heap_mark, roots, 1);

            if (lisp_err) {
                console_print_string("? ");
                console_print_string(lisp_err);
                console_print_char('\n');
                global_env = saved_env;
                heap_top = heap_mark;
                return;
            }
        }

        heap_mark = heap_top;
    }

    heap_mark = heap_top;
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
 * The whole program must fit in the heap as cells, because no collection can
 * run during reading (the list builder appends at the tail). A program too
 * large for the heap reports "mem".
 *
 * heap_mark is deliberately left alone until the end: keeping the watermark
 * below the form list lets each between-form collection reclaim that form's
 * garbage, and the final collection then reclaims the program itself.
 */
void lisp_handle_screen(void)
{
    val head = NIL;
    val tail = NIL;
    val saved_env;
    val v;

    lisp_err = 0;
    eval_depth = 0;
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
        heap_top = heap_mark;
        lisp_pending = NIL;
        return;
    }

    /* pass 2: evaluate each form, printing as we go */
    lisp_pending = head;

    while (lisp_is_cons(lisp_pending)) {
        saved_env = global_env;

        v = lisp_eval(lisp_car(lisp_pending), NIL);

        if (lisp_err) {
            lisp_report_error();
            global_env = saved_env;
            lisp_pending = NIL;
            heap_top = heap_mark;
            return;
        }

        lisp_print(v);
        console_print_char('\n');

        lisp_pending = lisp_cdr(lisp_pending);

        {
            val *roots[2];

            roots[0] = &global_env;
            roots[1] = &lisp_pending;
            lisp_gc(heap_mark, roots, 2);

            if (lisp_err) {
                lisp_report_error();
                global_env = saved_env;
                lisp_pending = NIL;
                heap_top = heap_mark;
                return;
            }
        }
    }

    lisp_pending = NIL;

    /* final collection reclaims the program text itself */
    {
        val *roots[1];

        roots[0] = &global_env;
        lisp_gc(heap_mark, roots, 1);
        if (lisp_err) {
            lisp_report_error();
        }
    }

    heap_mark = heap_top;
}
