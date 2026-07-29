#ifndef LISP_RUNTIME_H
#define LISP_RUNTIME_H
#include <stdint.h>

void lisp_init(void);
void lisp_handle_input_line(const char *line);
uint8_t lisp_is_running(void);
void lisp_handle_run_control_byte(uint8_t ch);
#endif
