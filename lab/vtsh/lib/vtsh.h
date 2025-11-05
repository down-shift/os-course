#pragma once

#define BUFFER_SIZE 1024
#define MAX_ARGS 64

const char* vtsh_prompt();

int read_line(char* buffer, int max_len);

int parse_line(char* line, char** args, int max_args);

int execute(char** args);

int execute_multiple(const char* input);
