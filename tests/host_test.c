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
    before = heap_top;
    got = prn(sym(name));
    used = (int)(heap_top - before);

    snprintf(msg, sizeof msg, "symbol %s -> %s (%d cells, want %d)",
             name, got, used, want_cells);
    ok(strcmp(got, name) == 0 && used == want_cells, msg);
}

int main(void)
{
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
        val v = NIL;
        int n = 0;
        while ((v = lisp_cons(NIL, NIL)) != NIL) {
            n++;
        }
        ok(heap_top == NCELLS, "allocation stops exactly at NCELLS");
        printf("  (heap exhausted after %d conses of %d cells)\n", n, NCELLS);
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
        small = heap_top;
        readback("(f 5000)");
        big = heap_top;
        snprintf(msg, sizeof msg,
                 "constant heap: after (f 10) %u cells, after (f 5000) %u",
                 small, big);
        ok(small == big, msg);
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
    err("(g 100)", "deep");

    /* a failed form must not leave cells behind */
    {
        uint16_t before = heap_top;
        lisp_handle_input_line("(zzz 1 2 3)");
        ok(heap_top == before, "frontier rewound after a failed form");
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
            after = heap_top;
            snprintf(msg, sizeof msg,
                     "screen: program text reclaimed (%u cells retained)", after);
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

    if (fails) {
        printf("%d test(s) FAILED\n", fails);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
