#include "lisp_runtime.h"
typedef enum {
    LISP_INPUT_NONE = 0,
    LISP_INPUT_NUMERIC,
    LISP_INPUT_STRING,
} lisp_input_mode_t;
static lisp_input_mode_t lisp_input_mode;
static uint8_t lisp_break_escape_state;
static uint8_t lisp_program_running;
static uint8_t lisp_break_reqiuested;
static char* l;
static char* p;
uint16_t ram[1024];
#define sNil  "NIL"
#define sT  "T"
#define sDefine "DEFINE"
#define sCond  "COND"
#define sQuote  "QUOTE"
#define sCAR "CAR"
#define sCDR  "CDR"
#define sCons  "CONS"
#define sEQ  "EQ"
#define sAtom  "ATOM"
const char* S[] = {
    sNil,
    sT,
    sDefine,
    sCond,
    sQuote,
    sCAR,
    sCDR,
    sCons,
    sEQ,
    sAtom,
};
char get_char(){

}
int get_token(){
    int c, i=0;
    do if ((c=get_char()) > ' ')
}

void read(){
    return get_object(get_token());
}

void lisp_init(void){

}
uint8_t lisp_is_running(void)
{
    return lisp_program_running;
}
void lisp_handle_input_line(const char *line){

}
uint8_t lisp_is_running(void);
void lisp_handle_run_control_byte(uint8_t ch){
    if(!lisp_program_running){
        return;
    }
    if(lisp_input_mode != LISP_INPUT_NONE){
        if(ch == 0x1B){
            lisp_program_running = 0;
            lisp_break_reqiuested = 0;
            lisp_break_escape_state = 0;
        }
    }
}
