#ifndef CONSOLE_TEXTMODE_H
#define CONSOLE_TEXTMODE_H

#include <stdint.h>

typedef enum {
    CONSOLE_EVENT_NONE = 0,
    CONSOLE_EVENT_SUBMIT,
} console_event_t;

void console_init(void);
void console_show_boot_banner(void);
console_event_t console_handle_byte(uint8_t ch);
const char *console_submit_current_row(void);
void console_prepare_input_row(void);
void console_clear_screen(void);

void console_print_char(char ch);
void console_print_string(const char *str);
void console_print_unsigned(uint16_t value);
void console_print_signed(int16_t value);
void console_print_line(const char *str);

#endif
