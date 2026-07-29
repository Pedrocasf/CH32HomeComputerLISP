# CH32 Home Computer — LISP design

Replacing the line-numbered BASIC with a Lisp, on the **CH32V002**, coexisting
with the existing PAL video generator and a small screen editor.

Every number below was measured on this tree with `riscv-wch-elf-gcc 12.2.0`
(see §7 for the commands), not estimated.

---

## 1. Findings from the current tree

### 1.1 The V002 target does not build yet

`Makefile` has an uncommitted change from `TARGET_MCU:=CH32V003` to
`CH32V002`, and in that state the build fails. The cause is purely a header
naming difference, not a hardware one:

- `CH32V003` pulls in `ch32v003hw.h`, which spells TIM bit masks unprefixed:
  `TIM_CC4E`, `TIM_MOE`, `TIM_CEN`, …
- `CH32V002` pulls in `ch32x00xhw.h` (via `ch32fun.h:409`), which spells the
  *same bits with the same values* peripheral-prefixed: `TIM1_CCER_CC4E`,
  `TIM1_BDTR_MOE`, `TIM1_CTLR1_CEN`, …

Exactly **13 macros** in `video_textmode.c` are affected, each with a 1:1
equivalent:

| used in `video_textmode.c` | CH32V002 header |
|---|---|
| `TIM_UIF` / `TIM_CC3IF` | `TIM1_INTFR_UIF` / `TIM1_INTFR_CC3IF` |
| `TIM_UIE` / `TIM_CC3IE` | `TIM1_DMAINTENR_UIE` / `TIM1_DMAINTENR_CC3IE` |
| `TIM_CC4E` / `TIM_CC4P` | `TIM1_CCER_CC4E` / `TIM1_CCER_CC4P` |
| `TIM_OC4PE` / `TIM_OC4M_1` / `TIM_OC4M_2` | `TIM1_CHCTLR2_*` |
| `TIM_MOE` | `TIM1_BDTR_MOE` |
| `TIM_ARPE` / `TIM_CEN` | `TIM1_CTLR1_ARPE` / `TIM1_CTLR1_CEN` |
| `TIM_UG` | `TIM1_SWEVGR_UG` |

A force-included compatibility header (`-include v002_compat.h` via
`EXTRA_CFLAGS`, which `ch32fun.mk:370` already honours) fixes it with **zero
edits to project sources**. The vendored `ch32fun` submodule (2026-03-16) is
new enough — it knows `CH32V002`, `TARGET_MCU_LD:=5`, 16 K flash / 4 K RAM.

**Status: done and verified on hardware** (`v002_compat.h` + `Makefile`):

- The 13 macros emit **byte-identical values** through the shim as the V003
  header does natively (compiled both ways, compared `.rodata`).
- Generated video code is functionally identical between targets. A
  normalized disassembly diff of `video_textmode_init`, both TIM1 ISRs and
  both render functions shows only two artifact classes: a 4-byte image
  shift, and `_eusrstack`-relative annotations that differ by exactly 0x800
  because RAM grew 2 K → 4 K. `text_vram` and `line_buffers` land at
  identical addresses in both builds.
- Flashed to a real CH32V002 (`Part Type 00-21-06-20`, 16 kB). Reading
  `text_vram` back over SDI reconstructs the boot banner, so the firmware
  boots and console + video init run.
- Field rate measured at **~50.2 Hz** (151–152 field-counter increments per
  3 s of run time) — correct PAL. Note the debugger halts the core to read
  memory, so back-to-back reads show zero elapsed fields; only samples
  separated by real run time are meaningful.

### 1.1a Zmmul is available and now enabled

The V2C core has hardware multiply. `ch32fun.mk` only passes
`-march=rv32ec_zmmul` on gcc ≥ 13, so this tree was silently building with
soft multiply. The WCH toolchain (`riscv-wch-elf-gcc 12.2.0`) **does** accept
`rv32ec_zmmul` and emits a real `mul`; the `Makefile` now forces it for
CH32V002. Result: `__mulsi3` disappears, flash drops 10964 → **10928 B**, and
the banner + 50 Hz field rate are unchanged on hardware after reflashing.

Zmmul is multiply-only — `__divsi3`/`__udivsi3`/`__umodsi3` remain linked.

**Constant division is *not* strength-reduced at `-Os`.** Measured on a test
containing `n/1600u`, `(n/40u)%40u` and `k/10`:

| `-march` | `-Os` | `-O2` |
|---|---|---|
| `rv32ec` | 0 mul, 4 libcalls | 0 mul, 4 libcalls |
| `rv32ec_zmmul` | 0 mul, 4 libcalls | **4 mul, 0 libcalls** |

(`rv32ecm` is rejected — no M multilib for ilp32e, so Zmmul is the only route
to hardware multiply.) ch32fun builds at `-Os`, so *every* division in the
Lisp is a libcall, including the radix-40 unpacker's `/1600` and `/40`. This
costs no extra flash — the firmware already links those routines for BASIC —
and neither the printer nor symbol unpacking is a hot path, so the design
keeps the readable division. Zmmul's win is on genuine multiplies (the `*`
builtin, and BASIC's) plus the 36 bytes of `__mulsi3` it removes.

If a division ever lands on a hot path, the fix is per-function
`__attribute__((optimize("O2")))` rather than raising `-Os` globally.

### 1.2 Measured budget — this is the real constraint

| build | flash | RAM |
|---|---|---|
| BASIC + video + console (V003) | 10960 B / 16 K (67 %) | 1848 B / **2 K** (90 %) |
| BASIC + video + console (V002) | 10964 B / 16 K (67 %) | 1848 B / **4 K** (45 %) |
| **video + console only (V002)** | **4880 B** | **1148 B** |

So the Lisp does **not** get 16 K / 4 K. After the video generator and console
it gets:

> **≈ 11 500 B flash and ≈ 2 950 B RAM** — and the C stack comes out of that RAM.

This is the single most important correction to any "16 K / 4 K Lisp" plan.

Flash, by subsystem (deduplicated `nm`, V003 build):

| subsystem | flash |
|---|---|
| BASIC runtime | 3916 B ← what Lisp replaces |
| video (incl. 2048 B font) | 2882 B |
| console | 1026 B |
| ch32fun runtime, debug I/O, vectors, `main` | 2780 B |

RAM, video + console only:

| | bytes |
|---|---|
| `text_vram` (32×25 framebuffer) | 800 |
| `.data` — render fns run **from RAM** (`.srodata` → `>RAM AT>FLASH`) | 228 |
| `line_buffers` (2 × DMA ping-pong) | 72 |
| `submit_buffer` | 33 |
| misc | ~15 |
| **total** | **1148** |

### 1.3 Constraints the hardware imposes on the language

- **32 columns is locked by the SPI divisor.** `SPI_CTLR1_BR_1` = PCLK/8 =
  6 MHz; 32 chars × 8 px = 256 px in 42.7 µs of the ~52 µs active line. The
  next divisor up is 12 MHz, which would need ~64 columns and a 2 KB
  framebuffer. 40 columns is not reachable. **Lisp must be readable in 32
  columns** — short names, few nested parens per line.
- **Video is ISR-driven and preempts everything.** `TIM1_UP` fires every line
  (15625 Hz) and `TIM1_CC` on each of the 256 active lines. The interpreter
  runs only in the foreground loop and must **never disable interrupts** —
  no critical sections in the GC, no `__disable_irq()` anywhere.
- **The V002 has less CPU headroom than the V003** (README: ~50 % fewer spare
  cycles per frame, one more flash wait state), while having twice the RAM.
  That trade — more memory, less speed — suits an interpreter.
- **Stack overflow is a known failure mode here.** The README already warns
  the BASIC stack "tends to overflow into the screen buffer". Video ISR
  frames nest on top of whatever the interpreter is doing, so the Lisp needs
  a hard recursion bound and a stack canary, not optimism.

---

## 2. Memory plan

### RAM (4096 B)

| region | size | note |
|---|---|---|
| video + console (measured) | 1148 B | unchanged |
| **Lisp cell heap — 384 cells × 4 B** | **1536 B** | the one tunable knob |
| Lisp globals (frontier, watermark, global env, flags) | ~24 B | |
| reader token buffer | 8 B | |
| `jmp_buf` (ILP32E: `ra sp s0 s1`) | 16 B | |
| C stack (foreground + nested video ISRs) | 1024 B | canary-guarded |
| slack | ~340 B | grow the heap into this once stack use is measured |
| **total** | **~3756 B** | |

384 cells is deliberately conservative for the first build. Measure real
stack watermark (fill with a pattern at boot, scan later), then raise
`NCELLS` toward 448–512 with the evidence in hand.

### Flash (16384 B)

| component | estimate |
|---|---|
| video + console + runtime (measured) | 4880 B |
| reader | 1.0–1.4 K |
| printer + heap + radix-40 (**measured: 628 B**) | 628 B |
| `eval` + special forms | 2.0–2.8 K |
| GC | 0.2–0.3 K |
| ~22 builtins | 1.8–2.4 K |
| builtin name table + dispatch | 0.4 K |
| error strings | 0.2 K |
| REPL + editor glue | 0.4–0.7 K |
| libgcc soft divide (`__udivsi3`) | 0.2 K |
| **total** | **≈ 11.5–13.9 K** |

Fits, with roughly 2.5 K of margin in the good case. The margin is real but
not generous — it is why the GC choice in §4 matters.

---

## 3. Value representation

16-bit tagged words; cells are 4 bytes. Unchanged from the standalone design
and well suited here, since a 1536 B heap addresses easily within 14 bits.

```
xxxxxxxxxxxxxxx1   fixnum, 15-bit signed        -16384..16383
xxxxxxxxxxxxxx00   heap ref, cell index >> 2    (NIL = 0, cell 0 never used)
xxxxxxxxxxxxxx10   immediate: singleton / builtin / special form / type marker
```

Heap objects are one cell each, distinguished by a marker in `car`:
cons, `SYM_MARK` (cdr = radix-40 packed name), `CLO_MARK`, `FWD_MARK` (GC only).

**Symbols are radix-40 packed**: 3 chars from a 40-char alphabet in 16 bits
(40³ = 64000 ≤ 65536). One cell per symbol, no string storage, no intern
table, `eq` is an integer compare. Builtin names are matched against the flash
table at read time and never allocated, so they can be spelled in full.

**Names of up to 6 characters are supported, via a chained `SYM2` cell.**
Three characters is what fits one cell, and in practice that was too tight —
writing the step-4 tests hit `? sym` on `loop`, `done`, `outer`, `shadow` and
`count`, forcing contractions like `lp`, `end`, `out`, `shd`. Longer names
therefore use two cells:

```
   <= 3 chars   {SYM_MARK,  packed}                 1 cell
   4..6 chars   {SYM2_MARK, ref} -> {hi, lo}        2 cells
```

The second cell holds two raw packed groups rather than tagged values, so it
must never be scanned as a pair of references. **The collector has to copy it
as a leaf** — this is the one representation detail step 6 cannot get wrong.
It is reachable only through a `SYM2` cell's cdr and is never handed to user
code.

Encoding a group always starts with a real character, never a pad, so `hi`
and `lo` are both ≥ 1600 while the largest marker immediate is 46: a data
cell can never be mistaken for a marker-headed object, and `lo == 0` is an
unambiguous "this name fits in three characters". Equality compares both
halves, so `abc`, `abcd` and `abce` are three distinct symbols.

Measured cost: **312 B of flash** (double the 150 B estimated) plus one extra
cell per long symbol occurrence. Since symbols are not interned, every
occurrence of a 4–6 character name costs 8 bytes of heap until the next
collection — a real consideration on 384 cells, and an argument for keeping
loop variables short even though longer names now work.

---

## 4. Garbage collection: watermark copying

The language is **purely functional** — no `setq`, `setcar`, `setcdr`, no
`while`; loops are tail calls. That guarantees an old cell can never point to
a newer one and that no cycles exist, which is what makes sectorlisp-style
watermark copying sound.

- **Allocation** is a frontier bump. No free list, no mark bits, no side tables.
- **Safepoints:** after each top-level form (roots: global env), and at
  `eval`'s TCO loop head when the heap passes 7/8 full (roots: `{x, env}`).
  C code never registers roots, so there is no `gc_push`/`gc_pop` discipline
  to get wrong.
- **Algorithm:** Cheney copy with forwarding pointers, then slide down to the
  watermark and patch refs by a constant delta. Two deliberate upgrades over
  sectorlisp's 40-byte version: the scan is **iterative** (a recursive copy
  would blow this stack, which is already the project's weak point), and
  **forwarding pointers** stop shared DAGs from being duplicated
  exponentially — fatal on 384 cells.
- **Error recovery is one store:** `fp = W0`.
- **No interrupt masking anywhere**, including during the slide.

This costs ~0.2–0.3 K of flash against ~0.8–1.0 K for mark-sweep, and saves
~190 B of RAM (no mark/state bitmaps, no shadow stack). At 11.5 K total that
saving is worth the loss of mutation.

Cost accepted: dead cells stay pinned until the enclosing eval returns, and an
accumulator loop re-copies its result at each triggered safepoint (worst case
quadratic). The 7/8 threshold keeps that rare.

---

## 5. The editor: the framebuffer *is* the source buffer

This is the design's best trade. `text_vram` is already 800 bytes of RAM that
holds exactly the characters on screen, and `console_textmode.c` already
implements full-screen cursor movement, backspace, tab and clear. So a
screen editor costs **no additional RAM at all** — only a second character
source for the reader.

```c
int src_getc(void);   /* line mode: debug-input queue (existing path)
                         screen mode: scan text_vram left-to-right,
                                      top-to-bottom, ' ' padding trimmed */
```

Two modes, one reader:

- **Line mode (default REPL).** Type a form, press Enter, it is read,
  evaluated and printed — the existing
  `console_handle_byte` → `console_submit_current_row` path, with
  `basic_handle_input_line` replaced by `lisp_handle_input_line`.
  Multi-line forms fall out for free: if parens are still unbalanced at
  end of line, keep accumulating instead of evaluating. A few dozen bytes.
- **Screen mode (`Ctrl-R`).** Read *the whole screen* as source and evaluate
  every top-level form in order. Compose a 25-line program anywhere on the
  glass, move the cursor freely with the arrow keys, hit `Ctrl-R` to run it.
  This is precisely how 8-bit screen editors worked, and here it is nearly
  free because the framebuffer is already the buffer.

Optional extras, tens of bytes each: `Ctrl-K` delete line, `Ctrl-O` open line,
and a paren-match blink on the cursor row.

The limit is honest and worth stating: a program is at most one screenful
(32×25 = 800 chars). There is no room for a separate program store — BASIC
spent 640 B on one, and that RAM is better spent on Lisp cells.

---

## 6. Language

Special forms: `quote if lambda define progn and or`
Builtins (~22): `cons car cdr list` · `eq atom consp numberp null not` ·
`+ - * / mod < > =` · `print princ terpri room`

`define` is top level only and pushes onto the global alist by updating a
*variable*, never a heap cell, preserving the GC invariant. Recursion works
because free variables resolve through the live global alist at call time.

Omitted by design, not by budget: mutation (`setq setcar setcdr while`) —
it breaks the GC invariant. Omitted for budget: strings, floats, macros,
`apply`, `let`, `cond`.

## 6a. Hardware primitives (done, +636 B)

`pin` `out` `in` `adc` `ms`, behind `lisp_hw.h` so `lisp.c` stays free of
ch32fun and host-testable. Pin numbering is flat: 0–7 = PA0–PA7,
8–15 = PC0–PC7, 16–23 = PD0–PD7. Modes: 0 input, 1 output, 2 input pull-up,
3 analog.

**Port C is refused in full, not just its two video pins.** The video
interrupt rewrites the whole of `GPIOC->CFGLR` on every scanline, so any user
configuration there is silently overwritten within microseconds. PD1 is
refused as well — it carries the single-wire debug console. A refused pin is
an error (`? pin`), never a silent no-op.

`in` returns `t`/`nil` rather than 1/0: only `nil` is false here, so a fixnum
result would make `(if (in p) ...)` always take the true branch.

**The ADC needed its own bring-up.** `funAnalogInit`/`funAnalogRead` follow
the classic CH32V003 sequence — calibrate via `RSTCAL`/`CAL`, start with
`ADC_SWSTART` — and on the V002 the conversion never completes, hanging the
machine hard. This was found on hardware: the interpreter stopped mid-probe
and the video ISR never started. `lisp_hw.c` follows ch32fun's own CH32V00x
example instead (`ADC_FLAG_STRT`, no calibration), and the wait loop is
**bounded** — a builtin that can wedge a home computer is worse than one that
can fail, so an unconfigured channel returns `? pin` rather than spinning.

Verified on hardware with video running throughout (148 fields per 3 s):

```
(pin 16 1) (out 16 1) (in 16)   -> t, 1, t      PD0 driven and read back
(pin 14 1)                      -> ? pin        PC6 is the pixel stream
(pin 18 3) (adc 3)              -> t, 588       PD2 analog, real conversion
(ms 200) (room)                 -> nil, 382
```

Deliberately not implemented: `peek`/`poke`. CH32 peripheral addresses
(0x4000_0000+) do not fit a 15-bit fixnum, so they would need to be
base+offset against a flash table of peripheral bases — `(poke 'tim1 12 n)`
— rather than raw addresses.

---

## 7. Reproducing the measurements

No RISC-V toolchain is on `PATH`; this machine has WCH's own at
`~/.platformio/packages/toolchain-riscv/bin` (`riscv-wch-elf-gcc 12.2.0`).

```bash
export PATH="$HOME/.platformio/packages/toolchain-riscv/bin:$PATH"
make PREFIX=riscv-wch-elf TARGET_MCU=CH32V003 main.elf          # baseline, builds
make PREFIX=riscv-wch-elf TARGET_MCU=CH32V002 main.elf          # fails: 13 TIM macros
make PREFIX=riscv-wch-elf TARGET_MCU=CH32V002 \
     EXTRA_CFLAGS="-include v002_compat.h" main.elf             # builds with the shim
```

Per-subsystem flash (dedupe by address — `nm` lists LTO symbols twice):

```bash
riscv-wch-elf-nm --print-size --radix=d main.elf \
  | awk 'NF==4 && ($3=="t"||$3=="T") && !seen[$1]++ {print $2, $4}' | sort -rn | head -20
```

---

## 8. Build order

1. ~~**Land the V002 port first**~~ — **done** (§1.1): `v002_compat.h` +
   `Makefile`, verified booting on real V002 at 50 Hz. Still worth checking
   picture quality on a TV: the README measures the V002's internal RC as too
   jittery for video, and `funconfig.h` selects `FUNCONF_USE_HSE`, so an
   external crystal is strongly advised.
2. ~~radix-40 + printer~~ — **done**: `lisp.c`, `lisp.h`, `tests/host_test.c`.
   Measured by linking it alongside BASIC: **+628 B flash, +1540 B RAM**
   (1536 B heap + globals), i.e. 11556 B / 3388 B total. That transitional
   state leaves only ~708 B of stack; removing BASIC returns ~3.9 K of flash
   and ~694 B of RAM, taking stack headroom back to ~1.4 K.
   `--gc-sections` drops the module until something calls it, so the
   committed firmware size is unchanged for now.
3. ~~Reader, line mode~~ — **done**, and the machine now boots into Lisp.
   `main.c` carries a `USE_LISP` switch (default 1); BASIC is still buildable
   with `-DUSE_LISP=0` and is dropped by `--gc-sections` when unused.
   Measured: **6996 B flash (43 %), 2700 B RAM (66 %)** — 9.4 K of flash and
   ~1.4 K of stack left for eval, GC and builtins, exactly the projection.
   Verified on hardware by flashing a probe that reads five forms at boot and
   reading the framebuffer back:

   ```
   |  *** CH32 LISP V.1 32X25 ***   |
   |READY.                          |
   |(a b 42)                        |   symbols + fixnum
   |(quote x)                       |   'x sugar
   |(1 . -7)                        |   dotted pair, negative
   |(cons 16383 nil)                |   builtin from the flash table
   |? sym                           |   error path ("abcd")
   ```

   Not yet done: multi-line continuation on unbalanced parens (screen mode in
   step 7 covers the multi-line case more directly).
4. ~~`eval`~~ — **done**: alist environments, TCO loop, all seven special
   forms, closures with lexical capture. Costs **1148 B** (well under the
   2.0–2.8 K estimate); firmware now **8144 B flash (50 %), 2700 B RAM**.
   Verified on hardware:

   ```
   (if t 'yes 'no)                        -> yes
   (define id (lambda (x) x))  (id 42)    -> id, 42
   (define mk (lambda (a) (lambda (b) a)))
   ((mk 'out) 'in)                        -> out      lexical capture
   (define f (lambda (x) (if x (f nil) 'end)))
   (f t)                                  -> end      recursion via globals
   zzz                                    -> ? unb
   ```

   Two deviations from this document, both deliberate:

   - **`define` works anywhere, not only at top level.** It pushes onto
     `global_env`, which is a C variable rather than a heap field, so no
     old-to-new store occurs and the collector's invariant holds wherever it
     is called. Enforcing top-level-only would need plumbing for no benefit.
     The consequence for step 6 is firm: **`global_env` must be a root at
     every safepoint**, including the one in eval's TCO loop, or a `define`
     evaluated inside a call would have its cells collected.
   - **Argument lists are evaluated recursively**, not built by appending at
     the tail, so each new cell points only at older ones. Costs one C-stack
     frame per argument, bounded by `MAXDEPTH`.

   TCO is implemented (tail positions `continue` rather than recurse) but
   **not yet proven**: a real test needs a countdown loop, which needs
   arithmetic. The design's `(f 10000)` constant-stack test belongs to step 5.

4a. ~~`SYM2` 6-character names~~ — **done** (§3), **+312 B flash**, firmware
   now **8456 B (52 %), 2700 B RAM**. Verified on hardware:

   ```
   (define count 10)   count       -> count, 10
   (define square (lambda (n) n))
   (square 42)                     -> square, 42
   (define loop (lambda (i) (if i (loop nil) 'done)))
   (loop t)                        -> loop, done
   'abcdef                         -> abcdef
   toolong                         -> ? sym     (7 characters)
   ```
5. ~~Builtins~~ — **done**, all 22, **+760 B flash**; firmware now
   **9216 B (56 %), 2704 B RAM**. Dispatch is a `switch` rather than the
   table of function pointers sketched in §6: 22 pointers would have cost
   88 B plus an indirect call each, and the names already live in their own
   table for the reader. `+`, `-` and `*` are variadic (`-` with one argument
   negates); `/`, `mod` and the comparisons are binary. Fixnum overflow is an
   error rather than a silent wrap.

   **TCO is now proven.** A tail-recursive countdown costs exactly 7 cells
   per iteration (2 for `(= n 0)`, 2 for `(- n 1)`, 1 for the call's argument
   list, 2 for the environment frame). `MAXDEPTH` is 24, so completing 40
   iterations rules out frame growth — a non-tail implementation would fail
   at iteration 25 with `deep`. Non-tail recursion still hits `deep` exactly
   as designed.

   Verified on hardware, including the limit that motivates step 6:

   ```
   (+ 1 2 3 4)                  -> 10
   (* 6 7)                      -> 42
   (cons 'a (list 1 2))         -> (a 1 2)
   (car (cdr '(x y z)))         -> y
   (if (< 1 2) 'less 'more)     -> less
   (f 40)                       -> zero     40 tail calls, constant stack
   (room)                       -> 2        only 2 of 384 cells left
   (/ 1 0)                      -> ? mem    heap exhausted, not "div"
   ```

   **Without a collector the machine is bounded at ~48 iterations of any
   loop.** Nothing is reclaimed until a line fails, so `(room)` falls to 2
   after a single 40-step countdown and the *next* form fails for lack of
   memory — the `(/ 1 0)` above reports `mem` instead of `div` for exactly
   that reason. This is the whole case for step 6.

   One robustness fix landed here: `lisp_cons` now raises `mem` itself
   instead of returning a bare `NIL`. Previously exhaustion propagated a
   broken environment and surfaced later as a misleading `type` error.
6. ~~GC~~ — **done**, **+988 B flash**; firmware **10204 B (62 %), 2708 B RAM**.
   Watermark copying with an iterative Cheney scan and a slide, exactly as
   §4 specifies. Both constraints inherited from earlier steps are honoured:
   `global_env` is a root at both safepoints, and a `SYM2` symbol is copied
   as a unit (header immediately followed by its `{hi, lo}` data cell) so the
   scan steps over the raw cell instead of tracing it as references.

   **The trigger is not the fixed 7/8 the design assumed.** The eval-loop
   safepoint fires when the region allocated since the watermark has grown to
   match the remaining free space. Live data can never exceed that region, so
   this both sizes to-space correctly and makes a loop collect at a steady
   interval instead of at a fixed occupancy.

   Verified on hardware — the design's §13 stress test, on the actual chip:

   ```
   (room)      -> 339      before
   (f 10000)   -> zero     ten thousand tail calls
   (room)      -> 339      after: identical, constant heap
   (add 5)     -> 12       closure still sees the global k = 7
   'abcdef     -> abcdef   SYM2 survives copying
   ```

   Host tests additionally cover sharing preserved across collection
   (`(eq p p)` after 2000 collections' worth of churn), definitions and
   closures surviving, long lists intact, and clean recovery from genuine
   exhaustion.

   Two honest limits:

   - **Usable heap is about half of `NCELLS`.** Copying needs free cells ≥
     live cells, so live data much above ~190 cells cannot be collected. A
     program that genuinely outgrows that reports `mem`; it does not corrupt
     the heap, and the interpreter stays usable afterwards (tested).
   - **Error recovery had a latent hole, now fixed.** A form that defined
     something and then failed left `global_env` pointing at cells the
     frontier rewind had freed. The line handler now saves and restores the
     global environment head alongside `heap_top`.

   Video is unaffected: the collector never masks interrupts, and the field
   counter still advances at ~149 per 3.1 s of run time, matching pre-GC
   measurements. (Exact rate is hard to measure through the debugger, which
   halts the core to read memory — back-to-back reads show zero elapsed
   fields. A TV remains the real check.)
7. ~~Screen mode and stack tuning~~ — **done**, **+428 B flash**; firmware
   **10632 B (65 %), 2732 B RAM**. Still smaller than the BASIC it replaces.

   **`Ctrl-R` runs the whole screen**, verified on hardware — the program was
   typed onto the glass and its results printed underneath:

   ```
   (define sq (lambda (n)      <- one form spanning two rows
     (* n n)))
   (define k 5)
   (sq k)
   (+ (sq 3) (sq 4))
   sq                          <- results
   k
   25
   25
   ```

   Reading and evaluating cannot be interleaved: evaluating prints, printing
   scrolls, and the screen *is* the source. So pass one consumes the entire
   screen into a list of forms and pass two evaluates them. That list is a
   GC root in its own right (`lisp_pending`) because the top-level safepoint
   runs between forms and its watermark predates the whole read. The whole
   program must fit in the heap as cells, since no collection can run while
   reading; a program too large reports `mem`. Clear the screen (`Ctrl-L`)
   before `Ctrl-R` — otherwise the boot banner is parsed as source.

   **The stack was overcommitted, and the measurement found it.** Painting
   the unused stack with a pattern at boot and reading the high-water mark
   back over SDI showed `MAXDEPTH = 24` consuming *all* 1380 bytes of the gap
   between `.bss` and the stack top — i.e. running into `.bss`, exactly the
   failure the README reports for BASIC. Measured frames explain it: one
   level of non-tail evaluation is `lisp_eval` + `lisp_eval_args`, and at
   24 levels that needs roughly 1700–2200 bytes.

   Two changes fixed it. The GC root array in `lisp_eval` is now `static`
   rather than automatic, cutting that frame from **52 to 36 bytes** — it is
   safe because a collection never runs inside another one, and eval frames
   are precisely what bound recursion depth. And `MAXDEPTH` is now **14**,
   chosen from measurement rather than guesswork:

   | MAXDEPTH | user recursion depth | stack high-water | headroom |
   |---|---|---|---|
   | 24 | — | ≥ 1380 B | **overflowed .bss** |
   | 16 | 13 | 1236 B | 128 B (9 %) |
   | **14** | **11** | **1120 B** | **244 B (18 %)** |

   16 is available if depth matters more than margin; given this project's
   history of stack overflow, 14 is the safer default. Note `MAXDEPTH` counts
   every eval entry including argument evaluation, so it is about a third
   larger than the user-visible recursion depth it allows.

   `NCELLS` stays at 384. The stack was bought back by shrinking the frame
   rather than the heap, so no cells were sacrificed.

Check `--print-memory-usage` after every step. Gates: flash ≤ 15 K, RAM ≤ 4 K
with ≥ 1 K of stack left unused under load.
