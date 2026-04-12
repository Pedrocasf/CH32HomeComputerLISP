#ifndef VIDEO_TEXTMODE_H
#define VIDEO_TEXTMODE_H

#include <stdint.h>

#define TEXT_COLS       32
#define TEXT_ROWS       25
#define TERM_TOP_ROW    0
#define TERM_BOTTOM_ROW 24

void video_textmode_init(void);
void video_textmode_clear(void);
void video_textmode_clear_row(uint8_t y);
uint8_t video_textmode_row_is_blank(uint8_t y);
void video_textmode_scroll_rows(uint8_t top, uint8_t bottom);
uint8_t video_textmode_read_cell(uint8_t x, uint8_t y);
void video_textmode_write_cell(uint8_t x, uint8_t y, uint8_t ch);
void video_textmode_set_cursor(uint8_t x, uint8_t y);

#endif
