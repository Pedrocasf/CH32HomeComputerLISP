#include "console_textmode.h"
#include "video_textmode.h"

#include "ch32fun.h"
#include <stdint.h>

/* Build either language runtime: -DUSE_LISP=1 (default, see Makefile) or
 * -DUSE_LISP=0 for the original BASIC. Only one is referenced, so
 * --gc-sections drops the other entirely. */
#ifndef USE_LISP
#define USE_LISP 1
#endif

#if USE_LISP
#include "lisp.h"
#else
#include "basic_runtime.h"
#endif

void handle_debug_input(int numbytes, uint8_t *data)
{
    for (int i = 0; i < numbytes; i++) {
#if USE_LISP
        /* Ctrl-R runs the whole screen as a program. Intercepted here so
         * console_textmode stays language-agnostic. */
        if (data[i] == 0x12) {
            lisp_handle_screen();
            continue;
        }
#else
        if (basic_is_running()) {
            basic_handle_run_control_byte(data[i]);
            continue;
        }
#endif

        if (console_handle_byte(data[i]) == CONSOLE_EVENT_SUBMIT) {
            const char *line = console_submit_current_row();
#if USE_LISP
            lisp_handle_input_line(line);
#else
            basic_handle_input_line(line);
#endif
            console_prepare_input_row();
        }
    }
}

int main(void)
{
    SystemInit();
    console_init();

#if USE_LISP
    /* The collector scans the C stack for roots, so it needs to know where
     * that stack ends. ch32fun's linker script puts the top at _eusrstack. */
    {
        extern uint32_t _eusrstack;
        lisp_set_stack_top(&_eusrstack);
    }

    lisp_init();
    console_clear_screen();
    console_print_line("  *** CH32 LISP V.1 32X25 ***");
    console_print_line("");
    console_print_line("READY.");
    console_prepare_input_row();
#else
    basic_init();
    console_show_boot_banner();
#endif

    video_textmode_init();

    while (1) {
        poll_input();
        __asm__ volatile ("nop");
    }
}
