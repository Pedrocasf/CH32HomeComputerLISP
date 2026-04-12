# Architecture

Experiment `16_tiny_basic_textmode` keeps the stable scanout path from
experiment `15_monitor_textmode`, including the later direct scanline cleanup:

- `TIM1` defines the PAL line period
- `TIM1 CH4` generates HSYNC
- `TIM1 CH3` marks active-video start
- `SPI1` shifts `16-bit` pixel words at `6 MHz`
- `DMA1 Channel 3` feeds SPI from a `2`-line ping-pong buffer
- the hot glyph packer runs from SRAM

## Framebuffer Model

- logical screen: `32x25` text cells
- character size: `8x8`
- video area: `256x200` active text pixels centered inside the `256` active PAL
  lines
- character data lives in SRAM as `text_vram[25][32]`
- glyphs stay in flash via `font_8x8_rowmajor.h`

Two SRAM line buffers form a strict ping-pong pair for active scanout.

The blinking cursor is drawn during scanout as a one-row underline overlay on
the current cell. It is not stored in `text_vram`.

## BASIC Runtime

The new experiment-specific part is the interpreter layer:

- `FUNCONF_USE_DEBUGPRINTF=1` keeps the `ch32fun` monitor input path
- `main()` continuously calls `poll_input()`
- `handle_debug_input()` edits framebuffer rows directly
- `Enter` submits the current cursor row into the BASIC runtime

Module split:

- `main.c` keeps startup and monitor-input glue
- `video_textmode.c` owns framebuffer storage, PAL scanout, and IRQ handlers
- `console_textmode.c` owns terminal/editor behavior
- `basic_runtime.c` owns BASIC parsing, heap management, and execution

Program representation:

- fixed `640`-byte BASIC heap
- tokenless stored line text
- each record stores line number, text length, text bytes, and terminator
- records remain sorted by line number
- target lines are found by scanning the program store at runtime
- program text grows upward from the bottom of BASIC memory
- variable records grow upward above the program
- string storage grows downward from the top of BASIC memory
- live strings are compacted by a simple garbage collector

Runtime state:

- variables `A..Z` are created on demand inside BASIC memory
- signed `16-bit` arithmetic
- numeric built-ins: `RND`, `LEN`, `ASC`
- string built-in: `CHR$`
- `FOR/NEXT` loop stack with fixed nesting depth
- `GOSUB/RETURN` stack with fixed nesting depth
- `A$..Z$` string variables use heap-backed descriptors plus string data
- `INPUT` suspends the foreground interpreter and resumes on the submitted row
- `REM` is stored as plain text and ignored at execution time
- no persistence
- no arrays or `PEEK/POKE` yet

## Console Behavior

Console layout:

- rows `0..24`: single full-screen console buffer
- startup prints a C64-style banner into normal text VRAM
- `Ctrl+L` clears the same buffer, so the boot banner is not persistent

Input handling:

- line number + statement -> store or replace program line
- bare line number -> delete program line
- line without number -> execute immediately

Supported editing controls:

- `Backspace` / `Delete`
- `Tab`
- `Ctrl+L`
- ANSI arrow keys

Runtime control:

- `Esc` requests interpreter break while a program is running

## Timing Separation

The critical boundary remains the same as in experiments `14` and `15`:

- all PAL-critical work stays in the ISR/DMA scanout path
- BASIC parsing, editing, and execution happen only in the foreground loop
- interpreter writes update `text_vram`, which the existing beam-raced renderer
  picks up on subsequent lines

So experiment `16` changes the semantics of the text console, not the PAL
transport itself.

## Current Scanline Pipeline

The active-video path is deterministic:

- at `VSYNC_END`, `TIM1_UP_IRQHandler` renders active line `0` into buffer `0`
- for each active PAL line `N`, `TIM1_CC_IRQHandler`:
  - starts DMA from buffer `N & 1`
  - renders active line `N + 1` into buffer `(N + 1) & 1`

There is no separate producer/display cursor or queue-depth tracking in the
scanout path anymore.
