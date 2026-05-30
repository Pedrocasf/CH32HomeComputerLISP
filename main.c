#include "lisp_runtime.h"
#include "console_textmode.h"
#include "video_textmode.h"

#include "ch32fun.h"
#include <stdint.h>

void handle_debug_input(int numbytes, uint8_t *data)
{
    for (int i = 0; i < numbytes; i++) {
        if (lisp_is_running()) {
            lisp_handle_run_control_byte(data[i]);
            continue;
        }

        if (console_handle_byte(data[i]) == CONSOLE_EVENT_SUBMIT) {
            const char *line = console_submit_current_row();

            lisp_handle_input_line(line);
            console_prepare_input_row();
        }
    }
}

int main(void)
{
    SystemInit();
    lisp_init();
    console_init();
    console_show_boot_banner();
    video_textmode_init();

    while (1) {
        poll_input();
        __asm__ volatile ("nop");
    }
}
