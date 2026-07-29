#include "console_textmode.h"

#include "video_textmode.h"
#include <stdint.h>

#define BASIC_INPUT_BYTES (TEXT_COLS + 1)

static uint8_t terminal_cursor_x;
static uint8_t terminal_cursor_y;
static uint8_t terminal_last_was_cr;
static uint8_t terminal_escape_state;
static char submit_buffer[BASIC_INPUT_BYTES];

static uint8_t tiny_strlen(const char *str)
{
    uint8_t len = 0;
    while (str[len]) {
        len++;
    }
    return len;
}

static inline void console_touch_cursor(void)
{
    video_textmode_set_cursor(terminal_cursor_x, terminal_cursor_y);
}

void console_init(void)
{
    terminal_cursor_x = 0;
    terminal_cursor_y = TERM_TOP_ROW;
    terminal_last_was_cr = 0;
    terminal_escape_state = 0;
    console_touch_cursor();
}

void console_clear_screen(void)
{
    video_textmode_clear();
    terminal_cursor_x = 0;
    terminal_cursor_y = TERM_TOP_ROW;
    terminal_last_was_cr = 0;
    terminal_escape_state = 0;
    console_touch_cursor();
}

static void console_scroll_up(void)
{
    video_textmode_scroll_rows(TERM_TOP_ROW, TERM_BOTTOM_ROW);
}

static void console_move_left(void)
{
    if (terminal_cursor_x > 0) {
        terminal_cursor_x--;
        console_touch_cursor();
    }
}

static void console_move_right(void)
{
    if (terminal_cursor_x + 1 < TEXT_COLS) {
        terminal_cursor_x++;
        console_touch_cursor();
    }
}

static void console_move_up(void)
{
    if (terminal_cursor_y > TERM_TOP_ROW) {
        terminal_cursor_y--;
        console_touch_cursor();
    }
}

static void console_move_down(void)
{
    if (terminal_cursor_y < TERM_BOTTOM_ROW) {
        terminal_cursor_y++;
        console_touch_cursor();
    }
}

static void console_output_advance_line(void)
{
    terminal_cursor_x = 0;
    if (terminal_cursor_y < TERM_BOTTOM_ROW) {
        terminal_cursor_y++;
        video_textmode_clear_row(terminal_cursor_y);
    }
    else {
        console_scroll_up();
    }
    console_touch_cursor();
}

static void console_get_row_text(uint8_t row, char *dst, uint8_t dst_size)
{
    uint8_t length = TEXT_COLS;

    while (length > 0 && video_textmode_read_cell((uint8_t)(length - 1), row) == ' ') {
        length--;
    }

    if (length >= dst_size) {
        length = (uint8_t)(dst_size - 1);
    }

    for (uint8_t i = 0; i < length; i++) {
        dst[i] = (char)video_textmode_read_cell(i, row);
    }
    dst[length] = 0;
}

void console_prepare_input_row(void)
{
    if (terminal_cursor_x != 0 || !video_textmode_row_is_blank(terminal_cursor_y)) {
        console_output_advance_line();
    }
    else {
        terminal_cursor_x = 0;
        console_touch_cursor();
    }
}

static void console_backspace(void)
{
    if (terminal_cursor_x > 0) {
        terminal_cursor_x--;
        video_textmode_write_cell(terminal_cursor_x, terminal_cursor_y, ' ');
        console_touch_cursor();
        return;
    }

    if (terminal_cursor_y > TERM_TOP_ROW) {
        terminal_cursor_y--;
        terminal_cursor_x = TEXT_COLS - 1;
        video_textmode_write_cell(terminal_cursor_x, terminal_cursor_y, ' ');
        console_touch_cursor();
    }
}

static void console_put_printable(uint8_t ch)
{
    video_textmode_write_cell(terminal_cursor_x, terminal_cursor_y, ch);
    terminal_cursor_x++;
    if (terminal_cursor_x >= TEXT_COLS) {
        console_output_advance_line();
    }
    else {
        console_touch_cursor();
    }
}

void console_print_char(char ch)
{
    if (ch == '\n') {
        console_output_advance_line();
        return;
    }

    video_textmode_write_cell(terminal_cursor_x, terminal_cursor_y, (uint8_t)ch);
    terminal_cursor_x++;
    if (terminal_cursor_x >= TEXT_COLS) {
        console_output_advance_line();
    }
    else {
        console_touch_cursor();
    }
}

void console_print_string(const char *str)
{
    while (*str) {
        console_print_char(*str++);
    }
}

void console_print_unsigned(uint16_t value)
{
    char temp[5];
    uint8_t count = 0;

    do {
        temp[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u);

    while (count > 0) {
        console_print_char(temp[--count]);
    }
}

void console_print_signed(int16_t value)
{
    int32_t wide = value;

    if (wide < 0) {
        console_print_char('-');
        wide = -wide;
    }

    console_print_unsigned((uint16_t)wide);
}

void console_print_line(const char *str)
{
    console_print_string(str);
    console_output_advance_line();
}

static void console_print_centered_line(const char *str)
{
    uint8_t len = tiny_strlen(str);

    if (len < TEXT_COLS) {
        uint8_t pad = (uint8_t)((TEXT_COLS - len) >> 1);
        while (pad--) {
            console_print_char(' ');
        }
    }

    console_print_line(str);
}

void console_show_boot_banner(void)
{
    console_clear_screen();
    console_print_centered_line("*** CH32 BASIC V.1 32X25 ***");
    console_print_centered_line("TEXT MODE 2048 BASIC BYTES FREE");
    console_print_line("");
    console_print_line("READY.");
    console_prepare_input_row();
}

const char *console_submit_current_row(void)
{
    console_get_row_text(terminal_cursor_y, submit_buffer, BASIC_INPUT_BYTES);
    console_output_advance_line();
    terminal_escape_state = 0;
    console_touch_cursor();
    return submit_buffer;
}

static void console_handle_escape_final(uint8_t ch)
{
    switch (ch) {
    case 'A':
        console_move_up();
        break;
    case 'B':
        console_move_down();
        break;
    case 'C':
        console_move_right();
        break;
    case 'D':
        console_move_left();
        break;
    case 'H':
        terminal_cursor_x = 0;
        console_touch_cursor();
        break;
    case 'F':
        terminal_cursor_x = TEXT_COLS - 1;
        console_touch_cursor();
        break;
    default:
        break;
    }
}

console_event_t console_handle_byte(uint8_t ch)
{
    if (terminal_escape_state == 1) {
        if (ch == '[' || ch == 'O') {
            terminal_escape_state = 2;
            return CONSOLE_EVENT_NONE;
        }
        terminal_escape_state = 0;
    }
    else if (terminal_escape_state == 2) {
        terminal_escape_state = 0;
        console_handle_escape_final(ch);
        return CONSOLE_EVENT_NONE;
    }

    if (ch == 0x1b) {
        terminal_escape_state = 1;
        return CONSOLE_EVENT_NONE;
    }

    if (ch == '\r') {
        terminal_last_was_cr = 1;
        return CONSOLE_EVENT_SUBMIT;
    }

    if (ch == '\n') {
        if (!terminal_last_was_cr) {
            return CONSOLE_EVENT_SUBMIT;
        }
        terminal_last_was_cr = 0;
        return CONSOLE_EVENT_NONE;
    }

    terminal_last_was_cr = 0;

    if (ch == '\b' || ch == 0x7f) {
        console_backspace();
        return CONSOLE_EVENT_NONE;
    }

    if (ch == '\f') {
        console_clear_screen();
        return CONSOLE_EVENT_NONE;
    }

    if (ch == '\t') {
        uint8_t spaces = (uint8_t)(4u - (terminal_cursor_x & 3u));
        while (spaces--) {
            console_put_printable(' ');
        }
        return CONSOLE_EVENT_NONE;
    }

    if (ch >= ' ' && ch <= '~') {
        console_put_printable(ch);
    }

    return CONSOLE_EVENT_NONE;
}
