#pragma once

#define BUFFER_SIZE 1024
#define MAX_ARGS 64
#define MAX_COMMANDS 50

typedef enum {
    CMD_NONE,
    CMD_AND,
    CMD_OR,
    CMD_SEQ,
    CMD_BG
} cmd_operator_t;

typedef struct {
    char* args[MAX_ARGS];
    cmd_operator_t next_op;
} command_t;

const char* vtsh_prompt();

int read_line(char* buffer, int max_len);

int parse_line(char* line, char** args, int max_args);

int execute(char* line);

int execute_multiple(const char* input);
