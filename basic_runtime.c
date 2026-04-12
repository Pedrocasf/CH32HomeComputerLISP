#include "basic_runtime.h"

#include "console_textmode.h"
#include "ch32fun.h"
#include <stdint.h>

#define BASIC_HEAP_BYTES        640
#define BASIC_FOR_STACK_DEPTH   4
#define BASIC_GOSUB_STACK_DEPTH 4
#define BASIC_NO_OFFSET         0xFFFFu
#define BASIC_VAR_RECORD_BYTES  4
#define BASIC_VAR_STRING_FLAG   0x80u

typedef enum {
    BASIC_STATUS_OK = 0,
    BASIC_STATUS_INPUT_WAIT,
    BASIC_STATUS_SYNTAX,
    BASIC_STATUS_OUT_OF_MEMORY,
    BASIC_STATUS_LINE_NOT_FOUND,
    BASIC_STATUS_DIV_ZERO,
    BASIC_STATUS_EMPTY_PROGRAM,
    BASIC_STATUS_BREAK,
    BASIC_STATUS_NEXT_WITHOUT_FOR,
    BASIC_STATUS_FOR_STACK_FULL,
    BASIC_STATUS_RETURN_WITHOUT_GOSUB,
    BASIC_STATUS_GOSUB_STACK_FULL,
} basic_status_t;

typedef struct {
    const char *ptr;
    basic_status_t status;
} basic_parser_t;

typedef struct {
    uint8_t variable;
    int16_t limit;
    int16_t step;
    uint16_t resume_offset;
} basic_for_frame_t;

typedef enum {
    BASIC_INPUT_NONE = 0,
    BASIC_INPUT_NUMERIC,
    BASIC_INPUT_STRING,
} basic_input_mode_t;

static uint8_t basic_heap[BASIC_HEAP_BYTES];
static uint16_t basic_program_end;
static uint16_t basic_vars_end;
static uint16_t basic_string_top;
static basic_for_frame_t basic_for_stack[BASIC_FOR_STACK_DEPTH];
static uint8_t basic_for_stack_depth;
static uint16_t basic_gosub_stack[BASIC_GOSUB_STACK_DEPTH];
static uint8_t basic_gosub_stack_depth;
static uint8_t basic_program_running;
static uint8_t basic_break_requested;
static uint8_t basic_break_escape_state;
static basic_input_mode_t basic_input_mode;
static uint8_t basic_input_variable;
static uint16_t basic_input_resume_offset;
static uint16_t basic_random_state;

static void basic_reset_for_stack(void);
static void basic_reset_gosub_stack(void);
static void basic_reset_input_state(void);
static void parser_skip_spaces(basic_parser_t *parser);
static uint8_t parser_match_keyword(basic_parser_t *parser, const char *keyword);
static uint16_t basic_record_size(uint16_t offset);
static char *basic_record_text(uint16_t offset);
static uint16_t basic_find_line_offset(uint16_t line_number, uint8_t *found);
static basic_status_t basic_parse_string_expression(basic_parser_t *parser,
                                                   const char **value,
                                                   uint8_t *length,
                                                   char *scratch);
static basic_status_t basic_resume_program(uint16_t offset);
static basic_status_t basic_execute_statement(const char *text, uint8_t from_program,
                                              uint16_t current_offset,
                                              uint16_t next_offset,
                                              uint16_t *jump_line,
                                              uint16_t *jump_offset,
                                              uint8_t *did_end);

static inline char ascii_upper(char ch)
{
    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - ('a' - 'A'));
    }
    return ch;
}

static inline uint8_t is_ascii_digit(char ch)
{
    return (ch >= '0' && ch <= '9');
}

static inline uint8_t is_ascii_alpha(char ch)
{
    ch = ascii_upper(ch);
    return (ch >= 'A' && ch <= 'Z');
}

static inline uint8_t is_ascii_alnum(char ch)
{
    return (uint8_t)(is_ascii_alpha(ch) || is_ascii_digit(ch));
}

static void tiny_memmove_forward(uint8_t *dst, const uint8_t *src, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        dst[i] = src[i];
    }
}

static void tiny_memmove_backward(uint8_t *dst, const uint8_t *src, uint16_t len)
{
    while (len--) {
        dst[len] = src[len];
    }
}

static uint8_t tiny_strlen(const char *str)
{
    uint8_t len = 0;
    while (str[len]) {
        len++;
    }
    return len;
}

static const char *basic_status_string(basic_status_t status)
{
    switch (status) {
    case BASIC_STATUS_INPUT_WAIT:
        return "OK";
    case BASIC_STATUS_SYNTAX:
        return "SYNTAX ERROR";
    case BASIC_STATUS_OUT_OF_MEMORY:
        return "OUT OF MEMORY";
    case BASIC_STATUS_LINE_NOT_FOUND:
        return "LINE NOT FOUND";
    case BASIC_STATUS_DIV_ZERO:
        return "DIV BY ZERO";
    case BASIC_STATUS_EMPTY_PROGRAM:
        return "EMPTY PROGRAM";
    case BASIC_STATUS_BREAK:
        return "BREAK";
    case BASIC_STATUS_NEXT_WITHOUT_FOR:
        return "NEXT WITHOUT FOR";
    case BASIC_STATUS_FOR_STACK_FULL:
        return "FOR STACK FULL";
    case BASIC_STATUS_RETURN_WITHOUT_GOSUB:
        return "RETURN WITHOUT GOSUB";
    case BASIC_STATUS_GOSUB_STACK_FULL:
        return "GOSUB STACK FULL";
    default:
        return "OK";
    }
}

static void basic_report_status(basic_status_t status)
{
    if (status != BASIC_STATUS_OK && status != BASIC_STATUS_INPUT_WAIT) {
        console_print_line(basic_status_string(status));
    }
}

static void basic_reset_variables(void)
{
    basic_vars_end = basic_program_end;
    basic_string_top = BASIC_HEAP_BYTES;
    basic_reset_for_stack();
    basic_reset_gosub_stack();
    basic_reset_input_state();
}

static void basic_reset_for_stack(void)
{
    basic_for_stack_depth = 0;
}

static void basic_reset_gosub_stack(void)
{
    basic_gosub_stack_depth = 0;
}

static void basic_reset_input_state(void)
{
    basic_input_mode = BASIC_INPUT_NONE;
    basic_input_variable = 0;
    basic_input_resume_offset = BASIC_NO_OFFSET;
}

static void basic_reset_program(void)
{
    basic_program_end = 0;
    basic_reset_variables();
}

void basic_init(void)
{
    basic_reset_program();
    basic_program_running = 0;
    basic_break_requested = 0;
    basic_break_escape_state = 0;
    basic_random_state = 0xACE1u;
}

uint8_t basic_is_running(void)
{
    return basic_program_running;
}

static uint16_t basic_record_line(uint16_t offset)
{
    return (uint16_t)basic_heap[offset]
         | ((uint16_t)basic_heap[offset + 1] << 8);
}

static uint8_t basic_record_length(uint16_t offset)
{
    return basic_heap[offset + 2];
}

static uint16_t basic_record_size(uint16_t offset)
{
    return (uint16_t)(4u + basic_record_length(offset));
}

static char *basic_record_text(uint16_t offset)
{
    return (char *)&basic_heap[offset + 3];
}

static uint16_t basic_find_line_offset(uint16_t line_number, uint8_t *found)
{
    uint16_t offset = 0;

    *found = 0;
    while (offset < basic_program_end) {
        uint16_t current = basic_record_line(offset);
        if (current >= line_number) {
            if (current == line_number) {
                *found = 1;
            }
            return offset;
        }
        offset += basic_record_size(offset);
    }

    return offset;
}

static void basic_delete_record_at(uint16_t offset)
{
    uint16_t size = basic_record_size(offset);
    uint16_t tail = (uint16_t)(basic_program_end - (offset + size));

    if (tail > 0) {
        tiny_memmove_forward(&basic_heap[offset],
                             &basic_heap[offset + size], tail);
    }
    basic_program_end -= size;
    basic_reset_variables();
}

static basic_status_t basic_store_line(uint16_t line_number, const char *text)
{
    uint8_t found;
    uint16_t offset = basic_find_line_offset(line_number, &found);
    uint8_t length = tiny_strlen(text);
    uint16_t record_size = (uint16_t)(4u + length);

    basic_reset_variables();
    offset = basic_find_line_offset(line_number, &found);

    if (found) {
        basic_delete_record_at(offset);
        offset = basic_find_line_offset(line_number, &found);
    }

    if ((uint16_t)(basic_program_end + record_size) > basic_string_top) {
        return BASIC_STATUS_OUT_OF_MEMORY;
    }

    {
        uint16_t tail = (uint16_t)(basic_program_end - offset);
        if (tail > 0) {
            tiny_memmove_backward(&basic_heap[offset + record_size],
                                  &basic_heap[offset], tail);
        }
    }

    basic_heap[offset] = (uint8_t)(line_number & 0xFFu);
    basic_heap[offset + 1] = (uint8_t)(line_number >> 8);
    basic_heap[offset + 2] = length;
    for (uint8_t i = 0; i < length; i++) {
        basic_heap[offset + 3 + i] = (uint8_t)text[i];
    }
    basic_heap[offset + 3 + length] = 0;
    basic_program_end += record_size;
    basic_reset_variables();
    return BASIC_STATUS_OK;
}

static void basic_delete_line(uint16_t line_number)
{
    uint8_t found;

    basic_reset_variables();
    {
        uint16_t offset = basic_find_line_offset(line_number, &found);

        if (found) {
            basic_delete_record_at(offset);
        }
    }
}

static uint8_t basic_var_numeric_tag(uint8_t variable_name)
{
    return variable_name;
}

static uint8_t basic_var_string_tag(uint8_t variable_name)
{
    return (uint8_t)(BASIC_VAR_STRING_FLAG | variable_name);
}

static uint16_t basic_find_var_record(uint8_t tag)
{
    uint16_t offset = basic_program_end;

    while (offset < basic_vars_end) {
        if (basic_heap[offset] == tag) {
            return offset;
        }
        offset += BASIC_VAR_RECORD_BYTES;
    }

    return BASIC_NO_OFFSET;
}

static uint16_t basic_find_highest_string_record_below(uint16_t ceiling)
{
    uint16_t best_offset = BASIC_NO_OFFSET;
    uint16_t best_ptr = 0;
    uint16_t offset = basic_program_end;

    while (offset < basic_vars_end) {
        if ((basic_heap[offset] & BASIC_VAR_STRING_FLAG) != 0u) {
            uint8_t length = basic_heap[offset + 1];
            uint16_t ptr = (uint16_t)basic_heap[offset + 2]
                         | ((uint16_t)basic_heap[offset + 3] << 8);

            if (length > 0 && ptr < ceiling && ptr >= best_ptr) {
                best_ptr = ptr;
                best_offset = offset;
            }
        }
        offset += BASIC_VAR_RECORD_BYTES;
    }

    return best_offset;
}

static void basic_collect_strings(void)
{
    uint16_t new_top = BASIC_HEAP_BYTES;
    uint16_t ceiling = BASIC_HEAP_BYTES;

    while (1) {
        uint16_t record_offset = basic_find_highest_string_record_below(ceiling);

        if (record_offset == BASIC_NO_OFFSET) {
            break;
        }

        {
            uint8_t length = basic_heap[record_offset + 1];
            uint16_t old_ptr = (uint16_t)basic_heap[record_offset + 2]
                             | ((uint16_t)basic_heap[record_offset + 3] << 8);
            uint16_t bytes = (uint16_t)(length + 1u);

            new_top = (uint16_t)(new_top - bytes);
            if (old_ptr != new_top) {
                tiny_memmove_forward(&basic_heap[new_top], &basic_heap[old_ptr], bytes);
            }
            basic_heap[record_offset + 2] = (uint8_t)(new_top & 0xFFu);
            basic_heap[record_offset + 3] = (uint8_t)(new_top >> 8);
            ceiling = old_ptr;
        }
    }

    basic_string_top = new_top;
}

static uint8_t basic_ensure_heap_space(uint16_t bytes)
{
    if ((uint16_t)(basic_vars_end + bytes) <= basic_string_top) {
        return 1;
    }

    basic_collect_strings();
    return (uint8_t)((uint16_t)(basic_vars_end + bytes) <= basic_string_top);
}

static uint16_t basic_claim_var_record(uint8_t tag, uint8_t *created)
{
    uint16_t offset = basic_find_var_record(tag);

    if (offset != BASIC_NO_OFFSET) {
        *created = 0;
        return offset;
    }
    if (!basic_ensure_heap_space(BASIC_VAR_RECORD_BYTES)) {
        *created = 0;
        return BASIC_NO_OFFSET;
    }

    offset = basic_vars_end;
    basic_vars_end = (uint16_t)(basic_vars_end + BASIC_VAR_RECORD_BYTES);
    basic_heap[offset] = tag;
    basic_heap[offset + 1] = 0;
    basic_heap[offset + 2] = 0;
    basic_heap[offset + 3] = 0;
    *created = 1;
    return offset;
}

static int16_t basic_get_numeric_variable(uint8_t variable_name)
{
    uint16_t offset = basic_find_var_record(basic_var_numeric_tag(variable_name));

    if (offset == BASIC_NO_OFFSET) {
        return 0;
    }
    return (int16_t)((uint16_t)basic_heap[offset + 1]
                   | ((uint16_t)basic_heap[offset + 2] << 8));
}

static basic_status_t basic_set_numeric_variable(uint8_t variable_name, int16_t value)
{
    uint8_t created;
    uint16_t offset = basic_claim_var_record(basic_var_numeric_tag(variable_name), &created);

    if (offset == BASIC_NO_OFFSET) {
        return BASIC_STATUS_OUT_OF_MEMORY;
    }

    basic_heap[offset + 1] = (uint8_t)(value & 0xFFu);
    basic_heap[offset + 2] = (uint8_t)(((uint16_t)value >> 8) & 0xFFu);
    return BASIC_STATUS_OK;
}

static uint8_t basic_get_string_length(uint8_t variable_name)
{
    uint16_t offset = basic_find_var_record(basic_var_string_tag(variable_name));

    if (offset == BASIC_NO_OFFSET) {
        return 0;
    }
    return basic_heap[offset + 1];
}

static const char *basic_get_string_value(uint8_t variable_name)
{
    uint16_t offset = basic_find_var_record(basic_var_string_tag(variable_name));

    if (offset == BASIC_NO_OFFSET || basic_heap[offset + 1] == 0) {
        return "";
    }

    return (const char *)&basic_heap[(uint16_t)basic_heap[offset + 2]
                                   | ((uint16_t)basic_heap[offset + 3] << 8)];
}

static basic_status_t basic_assign_string_buffer(uint8_t variable_name,
                                                 const char *src,
                                                 uint8_t length)
{
    uint8_t created;
    uint16_t record_offset = basic_claim_var_record(basic_var_string_tag(variable_name), &created);
    uint16_t bytes = (uint16_t)(length + 1u);
    uint16_t string_offset;

    if (record_offset == BASIC_NO_OFFSET) {
        return BASIC_STATUS_OUT_OF_MEMORY;
    }

    if (bytes > 0) {
        if (!basic_ensure_heap_space(bytes)) {
            return BASIC_STATUS_OUT_OF_MEMORY;
        }
        basic_string_top = (uint16_t)(basic_string_top - bytes);
        string_offset = basic_string_top;
        for (uint8_t i = 0; i < length; i++) {
            basic_heap[string_offset + i] = (uint8_t)src[i];
        }
        basic_heap[string_offset + length] = 0;
    }
    else {
        string_offset = basic_string_top;
    }

    basic_heap[record_offset + 1] = length;
    basic_heap[record_offset + 2] = (uint8_t)(string_offset & 0xFFu);
    basic_heap[record_offset + 3] = (uint8_t)(string_offset >> 8);
    return BASIC_STATUS_OK;
}

static uint8_t basic_for_condition_holds(int16_t value, int16_t limit, int16_t step)
{
    if (step >= 0) {
        return (uint8_t)(value <= limit);
    }
    return (uint8_t)(value >= limit);
}

static uint16_t basic_find_matching_next_resume(uint16_t for_offset, uint8_t *found)
{
    uint16_t offset = (uint16_t)(for_offset + basic_record_size(for_offset));
    uint8_t depth = 0;

    *found = 0;

    while (offset < basic_program_end) {
        basic_parser_t parser;

        parser.ptr = basic_record_text(offset);
        parser.status = BASIC_STATUS_OK;
        parser_skip_spaces(&parser);

        if (parser_match_keyword(&parser, "FOR")) {
            depth++;
        }
        else if (parser_match_keyword(&parser, "NEXT")) {
            if (depth == 0) {
                *found = 1;
                return (uint16_t)(offset + basic_record_size(offset));
            }
            depth--;
        }

        offset += basic_record_size(offset);
    }

    return BASIC_NO_OFFSET;
}

static void parser_skip_spaces(basic_parser_t *parser)
{
    while (*parser->ptr == ' ' || *parser->ptr == '\t') {
        parser->ptr++;
    }
}

static uint8_t parser_match_char(basic_parser_t *parser, char ch)
{
    parser_skip_spaces(parser);
    if (*parser->ptr == ch) {
        parser->ptr++;
        return 1;
    }
    return 0;
}

static uint8_t parser_match_keyword(basic_parser_t *parser, const char *keyword)
{
    const char *scan;
    uint8_t length = 0;

    parser_skip_spaces(parser);
    scan = parser->ptr;

    while (keyword[length]) {
        if (ascii_upper(scan[length]) != keyword[length]) {
            return 0;
        }
        length++;
    }

    if (is_ascii_alnum(scan[length])) {
        return 0;
    }

    parser->ptr = scan + length;
    return 1;
}

static uint8_t parser_parse_numeric_variable(basic_parser_t *parser,
                                             uint8_t *variable_index)
{
    parser_skip_spaces(parser);
    if (!is_ascii_alpha(*parser->ptr)) {
        return 0;
    }

    *variable_index = (uint8_t)(ascii_upper(*parser->ptr++) - 'A');
    if (*parser->ptr == '$' || is_ascii_alnum(*parser->ptr)) {
        parser->status = BASIC_STATUS_SYNTAX;
        return 0;
    }
    return 1;
}

static uint8_t parser_parse_string_variable(basic_parser_t *parser,
                                            uint8_t *variable_index)
{
    const char *scan;

    parser_skip_spaces(parser);
    scan = parser->ptr;

    if (!is_ascii_alpha(*scan) || scan[1] != '$') {
        return 0;
    }

    if (is_ascii_alnum(scan[2])) {
        parser->status = BASIC_STATUS_SYNTAX;
        return 0;
    }

    *variable_index = (uint8_t)(ascii_upper(*scan) - 'A');
    parser->ptr = scan + 2;
    return 1;
}

static uint8_t parser_parse_line_number(const char **text, uint16_t *line_number)
{
    const char *scan = *text;
    uint32_t value = 0;
    uint8_t have_digits = 0;

    while (*scan == ' ' || *scan == '\t') {
        scan++;
    }

    while (is_ascii_digit(*scan)) {
        have_digits = 1;
        value = (value * 10u) + (uint32_t)(*scan - '0');
        if (value > 65535u) {
            return 0;
        }
        scan++;
    }

    if (!have_digits || value == 0u) {
        return 0;
    }

    *line_number = (uint16_t)value;
    *text = scan;
    return 1;
}

static int16_t basic_parse_expression(basic_parser_t *parser);

static uint16_t basic_random_next(void)
{
    basic_random_state = (uint16_t)(basic_random_state * 25173u + 13849u);
    return basic_random_state;
}

static basic_status_t basic_parse_string_expression(basic_parser_t *parser,
                                                   const char **value,
                                                   uint8_t *length,
                                                   char *scratch)
{
    uint8_t string_index;

    parser_skip_spaces(parser);

    if (*parser->ptr == '"') {
        const char *literal_start;
        uint8_t literal_length = 0;

        parser->ptr++;
        literal_start = parser->ptr;
        while (*parser->ptr && *parser->ptr != '"') {
            literal_length++;
            parser->ptr++;
        }
        if (*parser->ptr != '"') {
            parser->status = BASIC_STATUS_SYNTAX;
            return parser->status;
        }
        parser->ptr++;
        *value = literal_start;
        *length = literal_length;
        return BASIC_STATUS_OK;
    }

    if (parser_parse_string_variable(parser, &string_index)) {
        *value = basic_get_string_value(string_index);
        *length = basic_get_string_length(string_index);
        return BASIC_STATUS_OK;
    }
    if (parser->status != BASIC_STATUS_OK) {
        return parser->status;
    }

    if (parser_match_keyword(parser, "CHR$")) {
        int16_t codepoint;

        if (!parser_match_char(parser, '(')) {
            parser->status = BASIC_STATUS_SYNTAX;
            return parser->status;
        }
        codepoint = basic_parse_expression(parser);
        if (parser->status != BASIC_STATUS_OK) {
            return parser->status;
        }
        if (!parser_match_char(parser, ')')) {
            parser->status = BASIC_STATUS_SYNTAX;
            return parser->status;
        }

        scratch[0] = (char)(codepoint & 0xFF);
        scratch[1] = 0;
        *value = scratch;
        *length = 1;
        return BASIC_STATUS_OK;
    }

    parser->status = BASIC_STATUS_SYNTAX;
    return parser->status;
}

static int16_t basic_parse_factor(basic_parser_t *parser)
{
    int16_t value = 0;

    parser_skip_spaces(parser);
    if (parser->status != BASIC_STATUS_OK) {
        return 0;
    }

    if (*parser->ptr == '-') {
        parser->ptr++;
        return (int16_t)(-basic_parse_factor(parser));
    }

    if (*parser->ptr == '(') {
        parser->ptr++;
        value = basic_parse_expression(parser);
        if (!parser_match_char(parser, ')')) {
            parser->status = BASIC_STATUS_SYNTAX;
        }
        return value;
    }

    if (is_ascii_digit(*parser->ptr)) {
        uint32_t number = 0;
        while (is_ascii_digit(*parser->ptr)) {
            number = (number * 10u) + (uint32_t)(*parser->ptr - '0');
            parser->ptr++;
        }
        return (int16_t)number;
    }

    if (parser_match_keyword(parser, "RND")) {
        int16_t argument = 1;
        uint16_t random_value;

        if (parser_match_char(parser, '(')) {
            argument = basic_parse_expression(parser);
            if (parser->status != BASIC_STATUS_OK) {
                return 0;
            }
            if (!parser_match_char(parser, ')')) {
                parser->status = BASIC_STATUS_SYNTAX;
                return 0;
            }
        }

        if (argument < 0) {
            basic_random_state = (uint16_t)(0u - (uint16_t)argument);
            return 0;
        }

        random_value = (uint16_t)(basic_random_next() & 0x7FFFu);
        if (argument == 0) {
            return (int16_t)random_value;
        }
        return (int16_t)(1 + (random_value % (uint16_t)argument));
    }

    if (parser_match_keyword(parser, "LEN")) {
        const char *string_value;
        uint8_t string_length;
        char scratch[2];

        if (!parser_match_char(parser, '(')) {
            parser->status = BASIC_STATUS_SYNTAX;
            return 0;
        }
        if (basic_parse_string_expression(parser, &string_value, &string_length, scratch)
            != BASIC_STATUS_OK) {
            return 0;
        }
        if (!parser_match_char(parser, ')')) {
            parser->status = BASIC_STATUS_SYNTAX;
            return 0;
        }
        return string_length;
    }

    if (parser_match_keyword(parser, "ASC")) {
        const char *string_value;
        uint8_t string_length;
        char scratch[2];

        if (!parser_match_char(parser, '(')) {
            parser->status = BASIC_STATUS_SYNTAX;
            return 0;
        }
        if (basic_parse_string_expression(parser, &string_value, &string_length, scratch)
            != BASIC_STATUS_OK) {
            return 0;
        }
        if (!parser_match_char(parser, ')')) {
            parser->status = BASIC_STATUS_SYNTAX;
            return 0;
        }
        if (string_length == 0) {
            return 0;
        }
        return (uint8_t)string_value[0];
    }

    if (is_ascii_alpha(*parser->ptr)) {
        char name = ascii_upper(*parser->ptr++);
        if (*parser->ptr == '$' || is_ascii_alnum(*parser->ptr)) {
            parser->status = BASIC_STATUS_SYNTAX;
            return 0;
        }
        return basic_get_numeric_variable((uint8_t)(name - 'A'));
    }

    parser->status = BASIC_STATUS_SYNTAX;
    return 0;
}

static int16_t basic_parse_term(basic_parser_t *parser)
{
    int16_t value = basic_parse_factor(parser);

    while (parser->status == BASIC_STATUS_OK) {
        int16_t rhs;
        char op;

        parser_skip_spaces(parser);
        op = *parser->ptr;
        if (op != '*' && op != '/') {
            break;
        }
        parser->ptr++;
        rhs = basic_parse_factor(parser);
        if (parser->status != BASIC_STATUS_OK) {
            return value;
        }

        if (op == '*') {
            value = (int16_t)((int32_t)value * (int32_t)rhs);
        }
        else {
            if (rhs == 0) {
                parser->status = BASIC_STATUS_DIV_ZERO;
                return 0;
            }
            value = (int16_t)(value / rhs);
        }
    }

    return value;
}

static int16_t basic_parse_expression(basic_parser_t *parser)
{
    int16_t value = basic_parse_term(parser);

    while (parser->status == BASIC_STATUS_OK) {
        int16_t rhs;
        char op;

        parser_skip_spaces(parser);
        op = *parser->ptr;
        if (op != '+' && op != '-') {
            break;
        }
        parser->ptr++;
        rhs = basic_parse_term(parser);
        if (parser->status != BASIC_STATUS_OK) {
            return value;
        }

        if (op == '+') {
            value = (int16_t)(value + rhs);
        }
        else {
            value = (int16_t)(value - rhs);
        }
    }

    return value;
}

static basic_status_t basic_parse_assignment(const char *text)
{
    basic_parser_t parser;
    uint8_t variable_index;

    parser.ptr = text;
    parser.status = BASIC_STATUS_OK;

    parser_skip_spaces(&parser);
    if (parser_match_keyword(&parser, "LET")) {
        parser_skip_spaces(&parser);
    }

    if (parser_parse_string_variable(&parser, &variable_index)) {
        basic_status_t status;
        const char *string_value;
        uint8_t string_length;
        char scratch[2];

        parser_skip_spaces(&parser);
        if (!parser_match_char(&parser, '=')) {
            return BASIC_STATUS_SYNTAX;
        }
        status = basic_parse_string_expression(&parser, &string_value, &string_length, scratch);
        if (status != BASIC_STATUS_OK) {
            return status;
        }

        status = basic_assign_string_buffer(variable_index, string_value, string_length);
        if (status != BASIC_STATUS_OK) {
            return status;
        }

        parser_skip_spaces(&parser);
        if (*parser.ptr != 0) {
            return BASIC_STATUS_SYNTAX;
        }
        return BASIC_STATUS_OK;
    }

    if (!parser_parse_numeric_variable(&parser, &variable_index)) {
        return BASIC_STATUS_SYNTAX;
    }

    if (!parser_match_char(&parser, '=')) {
        return BASIC_STATUS_SYNTAX;
    }

    {
        int16_t value = basic_parse_expression(&parser);
        if (parser.status != BASIC_STATUS_OK) {
            return parser.status;
        }

        parser_skip_spaces(&parser);
        if (*parser.ptr != 0) {
            return BASIC_STATUS_SYNTAX;
        }

        return basic_set_numeric_variable(variable_index, value);
    }
}

static basic_status_t basic_execute_print(basic_parser_t *parser)
{
    parser_skip_spaces(parser);

    while (*parser->ptr != 0) {
        const char *string_value;
        uint8_t string_length;
        char scratch[2];

        if (*parser->ptr == '"' ||
            (is_ascii_alpha(*parser->ptr) && parser->ptr[1] == '$') ||
            (ascii_upper(parser->ptr[0]) == 'C' &&
             ascii_upper(parser->ptr[1]) == 'H' &&
             ascii_upper(parser->ptr[2]) == 'R' &&
             parser->ptr[3] == '$')) {
            if (basic_parse_string_expression(parser, &string_value, &string_length, scratch)
                != BASIC_STATUS_OK) {
                return parser->status;
            }
            for (uint8_t i = 0; i < string_length; i++) {
                console_print_char(string_value[i]);
            }
        }
        else {
            int16_t value = basic_parse_expression(parser);
            if (parser->status != BASIC_STATUS_OK) {
                return parser->status;
            }
            console_print_signed(value);
        }

        parser_skip_spaces(parser);
        if (*parser->ptr == ';' || *parser->ptr == ',') {
            char separator = *parser->ptr++;
            parser_skip_spaces(parser);
            if (*parser->ptr == 0) {
                return BASIC_STATUS_OK;
            }
            if (separator == ',') {
                console_print_char(' ');
            }
            continue;
        }
        if (*parser->ptr != 0) {
            return BASIC_STATUS_SYNTAX;
        }
    }

    console_print_line("");
    return BASIC_STATUS_OK;
}

static basic_status_t basic_begin_input(basic_input_mode_t mode,
                                        uint8_t variable_index,
                                        uint16_t resume_offset)
{
    basic_program_running = 1;
    basic_break_requested = 0;
    basic_input_mode = mode;
    basic_input_variable = variable_index;
    basic_input_resume_offset = resume_offset;
    basic_break_escape_state = 0;
    console_prepare_input_row();
    return BASIC_STATUS_INPUT_WAIT;
}

static basic_status_t basic_resume_program(uint16_t offset)
{
    while (offset < basic_program_end) {
        uint16_t jump_line;
        uint16_t jump_offset;
        uint16_t next_offset = (uint16_t)(offset + basic_record_size(offset));
        uint8_t did_end;
        basic_status_t status =
            basic_execute_statement(basic_record_text(offset), 1, offset, next_offset,
                                    &jump_line, &jump_offset, &did_end);

        poll_input();
        if (basic_break_requested) {
            basic_program_running = 0;
            basic_break_requested = 0;
            basic_break_escape_state = 0;
            basic_reset_input_state();
            return BASIC_STATUS_BREAK;
        }
        if (status == BASIC_STATUS_INPUT_WAIT) {
            return status;
        }
        if (status != BASIC_STATUS_OK) {
            basic_program_running = 0;
            basic_break_escape_state = 0;
            basic_reset_input_state();
            return status;
        }
        if (did_end) {
            basic_program_running = 0;
            basic_break_escape_state = 0;
            basic_reset_input_state();
            return BASIC_STATUS_OK;
        }
        if (jump_offset != BASIC_NO_OFFSET) {
            offset = jump_offset;
            continue;
        }
        if (jump_line != 0) {
            uint8_t found;
            offset = basic_find_line_offset(jump_line, &found);
            if (!found) {
                basic_program_running = 0;
                basic_break_escape_state = 0;
                basic_reset_input_state();
                return BASIC_STATUS_LINE_NOT_FOUND;
            }
            continue;
        }
        offset = next_offset;
    }

    basic_program_running = 0;
    basic_break_escape_state = 0;
    basic_reset_input_state();
    return BASIC_STATUS_OK;
}

static basic_status_t basic_complete_input_line(const char *line)
{
    basic_status_t status;

    if (basic_input_mode == BASIC_INPUT_STRING) {
        status = basic_assign_string_buffer(basic_input_variable, line, tiny_strlen(line));
    }
    else if (basic_input_mode == BASIC_INPUT_NUMERIC) {
        basic_parser_t parser;
        int16_t value;

        parser.ptr = line;
        parser.status = BASIC_STATUS_OK;
        value = basic_parse_expression(&parser);
        if (parser.status != BASIC_STATUS_OK) {
            status = parser.status;
        }
        else {
            parser_skip_spaces(&parser);
            if (*parser.ptr != 0) {
                status = BASIC_STATUS_SYNTAX;
            }
            else {
                status = basic_set_numeric_variable(basic_input_variable, value);
            }
        }
    }
    else {
        return BASIC_STATUS_SYNTAX;
    }

    if (status != BASIC_STATUS_OK) {
        basic_program_running = 0;
        basic_break_escape_state = 0;
        basic_break_requested = 0;
        basic_reset_input_state();
        return status;
    }

    {
        uint16_t resume_offset = basic_input_resume_offset;

        basic_reset_input_state();
        if (resume_offset == BASIC_NO_OFFSET) {
            basic_program_running = 0;
            basic_break_escape_state = 0;
            basic_break_requested = 0;
            return BASIC_STATUS_OK;
        }
        return basic_resume_program(resume_offset);
    }
}

static basic_status_t basic_execute_statement(const char *text, uint8_t from_program,
                                              uint16_t current_offset,
                                              uint16_t next_offset,
                                              uint16_t *jump_line,
                                              uint16_t *jump_offset,
                                              uint8_t *did_end)
{
    basic_parser_t parser;

    *jump_line = 0;
    *jump_offset = BASIC_NO_OFFSET;
    *did_end = 0;

    parser.ptr = text;
    parser.status = BASIC_STATUS_OK;
    parser_skip_spaces(&parser);

    if (*parser.ptr == 0) {
        return BASIC_STATUS_OK;
    }

    if (parser_match_keyword(&parser, "REM")) {
        return BASIC_STATUS_OK;
    }

    if (parser_match_keyword(&parser, "PRINT")) {
        return basic_execute_print(&parser);
    }

    if (parser_match_keyword(&parser, "INPUT")) {
        uint8_t variable_index;
        basic_input_mode_t input_mode;

        if (parser_parse_string_variable(&parser, &variable_index)) {
            input_mode = BASIC_INPUT_STRING;
        }
        else if (parser_parse_numeric_variable(&parser, &variable_index)) {
            input_mode = BASIC_INPUT_NUMERIC;
        }
        else {
            return BASIC_STATUS_SYNTAX;
        }

        parser_skip_spaces(&parser);
        if (*parser.ptr != 0) {
            return BASIC_STATUS_SYNTAX;
        }
        return basic_begin_input(input_mode, variable_index,
                                 from_program ? next_offset : BASIC_NO_OFFSET);
    }

    if (parser_match_keyword(&parser, "LET")) {
        return basic_parse_assignment(text);
    }

    if (is_ascii_alpha(*parser.ptr)) {
        const char *scan = parser.ptr + 1;
        if (*scan == '$') {
            return basic_parse_assignment(text);
        }
        while (*scan == ' ' || *scan == '\t') {
            scan++;
        }
        if (*scan == '=') {
            return basic_parse_assignment(text);
        }
    }

    if (from_program && parser_match_keyword(&parser, "FOR")) {
        uint8_t variable_index;
        int16_t start_value;
        int16_t limit_value;
        int16_t step_value = 1;

        if (!parser_parse_numeric_variable(&parser, &variable_index)) {
            return BASIC_STATUS_SYNTAX;
        }
        if (!parser_match_char(&parser, '=')) {
            return BASIC_STATUS_SYNTAX;
        }

        start_value = basic_parse_expression(&parser);
        if (parser.status != BASIC_STATUS_OK) {
            return parser.status;
        }
        if (!parser_match_keyword(&parser, "TO")) {
            return BASIC_STATUS_SYNTAX;
        }

        limit_value = basic_parse_expression(&parser);
        if (parser.status != BASIC_STATUS_OK) {
            return parser.status;
        }

        if (parser_match_keyword(&parser, "STEP")) {
            step_value = basic_parse_expression(&parser);
            if (parser.status != BASIC_STATUS_OK) {
                return parser.status;
            }
            if (step_value == 0) {
                return BASIC_STATUS_SYNTAX;
            }
        }

        parser_skip_spaces(&parser);
        if (*parser.ptr != 0) {
            return BASIC_STATUS_SYNTAX;
        }

        {
            basic_status_t status = basic_set_numeric_variable(variable_index, start_value);
            if (status != BASIC_STATUS_OK) {
                return status;
            }
        }

        if (!basic_for_condition_holds(start_value, limit_value, step_value)) {
            uint8_t found;
            uint16_t resume_offset = basic_find_matching_next_resume(current_offset, &found);

            if (!found) {
                return BASIC_STATUS_SYNTAX;
            }
            *jump_offset = resume_offset;
            return BASIC_STATUS_OK;
        }

        if (basic_for_stack_depth >= BASIC_FOR_STACK_DEPTH) {
            return BASIC_STATUS_FOR_STACK_FULL;
        }

        basic_for_stack[basic_for_stack_depth].variable = variable_index;
        basic_for_stack[basic_for_stack_depth].limit = limit_value;
        basic_for_stack[basic_for_stack_depth].step = step_value;
        basic_for_stack[basic_for_stack_depth].resume_offset = next_offset;
        basic_for_stack_depth++;
        return BASIC_STATUS_OK;
    }

    if (from_program && parser_match_keyword(&parser, "NEXT")) {
        basic_for_frame_t *frame;
        uint8_t variable_index;
        uint8_t have_variable = 0;

        if (parser_parse_numeric_variable(&parser, &variable_index)) {
            have_variable = 1;
        }
        else if (parser.status != BASIC_STATUS_OK) {
            return parser.status;
        }

        parser_skip_spaces(&parser);
        if (*parser.ptr != 0) {
            return BASIC_STATUS_SYNTAX;
        }
        if (basic_for_stack_depth == 0) {
            return BASIC_STATUS_NEXT_WITHOUT_FOR;
        }

        frame = &basic_for_stack[basic_for_stack_depth - 1];
        if (have_variable && frame->variable != variable_index) {
            return BASIC_STATUS_NEXT_WITHOUT_FOR;
        }

        {
            int16_t next_value =
                (int16_t)(basic_get_numeric_variable(frame->variable) + frame->step);
            basic_status_t status = basic_set_numeric_variable(frame->variable, next_value);

            if (status != BASIC_STATUS_OK) {
                return status;
            }

            if (basic_for_condition_holds(next_value, frame->limit, frame->step)) {
                *jump_offset = frame->resume_offset;
            }
            else {
                basic_for_stack_depth--;
            }
        }
        return BASIC_STATUS_OK;
    }

    if (from_program && parser_match_keyword(&parser, "GOTO")) {
        uint16_t target;

        if (!parser_parse_line_number(&parser.ptr, &target)) {
            return BASIC_STATUS_SYNTAX;
        }
        parser_skip_spaces(&parser);
        if (*parser.ptr != 0) {
            return BASIC_STATUS_SYNTAX;
        }
        *jump_line = target;
        return BASIC_STATUS_OK;
    }

    if (from_program && parser_match_keyword(&parser, "GOSUB")) {
        uint16_t target;

        if (!parser_parse_line_number(&parser.ptr, &target)) {
            return BASIC_STATUS_SYNTAX;
        }
        parser_skip_spaces(&parser);
        if (*parser.ptr != 0) {
            return BASIC_STATUS_SYNTAX;
        }
        if (basic_gosub_stack_depth >= BASIC_GOSUB_STACK_DEPTH) {
            return BASIC_STATUS_GOSUB_STACK_FULL;
        }
        basic_gosub_stack[basic_gosub_stack_depth++] = next_offset;
        *jump_line = target;
        return BASIC_STATUS_OK;
    }

    if (from_program && parser_match_keyword(&parser, "RETURN")) {
        parser_skip_spaces(&parser);
        if (*parser.ptr != 0) {
            return BASIC_STATUS_SYNTAX;
        }
        if (basic_gosub_stack_depth == 0) {
            return BASIC_STATUS_RETURN_WITHOUT_GOSUB;
        }
        *jump_offset = basic_gosub_stack[--basic_gosub_stack_depth];
        return BASIC_STATUS_OK;
    }

    if (from_program && parser_match_keyword(&parser, "IF")) {
        int16_t left;
        int16_t right;
        uint8_t condition = 0;
        uint16_t target;

        left = basic_parse_expression(&parser);
        if (parser.status != BASIC_STATUS_OK) {
            return parser.status;
        }

        parser_skip_spaces(&parser);
        if (*parser.ptr == '<' && parser.ptr[1] == '=') {
            parser.ptr += 2;
            right = basic_parse_expression(&parser);
            condition = (uint8_t)(left <= right);
        }
        else if (*parser.ptr == '>' && parser.ptr[1] == '=') {
            parser.ptr += 2;
            right = basic_parse_expression(&parser);
            condition = (uint8_t)(left >= right);
        }
        else if (*parser.ptr == '<' && parser.ptr[1] == '>') {
            parser.ptr += 2;
            right = basic_parse_expression(&parser);
            condition = (uint8_t)(left != right);
        }
        else if (*parser.ptr == '<') {
            parser.ptr++;
            right = basic_parse_expression(&parser);
            condition = (uint8_t)(left < right);
        }
        else if (*parser.ptr == '>') {
            parser.ptr++;
            right = basic_parse_expression(&parser);
            condition = (uint8_t)(left > right);
        }
        else if (*parser.ptr == '=') {
            parser.ptr++;
            right = basic_parse_expression(&parser);
            condition = (uint8_t)(left == right);
        }
        else {
            return BASIC_STATUS_SYNTAX;
        }

        if (parser.status != BASIC_STATUS_OK) {
            return parser.status;
        }
        if (!parser_match_keyword(&parser, "THEN")) {
            return BASIC_STATUS_SYNTAX;
        }
        if (!parser_parse_line_number(&parser.ptr, &target)) {
            return BASIC_STATUS_SYNTAX;
        }
        parser_skip_spaces(&parser);
        if (*parser.ptr != 0) {
            return BASIC_STATUS_SYNTAX;
        }

        if (condition) {
            *jump_line = target;
        }
        return BASIC_STATUS_OK;
    }

    if (from_program && parser_match_keyword(&parser, "END")) {
        parser_skip_spaces(&parser);
        if (*parser.ptr != 0) {
            return BASIC_STATUS_SYNTAX;
        }
        *did_end = 1;
        return BASIC_STATUS_OK;
    }

    return BASIC_STATUS_SYNTAX;
}

static basic_status_t basic_list_program(void)
{
    uint16_t offset = 0;

    if (basic_program_end == 0) {
        return BASIC_STATUS_EMPTY_PROGRAM;
    }

    while (offset < basic_program_end) {
        console_print_unsigned(basic_record_line(offset));
        console_print_char(' ');
        console_print_string(basic_record_text(offset));
        console_print_line("");
        offset += basic_record_size(offset);
    }

    return BASIC_STATUS_OK;
}

static basic_status_t basic_run_program(void)
{
    if (basic_program_end == 0) {
        return BASIC_STATUS_EMPTY_PROGRAM;
    }

    basic_reset_variables();
    basic_program_running = 1;
    basic_break_requested = 0;
    basic_break_escape_state = 0;
    return basic_resume_program(0);
}

static basic_status_t basic_execute_immediate(const char *text)
{
    basic_parser_t parser;
    uint16_t jump_line = 0;
    uint16_t jump_offset = BASIC_NO_OFFSET;
    uint8_t did_end = 0;

    parser.ptr = text;
    parser.status = BASIC_STATUS_OK;
    parser_skip_spaces(&parser);

    if (*parser.ptr == 0) {
        return BASIC_STATUS_OK;
    }

    if (parser_match_keyword(&parser, "RUN")) {
        parser_skip_spaces(&parser);
        if (*parser.ptr != 0) {
            return BASIC_STATUS_SYNTAX;
        }
        return basic_run_program();
    }

    if (parser_match_keyword(&parser, "LIST")) {
        parser_skip_spaces(&parser);
        if (*parser.ptr != 0) {
            return BASIC_STATUS_SYNTAX;
        }
        return basic_list_program();
    }

    if (parser_match_keyword(&parser, "NEW")) {
        parser_skip_spaces(&parser);
        if (*parser.ptr != 0) {
            return BASIC_STATUS_SYNTAX;
        }
        basic_reset_program();
        basic_reset_variables();
        console_print_line("OK");
        return BASIC_STATUS_OK;
    }

    return basic_execute_statement(text, 0, BASIC_NO_OFFSET, BASIC_NO_OFFSET,
                                   &jump_line, &jump_offset, &did_end);
}

void basic_handle_input_line(const char *line)
{
    const char *scan = line;
    uint16_t line_number;
    basic_status_t status;

    while (*scan == ' ' || *scan == '\t') {
        scan++;
    }

    if (*scan == 0) {
        return;
    }

    if (parser_parse_line_number(&scan, &line_number)) {
        while (*scan == ' ' || *scan == '\t') {
            scan++;
        }

        if (*scan == 0) {
            basic_delete_line(line_number);
            console_print_line("OK");
            return;
        }

        status = basic_store_line(line_number, scan);
        if (status == BASIC_STATUS_OK) {
            console_print_line("OK");
        }
        else {
            basic_report_status(status);
        }
        return;
    }

    status = basic_execute_immediate(line);
    basic_report_status(status);
}

void basic_handle_run_control_byte(uint8_t ch)
{
    if (!basic_program_running) {
        return;
    }

    if (basic_input_mode != BASIC_INPUT_NONE) {
        if (ch == 0x1b) {
            basic_program_running = 0;
            basic_break_requested = 0;
            basic_break_escape_state = 0;
            basic_reset_input_state();
            basic_report_status(BASIC_STATUS_BREAK);
            console_prepare_input_row();
            return;
        }

        if (console_handle_byte(ch) == CONSOLE_EVENT_SUBMIT) {
            const char *line = console_submit_current_row();
            basic_status_t status = basic_complete_input_line(line);

            basic_report_status(status);
            if (status != BASIC_STATUS_INPUT_WAIT) {
                console_prepare_input_row();
            }
        }
        return;
    }

    if (basic_break_escape_state == 0) {
        if (ch == 0x1b) {
            basic_break_requested = 1;
            basic_break_escape_state = 1;
        }
        return;
    }

    if (basic_break_escape_state == 1) {
        if (ch == '[' || ch == 'O') {
            basic_break_escape_state = 2;
        }
        else {
            basic_break_escape_state = 0;
        }
        return;
    }

    basic_break_escape_state = 0;
}
