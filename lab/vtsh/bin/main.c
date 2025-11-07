#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vtsh.h"

int main(void) {
  char buffer[BUFFER_SIZE];
  int is_interactive = isatty(STDIN_FILENO);

  while (1) {
    if (is_interactive) {
      printf("%s", vtsh_prompt());
      (void)fflush(stdout);
    }

    if (!read_line(buffer, BUFFER_SIZE)) {
      break;
    }

    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n') {
      buffer[len - 1] = '\0';
    }

    if (strlen(buffer) == 0) {
      continue;
    }

    execute(buffer);
  }

  return EXIT_SUCCESS;
}
