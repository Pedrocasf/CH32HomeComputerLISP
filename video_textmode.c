#include "video_textmode.h"
#include "TIM.h"
#include "ch32fun.h"
#include "font_8x8_rowmajor.h"
#include <stdint.h>

#define CHAR_WIDTH                     8
#define CHAR_HEIGHT                    8
#define VISIBLE_WIDTH                  (TEXT_COLS * CHAR_WIDTH)
#define VISIBLE_HALFWORDS              (VISIBLE_WIDTH / 16)
#define LINE_GUARD_HALFWORDS           1
#define LINE_HALFWORDS                 (VISIBLE_HALFWORDS + LINE_GUARD_HALFWORDS)
#define LINE_BUFFER_STRIDE_HALFWORDS   (((LINE_HALFWORDS + 1) + 1) & ~1)
#define LINE_BUFFER_WORDS              (LINE_BUFFER_STRIDE_HALFWORDS / 2)
#define LINE_BUFFER_COUNT              2

#define LINES_PER_FIELD                312
#define VSYNC_EQ_PRE_END               5
#define VSYNC_BROAD_END                10
#define VSYNC_END                      15
#define BORDER_TOP_END                 39
#define ACTIVE_END                     295
#define ACTIVE_LINES                   (ACTIVE_END - BORDER_TOP_END)

#define CYCLES_PER_LINE                3072
#define HSYNC_CYCLES                   226
#define BACKPORCH_CYCLES               274
#define ACTIVE_START_CYCLES            (HSYNC_CYCLES + BACKPORCH_CYCLES + 127)

#define TEXT_ACTIVE_LINES              (TEXT_ROWS * CHAR_HEIGHT)
#define TEXT_TOP_MARGIN                ((ACTIVE_LINES - TEXT_ACTIVE_LINES) / 2)

#define EQ_PULSE_DELAY                 17
#define EQ_HALF_DELAY                  235
#define BROAD_DELAY                    215
#define SERRATION_DELAY                35

#define PIN_SYNC                       4
#define PIN_PIXEL                      6

#define CURSOR_BLINK_MASK              0x10

#define CFGLR_GPIO                     ((uint32_t)0x43B34444)
#define CFGLR_TIM_GPIO                 ((uint32_t)0x43BB4444)
#define CFGLR_TIM_SPI                  ((uint32_t)0x4BBB4444)

typedef union {
    uint16_t halfwords[LINE_BUFFER_STRIDE_HALFWORDS];
    uint32_t words[LINE_BUFFER_WORDS];
    uint8_t bytes[LINE_BUFFER_STRIDE_HALFWORDS * 2];
} dma_line_t;

static dma_line_t line_buffers[LINE_BUFFER_COUNT];
static uint8_t text_vram[TEXT_ROWS][TEXT_COLS] __attribute__((aligned(4)));
static volatile uint16_t current_line;
static uint8_t cursor_x;
static uint8_t cursor_y;
static uint8_t cursor_blink_phase;

static inline void line_clear(dma_line_t *dest)
{
    for (uint8_t i = 0; i < LINE_HALFWORDS; i++) {
        dest->halfwords[i] = 0;
    }
}

void video_textmode_clear(void)
{
    for (uint8_t y = 0; y < TEXT_ROWS; y++) {
        for (uint8_t x = 0; x < TEXT_COLS; x++) {
            text_vram[y][x] = ' ';
        }
    }
}

void video_textmode_clear_row(uint8_t y)
{
    for (uint8_t x = 0; x < TEXT_COLS; x++) {
        text_vram[y][x] = ' ';
    }
}

uint8_t video_textmode_row_is_blank(uint8_t y)
{
    for (uint8_t x = 0; x < TEXT_COLS; x++) {
        if (text_vram[y][x] != ' ') {
            return 0;
        }
    }
    return 1;
}

void video_textmode_scroll_rows(uint8_t top, uint8_t bottom)
{
    for (uint8_t y = top; y < bottom; y++) {
        for (uint8_t x = 0; x < TEXT_COLS; x++) {
            text_vram[y][x] = text_vram[y + 1][x];
        }
    }
    video_textmode_clear_row(bottom);
}

uint8_t video_textmode_read_cell(uint8_t x, uint8_t y)
{
    return text_vram[y][x];
}

void video_textmode_write_cell(uint8_t x, uint8_t y, uint8_t ch)
{
    text_vram[y][x] = ch;
}

void video_textmode_set_cursor(uint8_t x, uint8_t y)
{
    cursor_x = x;
    cursor_y = y;
    cursor_blink_phase = 0;
}

static inline void overlay_cursor(dma_line_t *dest, uint8_t text_row,
                                  uint8_t glyph_row)
{
    if (text_row != cursor_y) {
        return;
    }
    if ((cursor_blink_phase & CURSOR_BLINK_MASK) != 0) {
        return;
    }
    if (glyph_row != (CHAR_HEIGHT - 1)) {
        return;
    }

    {
        uint8_t cell = cursor_x;
        uint8_t halfword_index = (uint8_t)(cell >> 1);

        if (halfword_index >= VISIBLE_HALFWORDS) {
            return;
        }

        if ((cell & 1u) == 0u) {
            dest->halfwords[halfword_index] |= 0xFF00u;
        }
        else {
            dest->halfwords[halfword_index] |= 0x00FFu;
        }
    }
}

static void render_char_row_line(uint16_t *dst, const uint8_t *chars,
                                 uint8_t glyph_row)
__attribute__((section(".srodata"))) __attribute__((noinline));

static void render_char_row_line(uint16_t *dst, const uint8_t *chars,
                                 uint8_t glyph_row)
{
    const uint8_t *font_row = fontdata_rowmajor[glyph_row];
    uint32_t groups = LINE_BUFFER_WORDS - 1;
    uintptr_t dst_ptr = (uintptr_t)dst;
    uintptr_t chars_ptr = (uintptr_t)chars;
    uintptr_t font_ptr = (uintptr_t)font_row;

    __asm__ volatile (
        "1:\n"
        "lbu t0, 0(%[chars])\n"
        "lbu t1, 1(%[chars])\n"
        "lbu a4, 2(%[chars])\n"
        "lbu a5, 3(%[chars])\n"
        "add t0, t0, %[font]\n"
        "add t1, t1, %[font]\n"
        "add a4, a4, %[font]\n"
        "add a5, a5, %[font]\n"
        "lbu t0, 0(t0)\n"
        "lbu t1, 0(t1)\n"
        "lbu a4, 0(a4)\n"
        "lbu a5, 0(a5)\n"
        "slli t0, t0, 8\n"
        "or t0, t0, t1\n"
        "slli a4, a4, 8\n"
        "or a4, a4, a5\n"
        "slli a4, a4, 16\n"
        "or t0, t0, a4\n"
        "sw t0, 0(%[dst])\n"
        "addi %[chars], %[chars], 4\n"
        "addi %[dst], %[dst], 4\n"
        "addi %[groups], %[groups], -1\n"
        "bnez %[groups], 1b\n"
        "sw zero, 0(%[dst])\n"
        : [dst] "+r"(dst_ptr), [chars] "+r"(chars_ptr), [groups] "+r"(groups)
        : [font] "r"(font_ptr)
        : "t0", "t1", "a4", "a5", "memory"
    );
}

static void render_scanline_buffered(dma_line_t *dest, uint8_t active_y)
__attribute__((section(".srodata"))) __attribute__((noinline));

static void render_scanline_buffered(dma_line_t *dest, uint8_t active_y)
{
    if (active_y >= TEXT_TOP_MARGIN
     && active_y < (TEXT_TOP_MARGIN + TEXT_ACTIVE_LINES)) {
        uint8_t text_y = (uint8_t)(active_y - TEXT_TOP_MARGIN);
        uint8_t row = (uint8_t)(text_y >> 3);
        uint8_t glyph_row = (uint8_t)(text_y & 7);
        render_char_row_line(dest->halfwords, text_vram[row], glyph_row);
        overlay_cursor(dest, row, glyph_row);
        return;
    }

    line_clear(dest);
}

static inline void emit_equalizing_line(void)
{
    Delay_Tiny(EQ_PULSE_DELAY);
    GPIOC->BSHR = (1 << PIN_SYNC);
    Delay_Tiny(EQ_HALF_DELAY);
    GPIOC->BCR = (1 << PIN_SYNC);
    Delay_Tiny(EQ_PULSE_DELAY);
    GPIOC->BSHR = (1 << PIN_SYNC);
}

static inline void emit_broad_sync_line(void)
{
    Delay_Tiny(BROAD_DELAY);
    GPIOC->BSHR = (1 << PIN_SYNC);
    Delay_Tiny(SERRATION_DELAY);
    GPIOC->BCR = (1 << PIN_SYNC);
    Delay_Tiny(BROAD_DELAY);
    GPIOC->BSHR = (1 << PIN_SYNC);
}

void TIM1_UP_IRQHandler(void) __attribute__((interrupt));
void TIM1_CC_IRQHandler(void) __attribute__((interrupt));

void TIM1_UP_IRQHandler(void)
{
    uint16_t line = current_line + 1;

    TIM1->INTFR = ~TIM_UIF;

    if (line >= LINES_PER_FIELD) {
        line = 0;
    }
    current_line = line;

    if (line == 0) {
        cursor_blink_phase++;
    }

    if (line >= BORDER_TOP_END && line < ACTIVE_END) {
        TIM1->INTFR = ~TIM_CC3IF;
        TIM1->DMAINTENR |= TIM_CC3IE;
    }
    else {
        TIM1->DMAINTENR &= ~TIM_CC3IE;
        TIM1->INTFR = ~TIM_CC3IF;
    }

    if (line < VSYNC_END) {
        TIM1->CCER &= ~TIM_CC4E;
        GPIOC->CFGLR = CFGLR_GPIO;
        GPIOC->BCR = (1 << PIN_SYNC) | (1 << PIN_PIXEL);

        if (line < VSYNC_EQ_PRE_END) {
            emit_equalizing_line();
        }
        else if (line < VSYNC_BROAD_END) {
            emit_broad_sync_line();
        }
        else {
            emit_equalizing_line();
        }

        if (line == VSYNC_END - 1) {
            GPIOC->CFGLR = CFGLR_TIM_GPIO;
            TIM1->CCER |= TIM_CC4E;
        }
    }
    else {
        TIM1->CCER |= TIM_CC4E;
        GPIOC->BCR = (1 << PIN_PIXEL);
    }

    if (line == VSYNC_END) {
        render_scanline_buffered(&line_buffers[0], 0);
    }
}

void TIM1_CC_IRQHandler(void)
{
    uint16_t active_y = current_line - BORDER_TOP_END;

    TIM1->INTFR = ~TIM_CC3IF;
    GPIOC->CFGLR = CFGLR_TIM_SPI;

    DMA1_Channel3->CFGR &= ~DMA_CFGR1_EN;
    DMA1->INTFCR = DMA1_IT_TC3 | DMA1_IT_GL3;
    DMA1_Channel3->CNTR = LINE_HALFWORDS;
    DMA1_Channel3->MADDR =
        (uint32_t)line_buffers[active_y & (LINE_BUFFER_COUNT - 1)].halfwords;
    DMA1_Channel3->CFGR |= DMA_CFGR1_EN;

    if ((uint16_t)(active_y + 1) < ACTIVE_LINES) {
        render_scanline_buffered(&line_buffers[(active_y + 1) & (LINE_BUFFER_COUNT - 1)],
                                 (uint8_t)(active_y + 1));
    }
}

void video_textmode_init(void)
{
    RCC->AHBPCENR  |= RCC_AHBPeriph_DMA1;
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC | RCC_APB2Periph_SPI1
                    | RCC_APB2Periph_AFIO | RCC_APB2Periph_TIM1;

    GPIOC->CFGLR = CFGLR_TIM_GPIO;
    GPIOC->BCR = (1 << PIN_SYNC) | (1 << PIN_PIXEL);

    SPI1->CTLR1 = SPI_CTLR1_MSTR
                | SPI_CTLR1_SSM
                | SPI_CTLR1_SSI
                | SPI_CTLR1_BR_1
                | SPI_CTLR1_DFF
                | SPI_CTLR1_BIDIMODE
                | SPI_CTLR1_BIDIOE
                | SPI_CTLR1_SPE;
    SPI1->CTLR2 = SPI_CTLR2_TXDMAEN;

    DMA1_Channel3->PADDR = (uint32_t)&SPI1->DATAR;
    DMA1_Channel3->CFGR  = DMA_CFGR1_DIR
                         | DMA_CFGR1_MINC
                         | DMA_CFGR1_PSIZE_0
                         | DMA_CFGR1_MSIZE_0
                         | DMA_CFGR1_PL_1;

    RCC->APB2PRSTR |= RCC_APB2Periph_TIM1;
    RCC->APB2PRSTR &= ~RCC_APB2Periph_TIM1;

    TIM1->PSC = 0;
    TIM1->ATRLR = CYCLES_PER_LINE - 1;
    TIM1->CH3CVR = ACTIVE_START_CYCLES;
    TIM1->CH4CVR = HSYNC_CYCLES;
    TIM1->CHCTLR2 = TIM_OC4PE | TIM_OC4M_2 | TIM_OC4M_1;
    TIM1->CCER = TIM_CC4E | TIM_CC4P;
    TIM1->BDTR = TIM_MOE;
    TIM1->CTLR1 = TIM_ARPE;
    TIM1->SWEVGR = TIM_UG;
    TIM1->INTFR = 0;
    TIM1->DMAINTENR = TIM_UIE;
    TIM1->CTLR1 |= TIM_CEN;

    NVIC_EnableIRQ(TIM1_UP_IRQn);
    NVIC_EnableIRQ(TIM1_CC_IRQn);

    current_line = LINES_PER_FIELD - 1;
}
