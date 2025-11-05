#include "vtsh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char buffer[BUFFER_SIZE];
    char* args[MAX_ARGS];

    int is_interactive = isatty(STDIN_FILENO);

    while (1) {
        if (is_interactive) {
            printf("%s", vtsh_prompt());
            // fflush(stdout);
        }

        if (!read_line(buffer, BUFFER_SIZE)) {
            break;  // EOF
        }

        // Remove trailing newline
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len - 1] == '\n') {
            buffer[len - 1] = '\0';
        }

        // Skip empty lines
        if (strlen(buffer) == 0) {
            continue;
        }

        int num_args = parse_line(buffer, args, MAX_ARGS);
        if (num_args > 0) {
            execute(args);
        }
    }

    return EXIT_SUCCESS;
}
