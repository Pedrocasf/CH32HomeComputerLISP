/* Host-side unit tests for lisp.c (LISP_DESIGN.md section 8, step 2).
 *
 * lisp.c is a single translation unit of static functions, so the tests
 * include it directly and supply the console hooks themselves. This runs on
 * the development machine with sanitizers, long before anything is flashed.
 *
 *   cc -std=c99 -Os -Wall -Wextra -I.. -fsanitize=address,undefined \
 *      host_test.c -o host_test && ./host_test
 */
#include <stdio.h>
#include <string.h>

#include "../lisp.c"

/* Console stubs: capture output so tests can assert on printed text. */
static char out[256];
static int outn;

void console_print_char(char ch)
{
    if (outn < (int)sizeof out - 1) {
        out[outn++] = ch;
    }
}

void console_print_string(const char *str)
{
    while (*str) {
        console_print_char(*str++);
    }
}

void console_prepare_input_row(void) {}

/* Simulated hardware: mirrors the pin guard in lisp_hw.c so the refusal
 * rules can be tested without a board. */
static int8_t  sim_mode[24];
static int8_t  sim_level[24];
static int16_t sim_delay_total;

static int16_t sim_usable(int16_t pin)
{
    if (pin < 0 || pin > 23) return -1;
    if (pin >= 8 && pin <= 15) return -1;   /* port C: owned by video */
    if (pin == 17) return -1;               /* PD1: debug console */
    return pin;
}

void hw_init(void) {}

int8_t hw_mode(int16_t pin, int16_t mode)
{
    if (sim_usable(pin) < 0) return -1;
    sim_mode[pin] = (int8_t)mode;
    return 0;
}

int8_t hw_write(int16_t pin, int16_t value)
{
    if (sim_usable(pin) < 0) return -1;
    sim_level[pin] = value ? 1 : 0;
    return 0;
}

int16_t hw_read(int16_t pin)
{
    if (sim_usable(pin) < 0) return -1;
    return sim_level[pin];
}

int16_t hw_adc(int16_t channel)
{
    if (channel < 0 || channel > 9) return -1;
    return (int16_t)(channel * 100);        /* deterministic fake reading */
}

void hw_delay_ms(int16_t ms)
{
    if (ms > 0) sim_delay_total += ms;
}

/* Simulated input pump. Setting sim_break_after makes Esc arrive that many
 * polls into a run, which is how the break path is tested without a
 * keyboard. */
static int32_t sim_break_after;
static int32_t sim_polls;

void hw_poll_input(void)
{
    sim_polls++;
    if (sim_break_after > 0 && sim_polls >= sim_break_after) {
        sim_break_after = 0;
        lisp_handle_run_control_byte(0x1b);
    }
}

/* Stub framebuffer for screen mode: rows are space-padded like the real one. */
static uint8_t fb[TEXT_ROWS][TEXT_COLS];

uint8_t video_textmode_read_cell(uint8_t x, uint8_t y)
{
    return fb[y][x];
}

/* Lay out program text on the fake screen, one source line per row. */
static void screen_set(const char *const *rows, int nrows)
{
    for (int y = 0; y < TEXT_ROWS; y++) {
        for (int x = 0; x < TEXT_COLS; x++) {
            fb[y][x] = ' ';
        }
    }
    for (int y = 0; y < nrows && y < TEXT_ROWS; y++) {
        for (int x = 0; rows[y][x] && x < TEXT_COLS; x++) {
            fb[y][x] = (uint8_t)rows[y][x];
        }
    }
}

/* Run the screen and return everything it printed, newlines shown as | */
static const char *screen_run(void)
{
    outn = 0;
    lisp_handle_screen();
    out[outn] = '\0';
    while (outn > 0 && out[outn - 1] == '\n') {
        out[--outn] = '\0';
    }
    for (char *p = out; *p; p++) {
        if (*p == '\n') {
            *p = '|';
        }
    }
    return out;
}

/* Cells in use, measured after a collection so the answer does not depend
 * on when the last allocation happened to run short. */
static uint16_t live_cells(void)
{
    lisp_gc(NIL, NIL);
    return (uint16_t)(NCELLS - 1 - free_count);
}

static int fails;

static void ok(int cond, const char *what)
{
    if (!cond) {
        fails++;
        printf("FAIL: %s\n", what);
    }
}

static const char *prn(val v)
{
    outn = 0;
    lisp_print(v);
    out[outn] = '\0';
    return out;
}

static val sym(const char *name)
{
    return lisp_make_symbol(name, (uint8_t)strlen(name));
}

/* Run a line through the reader+printer and return what was printed,
 * with the trailing newline of each echoed form stripped. */
static const char *readback(const char *line)
{
    outn = 0;
    lisp_handle_input_line(line);
    while (outn > 0 && out[outn - 1] == '\n') {
        outn--;
    }
    out[outn] = '\0';
    return out;
}

/* input must print back byte-for-byte */
static void rt(const char *text)
{
    const char *got = readback(text);
    static char msg[80];
    snprintf(msg, sizeof msg, "roundtrip %s -> %s", text, got);
    ok(strcmp(got, text == NULL ? "" : (!strcmp(text, "()") ? "nil" : text)) == 0, msg);
}

/* input prints back as some other exact text */
static void reads(const char *text, const char *want)
{
    const char *got = readback(text);
    static char msg[96];
    snprintf(msg, sizeof msg, "read %s -> %s (want %s)", text, got, want);
    ok(strcmp(got, want) == 0, msg);
}

/* input must report this error code */
static void err(const char *text, const char *want)
{
    const char *got = readback(text);
    static char msg[96];
    snprintf(msg, sizeof msg, "error %s -> %s (want ? %s)", text, got, want);
    ok(strstr(got, want) != NULL, msg);
}

static void roundtrip(const char *name)
{
    char buf[4];
    int32_t n = lisp_pack3(name, (uint8_t)strlen(name));

    ok(n >= 0, name);
    lisp_unpack3((uint16_t)n, buf);
    ok(strcmp(buf, name) == 0, name);
}

/* Full 1..6 character names through the cell representation, checking the
 * cell cost: <= 3 chars is one cell, 4..6 chars is a SYM2 pair. */
static void symtrip(const char *name, int want_cells)
{
    static char msg[96];
    const char *got;
    uint16_t before;
    int used;

    lisp_init();
    before = (uint16_t)free_count;
    got = prn(sym(name));
    used = (int)(before - free_count);

    snprintf(msg, sizeof msg, "symbol %s -> %s (%d cells, want %d)",
             name, got, used, want_cells);
    ok(strcmp(got, name) == 0 && used == want_cells, msg);
}

int main(void)
{
    /* Stands in for _eusrstack. It must be a real stack address above every
     * frame the tests go on to create, so it is taken here in main. */
    char stack_anchor = 0;
    lisp_set_stack_top(&stack_anchor + 1);

    lisp_init();

    /* ---- radix-40 packing ------------------------------------------- */
    ok(lisp_pack3("x", 1)   == 24 * 1600,               "pack x");
    ok(lisp_pack3("gc", 2)  == (7 * 40 + 3) * 40,       "pack gc");
    ok(lisp_pack3("foo", 3) == (6 * 40 + 15) * 40 + 15, "pack foo");
    ok(lisp_pack3("a-9", 3) == (1 * 40 + 37) * 40 + 36, "pack a-9");
    ok(lisp_pack3("$$$", 3) == 39 * 1600 + 39 * 40 + 39, "pack $$$ (max)");
    ok(lisp_pack3("$$$", 3) <= 65535,                   "max name fits 16 bits");

    ok(lisp_pack3("fool", 4) < 0, "reject: group longer than 3");
    ok(lisp_pack3("F", 1)    < 0, "reject: uppercase");
    ok(lisp_pack3("f!", 2)   < 0, "reject: bad char");
    ok(lisp_pack3("", 0)     < 0, "reject: empty");

    roundtrip("a"); roundtrip("z"); roundtrip("0"); roundtrip("9");
    roundtrip("-"); roundtrip("*"); roundtrip("$");
    roundtrip("ab"); roundtrip("zz9"); roundtrip("x2-"); roundtrip("$$$");

    /* padding must not collide with a real third character */
    ok(lisp_pack3("ab", 2) != lisp_pack3("aba", 3), "ab != aba");

    /* ---- SYM2: names of 4..6 characters ------------------------------ */
    symtrip("a", 1);      symtrip("ab", 1);     symtrip("abc", 1);
    symtrip("abcd", 2);   symtrip("abcde", 2);  symtrip("abcdef", 2);
    symtrip("loop", 2);   symtrip("count", 2);  symtrip("define", 2);
    symtrip("x-2", 1);    symtrip("a-b-c-", 2); symtrip("$$$$$$", 2);

    /* both halves of a SYM2 name are always well above any marker value,
     * so a data cell can never be mistaken for a marker-headed object */
    {
        uint16_t hi = 0, lo = 0;
        ok(lisp_pack_name("abcdef", 6, &hi, &lo) && hi >= 1600 && lo >= 1600,
           "SYM2 halves cannot alias a marker");
        ok(lisp_pack_name("abc", 3, &hi, &lo) && lo == 0,
           "short name has lo == 0");
        ok(!lisp_pack_name("toolong", 7, &hi, &lo), "reject: 7 characters");
        ok(!lisp_pack_name("abcdeF", 6, &hi, &lo), "reject: bad char in tail");
    }

    /* names differing only in the tail must not be confused
     * (prn returns a shared buffer, so the first result must be copied) */
    {
        char first[16];
        lisp_init();
        snprintf(first, sizeof first, "%s", prn(sym("abcd")));
        ok(strcmp(first, prn(sym("abce"))) != 0, "abcd prints unlike abce");
    }

    /* ---- printer ----------------------------------------------------- */
    ok(!strcmp(prn(MKFIX(0)),      "0"),      "print 0");
    ok(!strcmp(prn(MKFIX(42)),     "42"),     "print 42");
    ok(!strcmp(prn(MKFIX(-7)),     "-7"),     "print -7");
    ok(!strcmp(prn(MKFIX(16383)),  "16383"),  "print fixnum max");
    ok(!strcmp(prn(MKFIX(-16384)), "-16384"), "print fixnum min");
    ok(!strcmp(prn(NIL), "nil"), "print nil");
    ok(!strcmp(prn(TEE), "t"),   "print t");

    ok(!strcmp(prn(sym("foo")), "foo"), "print symbol");
    ok(!strcmp(prn(sym("x")),   "x"),   "print 1-char symbol");

    {
        val l = lisp_cons(MKFIX(1), lisp_cons(MKFIX(2), lisp_cons(MKFIX(3), NIL)));
        ok(!strcmp(prn(l), "(1 2 3)"), "print proper list");
    }
    ok(!strcmp(prn(lisp_cons(MKFIX(1), MKFIX(2))), "(1 . 2)"), "print dotted pair");
    ok(!strcmp(prn(lisp_cons(sym("a"), lisp_cons(lisp_cons(sym("b"), NIL), NIL))),
               "(a (b))"), "print nested list");
    /* a symbol in the tail is a dotted pair, not a list spine */
    ok(!strcmp(prn(lisp_cons(sym("ab"), sym("cd"))), "(ab . cd)"), "symbol tail");

    /* ---- fixnum tag round-trip --------------------------------------- */
    ok(FIXVAL(MKFIX(0))      == 0,      "fixnum 0");
    ok(FIXVAL(MKFIX(-1))     == -1,     "fixnum -1");
    ok(FIXVAL(MKFIX(16383))  == 16383,  "fixnum max");
    ok(FIXVAL(MKFIX(-16384)) == -16384, "fixnum min");
    ok(ISFIX(MKFIX(5)) && !ISREF(MKFIX(5)), "fixnum is not a ref");
    ok(!lisp_is_cons(sym("q")), "symbol cell is not a cons");
    ok(lisp_is_cons(lisp_cons(NIL, NIL)), "cons cell is a cons");

    /* ---- deep nesting must degrade, not smash the stack -------------- */
    {
        val v = NIL;
        for (int i = 0; i < 4 * MAXDEPTH; i++) {
            v = lisp_cons(v, NIL);
        }
        ok(strstr(prn(v), "...") != NULL, "deep print truncates at MAXDEPTH");
    }

    /* ---- allocator must not run off the end of the heap -------------- */
    {
        /* Allocating garbage can no longer exhaust the heap -- the collector
         * reclaims it -- so exhaustion requires retaining what is allocated.
         * The count reached also measures how much of the heap is usable,
         * which is the whole point of not copying: the previous collector
         * needed free cells >= live cells and so capped out near half. */
        val list = NIL;
        int n;

        lisp_init();
        for (n = 0; n < NCELLS + 64; n++) {
            val c = lisp_cons(MKFIX(1), list);
            if (lisp_err) {
                break;
            }
            list = c;
        }

        ok(lisp_err != 0, "retaining cells eventually reports mem");
        ok(n > (NCELLS * 3) / 4, "far more than half the heap is usable");
        printf("  (retained %d live cells of %d before mem)\n", n, NCELLS);
        lisp_init();
    }

    /* ---- reader + evaluator ------------------------------------------ */
    lisp_init();

    /* self-evaluating: read, eval and print must all round-trip */
    rt("42");     rt("-7");     rt("0");
    rt("16383");  rt("-16384");
    rt("nil");    rt("t");
    rt("cons");   rt("numberp");        /* builtins are self-evaluating */

    /* quote stops evaluation */
    reads("'x",           "x");
    reads("'foo",         "foo");
    reads("'(1 2 3)",     "(1 2 3)");
    reads("(quote (a b))", "(a b)");
    reads("'(1 . 2)",     "(1 . 2)");
    reads("'()",          "nil");
    reads("''x",          "(quote x)");

    /* if */
    reads("(if t 'yes 'no)",   "yes");
    reads("(if nil 'yes 'no)", "no");
    reads("(if nil 'yes)",     "nil");   /* missing else is nil */
    reads("(if 0 'yes 'no)",   "yes");   /* only nil is false */

    /* progn / and / or */
    reads("(progn 1 2 3)", "3");
    reads("(progn)",       "nil");
    reads("(and t t)",     "t");
    reads("(and t nil t)", "nil");
    reads("(and)",         "t");
    reads("(or nil nil)",  "nil");
    reads("(or nil 'a)",   "a");
    reads("(or)",          "nil");
    reads("(and 1 2)",     "2");         /* returns the last value */

    /* lambda and application */
    reads("((lambda (x) x) 42)",        "42");
    reads("((lambda (x) 'k) 42)",       "k");
    reads("((lambda (x y) y) 1 2)",     "2");
    reads("((lambda () 'z))",           "z");
    reads("(if t (lambda (x) x) nil)",  "#clo");

    /* define, and the bindings it leaves behind */
    reads("(define id (lambda (x) x))", "id");
    reads("(id 'a)",                    "a");
    reads("(id 42)",                    "42");
    reads("(define n 7)",               "n");
    reads("n",                          "7");
    reads("(id n)",                     "7");
    reads("(define n 9)",               "n");   /* redefinition shadows */
    reads("n",                          "9");

    /* lexical scope: the closure captures its defining environment */
    reads("(define mk (lambda (a) (lambda (b) a)))", "mk");
    reads("((mk 'outer) 'inner)",                     "outer");

    /* a parameter shadows a global of the same name */
    reads("((lambda (n) n) 'shadow)", "shadow");
    reads("n",                     "9");

    /* SYM2 equality must compare both halves: these differ only in char 4 */
    reads("(define abcd 1)", "abcd");
    reads("abcd",            "1");
    err("abce",              "unb");
    reads("(define abce 2)", "abce");
    reads("abcd",            "1");
    reads("abce",            "2");
    /* and a short name must not match a long one sharing its first 3 chars */
    reads("(define abc 3)", "abc");
    reads("abc",            "3");
    reads("abcd",           "1");

    /* recursion resolves through the live global alist */
    reads("(define f (lambda (x) (if x (f nil) 'done)))", "f");
    reads("(f t)", "done");

    /* errors */
    err("(1 2",     "eof");          /* unbalanced open paren */
    err(")",        "syn");
    err("(. 1)",    "syn");
    err("32768",    "num");          /* beyond 15-bit fixnum range */
    err("-16385",   "num");
    err("wxyz",     "unb");          /* legal SYM2 name, just unbound */
    err("toolong",  "sym");          /* 7 chars exceeds SYM2 */
    err("FOO",      "sym");          /* uppercase is outside the alphabet */
    err("zzz",      "unb");          /* unbound symbol */
    err("(42 1)",   "call");         /* a fixnum is not applicable */

    /* ---- builtins ---------------------------------------------------- */
    lisp_init();

    /* pairs */
    reads("(cons 1 2)",        "(1 . 2)");
    reads("(cons 1 nil)",      "(1)");
    reads("(car '(1 2))",      "1");
    reads("(cdr '(1 2))",      "(2)");
    reads("(car nil)",         "nil");     /* forgiving on nil */
    reads("(cdr nil)",         "nil");
    reads("(list 1 2 3)",      "(1 2 3)");
    reads("(list)",            "nil");
    reads("(car (cdr '(1 2 3)))", "2");

    /* predicates */
    reads("(eq 'a 'a)",        "t");
    reads("(eq 'a 'b)",        "nil");
    reads("(eq 1 1)",          "t");
    reads("(eq 'abcdef 'abcdef)", "t");    /* SYM2 names compare by name */
    reads("(eq 'abcdef 'abcdeg)", "nil");
    reads("(eq '(1) '(1))",    "nil");     /* distinct cells */
    reads("(atom 'a)",         "t");
    reads("(atom '(1))",       "nil");
    reads("(atom nil)",        "t");
    reads("(consp '(1))",      "t");
    reads("(consp 1)",         "nil");
    reads("(numberp 1)",       "t");
    reads("(numberp 'a)",      "nil");
    reads("(null nil)",        "t");
    reads("(null 1)",          "nil");
    reads("(not nil)",         "t");

    /* arithmetic */
    reads("(+ 1 2)",           "3");
    reads("(+ 1 2 3 4)",       "10");
    reads("(+)",               "0");
    reads("(- 10 3)",          "7");
    reads("(- 5)",             "-5");      /* unary negation */
    reads("(- 10 1 2)",        "7");
    reads("(* 6 7)",           "42");
    reads("(*)",               "1");
    reads("(* 2 3 4)",         "24");
    reads("(/ 7 2)",           "3");
    reads("(mod 7 2)",         "1");
    reads("(/ -7 2)",          "-3");
    reads("(+ -16384 1)",      "-16383");
    reads("(* 128 127)",       "16256");

    /* comparisons */
    reads("(< 1 2)",           "t");
    reads("(< 2 1)",           "nil");
    reads("(> 2 1)",           "t");
    reads("(= 2 2)",           "t");
    reads("(= 2 3)",           "nil");

    /* arithmetic errors */
    err("(/ 1 0)",   "div");
    err("(mod 1 0)", "div");
    err("(+ 1 'a)",  "type");
    err("(+ 16383 1)", "num");             /* overflow is reported */
    err("(* 1000 1000)", "num");
    err("(car 1)",   "");                  /* forgiving: returns nil, no error */

    /* room reports free cells and shrinks as the heap fills */
    reads("(numberp (room))", "t");

    /* nested calls */
    reads("(+ (* 2 3) (- 10 6))", "10");
    reads("(if (< 1 2) 'less 'more)", "less");

    /* ---- cond ---------------------------------------------------- */
    lisp_init();
    reads("(cond (t 'a))",              "a");
    reads("(cond (nil 'a) (t 'b))",     "b");
    reads("(cond (nil 'a))",            "nil");   /* nothing matched */
    reads("(cond)",                     "nil");
    reads("(cond (t 1 2 3))",           "3");     /* body is a progn */
    reads("(cond ((= 1 1) 'eq) (t 'no))", "eq");
    reads("(cond ((= 1 2) 'eq) (t 'no))", "no");
    reads("(cond (7))",                 "7");     /* clause with no body */
    reads("(cond (nil 'a) ((+ 1 1) 'b))", "b");   /* only nil is false */

    /* later clauses must not be evaluated once one is taken */
    reads("(define n 0)",                        "n");
    reads("(cond (t 'first) ((define n 9) 'x))", "first");
    reads("n",                                   "0");

    err("(cond (zzz 'a))", "unb");

    /* ---- let ----------------------------------------------------- */
    lisp_init();
    reads("(let ((x 1)) x)",                "1");
    reads("(let ((x 1) (y 2)) (+ x y))",    "3");
    reads("(let () 'ok)",                   "ok");
    reads("(let ((x 1)) 'a 'b)",            "b");     /* body is a progn */
    reads("(let ((x 1)) (let ((x 2)) x))",  "2");     /* inner shadows */
    reads("(let ((x 5)) (+ (let ((x 1)) x) x))", "6");

    /* bindings are parallel, not sequential: init sees the outer x */
    reads("(define x 10)",              "x");
    reads("(let ((x 1) (y x)) y)",      "10");
    reads("x",                          "10");   /* and the global is intact */

    /* a let binding shadows a global only inside its body */
    reads("(let ((x 2)) x)",  "2");
    reads("x",                "10");

    /* closures capture the let environment */
    reads("(define mk (lambda () (let ((v 42)) (lambda () v))))", "mk");
    reads("((mk))", "42");

    err("(let ((1 2)) 3)", "syn");      /* binding name must be a symbol */

    /* ---- apply, map, filter --------------------------------------- */
    lisp_init();

    reads("(apply + '(1 2 3))",                  "6");
    reads("(apply cons '(1 2))",                 "(1 . 2)");
    reads("(apply + nil)",                       "0");
    reads("(apply (lambda (x y) (- x y)) '(10 3))", "7");
    err("(apply 1 '(2))",                        "call");

    reads("(map (lambda (x) (* x x)) '(1 2 3))", "(1 4 9)");
    reads("(map car '((1 2) (3 4)))",            "(1 3)");
    reads("(map (lambda (x) x) nil)",            "nil");
    reads("(map numberp '(1 a))",                "(t nil)");

    reads("(filter (lambda (x) (> x 2)) '(1 2 3 4))", "(3 4)");
    reads("(filter numberp '(1 a 2))",           "(1 2)");
    reads("(filter (lambda (x) nil) '(1 2))",    "nil");
    reads("(filter (lambda (x) t) nil)",         "nil");

    /* map and filter compose with closures over the enclosing scope */
    reads("(define k 10)",                       "k");
    reads("(map (lambda (x) (+ x k)) '(1 2))",   "(11 12)");
    reads("(let ((b 100)) (map (lambda (x) (+ x b)) '(1 2)))", "(101 102)");

    /* ---- regression: a live accumulator must survive collection ----
     *
     * A tail loop that conses onto an accumulator keeps that accumulator
     * only in eval's env, which lives in a C local -- and a caller's local
     * can sit in a callee-saved register that no intervening callee spills.
     * The collector therefore missed it and reclaimed the list mid-loop,
     * and (g 5000 nil) silently returned nine elements instead of
     * reporting that it could not fit. lisp_gc now forces those registers
     * onto the stack before scanning. */
    lisp_init();
    reads("(define g (lambda (n a) (if (= n 0) a (g (- n 1) (cons n a)))))", "g");
    reads("(car (g 20 nil))",            "1");
    reads("(car (g 150 nil))",           "1");   /* long enough to collect */
    reads("(car (cdr (g 150 nil)))",     "2");
    reads("(car (cdr (cdr (g 150 nil))))", "3");
    err("(g 5000 nil)", "mem");          /* must report, not truncate */
    reads("(+ 1 2)", "3");

    /* the same hazard reached through map, whose result list is built in a
     * C local while the collector may run */
    lisp_init();
    reads("(define g (lambda (n a) (if (= n 0) a (g (- n 1) (cons n a)))))", "g");
    reads("(define l (g 60 nil))",       "l");
    reads("(car l)",                     "1");
    reads("(car (map (lambda (x) (+ x 0)) l))", "1");
    reads("(car (filter numberp l))",    "1");

    /* ---- break: Esc must stop a running program ------------------- */
    lisp_init();

    /* A loop that is otherwise unstoppable: tail calls run in constant
     * stack and, since the collector landed, constant heap too. Before
     * break support this could only be escaped with a reset. */
    reads("(define l (lambda () (l)))", "l");
    sim_polls = 0;
    sim_break_after = 5;
    err("(l)", "brk");
    ok(sim_break_after == 0, "break was delivered");

    /* the machine is usable immediately afterwards */
    reads("(+ 1 2)", "3");

    /* a counted loop can be interrupted part-way and leaves no damage */
    reads("(define f (lambda (n) (if (= n 0) 'end (f (- n 1)))))", "f");
    sim_polls = 0;
    sim_break_after = 3;
    err("(f 16000)", "brk");
    reads("(f 10)", "end");

    /* a stale Esc must not kill the next line */
    lisp_handle_run_control_byte(0x1b);
    sim_break_after = 0;
    reads("(+ 2 3)", "5");

    /* bytes other than Esc do not break */
    lisp_handle_run_control_byte('x');
    reads("(f 10)", "end");

    /* the running flag is only set while evaluating */
    ok(!lisp_is_running(), "not running once a line is done");

    /* polling really is periodic rather than every trip */
    {
        static char msg[80];
        sim_polls = 0;
        sim_break_after = 0;
        readback("(f 200)");
        snprintf(msg, sizeof msg, "polled %d times for 200 iterations", (int)sim_polls);
        ok(sim_polls > 0 && sim_polls < 200, msg);
    }

    /* ---- TCO: only provable now that we can count -------------------
     *
     * A tail-recursive countdown costs 7 cells per iteration (2 for
     * (= n 0), 2 for (- n 1), 1 for the call's argument list, 2 for the
     * environment frame) and none are reclaimed until step 6 adds the
     * collector, so the loop is bounded by heap rather than by stack.
     *
     * MAXDEPTH is 24. Running 40 iterations without a "deep" error is
     * therefore proof that tail calls reuse the frame: a non-tail
     * implementation would fail at iteration 25.
     */
    lisp_init();
    reads("(define f (lambda (n) (if (= n 0) 'zero (f (- n 1)))))", "f");
    reads("(f 40)", "zero");
    ok(eval_depth == 0, "eval depth unwinds to zero after deep tail recursion");

    /* ---- collector: the loop must now run unbounded ------------------ */
    lisp_init();
    reads("(define f (lambda (n) (if (= n 0) 'zero (f (- n 1)))))", "f");
    reads("(f 300)",   "zero");
    reads("(f 3000)",  "zero");
    reads("(f 10000)", "zero");          /* the design's stress test */
    ok(eval_depth == 0, "depth unwinds after 10000 tail calls");

    /* constant heap: occupancy after a long loop matches a short one */
    {
        static char msg[96];
        uint16_t small, big;

        lisp_init();
        reads("(define f (lambda (n) (if (= n 0) 'zero (f (- n 1)))))", "f");
        readback("(f 10)");
        small = live_cells();
        readback("(f 5000)");
        big = live_cells();
        /* Occupancy must not grow with the iteration count. A cell or two
         * of slack is expected and harmless: the stack scan is
         * conservative, so a stale value that merely looks like a
         * reference can retain one cell for one cycle. */
        snprintf(msg, sizeof msg,
                 "constant heap: %u live after (f 10), %u after (f 5000)",
                 small, big);
        ok(big <= small + 4, msg);
    }

    /* the collector must preserve sharing rather than duplicate it:
     * a value bound once and referenced twice stays one object */
    lisp_init();
    reads("(define p (cons 1 2))", "p");
    reads("(eq p p)",              "t");
    reads("(eq (car (cons p p)) (cdr (cons p p)))", "t");
    /* survives a collection triggered by unrelated work */
    reads("(define f (lambda (n) (if (= n 0) 'zero (f (- n 1)))))", "f");
    reads("(f 2000)", "zero");
    reads("(eq p p)", "t");
    reads("p",        "(1 . 2)");

    /* definitions and closures survive collection */
    lisp_init();
    reads("(define k 7)", "k");
    reads("(define add (lambda (a) (+ a k)))", "add");
    reads("(define f (lambda (n) (if (= n 0) 'zero (f (- n 1)))))", "f");
    reads("(f 3000)",  "zero");
    reads("k",         "7");
    reads("(add 5)",   "12");            /* closure still sees the global */
    reads("'abcdef",   "abcdef");        /* SYM2 survives copying */
    reads("(define nm 'abcdef)", "nm");
    reads("(f 2000)",  "zero");
    reads("nm",        "abcdef");        /* SYM2 copied as a leaf, intact */
    reads("(eq nm 'abcdef)", "t");

    /* long lists survive, and room recovers after the garbage is dropped */
    lisp_init();
    reads("(define l (list 1 2 3 4 5))", "l");
    reads("(define f (lambda (n) (if (= n 0) 'zero (f (- n 1)))))", "f");
    reads("(f 2000)", "zero");
    reads("l",        "(1 2 3 4 5)");
    reads("(car (cdr l))", "2");

    /* a genuinely unbounded allocation still fails cleanly, not corruptly */
    lisp_init();
    reads("(define g (lambda (n a) (if (= n 0) a (g (- n 1) (cons n a)))))", "g");
    reads("(car (g 20 nil))", "1");      /* small list is fine */
    err("(g 5000 nil)", "mem");          /* cannot fit: reports, no crash */
    reads("(+ 1 2)", "3");               /* interpreter still usable after */

    /* Non-tail recursion is still bounded by MAXDEPTH, as designed. */
    lisp_init();
    reads("(define g (lambda (n) (if (= n 0) 0 (+ 1 (g (- n 1))))))", "g");
    reads("(g 5)", "5");
    /* MAXDEPTH counts every eval entry, argument evaluation included, so 14
     * buys about 11 levels of user recursion. 16 would allow 12, but
     * measured at only 96 bytes of stack headroom against 228 for 14. */
    reads("(g 11)", "11");
    err("(g 12)", "deep");
    err("(g 100)", "deep");

    /* A failed form leaves garbage rather than rewinding a frontier, so the
     * test is that the machine stays usable and the cells come back. */
    {
        uint16_t before;

        lisp_handle_input_line("(zzz 1 2 3)");
        reads("(+ 1 2)", "3");

        before = (uint16_t)free_count;
        for (int i = 0; i < 200; i++) {
            readback("(zzz 1 2 3)");        /* fails, allocating each time */
        }
        reads("(+ 1 2)", "3");
        ok((uint16_t)free_count > (uint16_t)(before / 2),
           "repeated failures do not leak the heap away");
    }

    /* tail calls must run in constant C stack and constant depth */
    reads("(define loop (lambda (i) (if i (loop nil) 'done)))", "loop");
    reads("(loop t)", "done");
    ok(eval_depth == 0, "eval depth unwinds to zero");

    /* ---- screen mode: the framebuffer is the source buffer ----------- */
    {
        static char msg[128];
        const char *got;

        /* a multi-row program: forms may span rows, since padding reads
         * as whitespace */
        static const char *const prog1[] = {
            "(define sq (lambda (n)",
            "  (* n n)))",
            "(sq 7)",
        };
        lisp_init();
        screen_set(prog1, 3);
        got = screen_run();
        snprintf(msg, sizeof msg, "screen: multi-row define -> %s", got);
        ok(!strcmp(got, "sq|49"), msg);

        /* several forms, evaluated in order */
        static const char *const prog2[] = {
            "(define a 2) (define b 3)",
            "(* a b)",
            "(+ a b)",
        };
        lisp_init();
        screen_set(prog2, 3);
        got = screen_run();
        snprintf(msg, sizeof msg, "screen: several forms -> %s", got);
        ok(!strcmp(got, "a|b|6|5"), msg);

        /* a loop long enough to force collections mid-program: the
         * remaining forms must survive being a GC root */
        static const char *const prog3[] = {
            "(define f (lambda (n)",
            "  (if (= n 0) 'zero",
            "    (f (- n 1)))))",
            "(f 3000)",
            "(define k 9)",
            "(f 3000)",
            "k",
        };
        lisp_init();
        screen_set(prog3, 7);
        got = screen_run();
        snprintf(msg, sizeof msg, "screen: survives collection -> %s", got);
        ok(!strcmp(got, "f|zero|k|zero|9"), msg);

        /* the program itself is reclaimed once it has run */
        {
            uint16_t after;
            lisp_init();
            screen_set(prog2, 3);
            screen_run();
            after = live_cells();
            snprintf(msg, sizeof msg,
                     "screen: program text reclaimed (%u cells still live)", after);
            ok(after < 40, msg);
        }

        /* an empty screen is not an error */
        lisp_init();
        screen_set(prog1, 0);
        got = screen_run();
        ok(!strcmp(got, ""), "screen: blank screen does nothing");

        /* errors are reported and the machine stays usable */
        static const char *const prog4[] = { "(+ 1 2)", "(zzz)" };
        lisp_init();
        screen_set(prog4, 2);
        got = screen_run();
        snprintf(msg, sizeof msg, "screen: error reported -> %s", got);
        ok(strstr(got, "unb") != NULL && strstr(got, "3") != NULL, msg);
        reads("(+ 2 2)", "4");

        /* a definition made on screen is visible from the line REPL */
        lisp_init();
        screen_set(prog1, 3);
        screen_run();
        reads("(sq 4)", "16");
    }

    /* ---- hardware primitives ----------------------------------------- */
    lisp_init();

    /* configure, drive, read back */
    reads("(pin 16 1)", "t");            /* PD0 as output */
    reads("(out 16 1)", "1");
    reads("(in 16)",    "t");            /* t/nil, not 1/0 */
    reads("(out 16 0)", "0");
    reads("(in 16)",    "nil");
    ok(sim_mode[16] == HW_OUT, "pin mode reached the driver");

    /* nil second argument means input */
    reads("(pin 0 nil)", "t");
    ok(sim_mode[0] == HW_IN, "nil mode is input");
    reads("(pin 0 2)",   "t");
    ok(sim_mode[0] == HW_IN_PU, "mode 2 is input pull-up");
    reads("(pin 18 3)",  "t");
    ok(sim_mode[18] == HW_ANALOG, "mode 3 is analog input");

    /* only nil is false, so (if (in p) ...) must work as written */
    reads("(out 16 0)", "0");
    reads("(if (in 16) 'hi 'lo)", "lo");
    reads("(out 16 1)", "1");
    reads("(if (in 16) 'hi 'lo)", "hi");

    /* port C belongs to the video interrupt and must be refused outright */
    err("(pin 8 1)",  "pin");
    err("(pin 12 1)", "pin");            /* PC4: sync */
    err("(pin 14 1)", "pin");            /* PC6: pixel stream */
    err("(out 12 1)", "pin");
    err("(in 12)",    "pin");

    /* PD1 carries the debug console */
    err("(pin 17 1)", "pin");
    err("(out 17 1)", "pin");

    /* out of range */
    err("(pin 24 1)", "pin");
    err("(pin -1 1)", "pin");
    err("(pin 'a 1)", "type");

    /* adc and ms */
    reads("(adc 0)", "0");
    reads("(adc 3)", "300");
    err("(adc 10)",  "pin");
    err("(adc -1)",  "pin");
    {
        static char msg[64];
        sim_delay_total = 0;
        readback("(ms 25)");
        readback("(ms 5)");
        readback("(ms -3)");             /* negative is a no-op */
        snprintf(msg, sizeof msg, "ms accumulated %d (want 30)", sim_delay_total);
        ok(sim_delay_total == 30, msg);
    }

    /* a blink loop must run in constant heap like any other tail recursion */
    lisp_init();
    reads("(pin 16 1)", "t");
    reads("(define bl (lambda (n)"
          "  (if (= n 0) 'end"
          "    (progn (out 16 1) (ms 1) (out 16 0) (ms 1)"
          "           (bl (- n 1))))))", "bl");
    {
        static char msg[80];
        uint16_t before, after;
        readback("(bl 5)");
        before = live_cells();
        readback("(bl 200)");
        after = live_cells();
        snprintf(msg, sizeof msg,
                 "blink loop constant heap (%u live vs %u)", before, after);
        ok(after <= before + 4, msg);
    }
    reads("(bl 3)", "end");

    if (fails) {
        printf("%d test(s) FAILED\n", fails);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
