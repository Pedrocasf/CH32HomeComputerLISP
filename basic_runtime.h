#ifndef BASIC_RUNTIME_H
#define BASIC_RUNTIME_H

#include <stdint.h>

void basic_init(void);
void basic_handle_input_line(const char *line);
uint8_t basic_is_running(void);
void basic_handle_run_control_byte(uint8_t ch);

#endif
