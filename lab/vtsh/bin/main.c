#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vtsh.h"

int main() {
  char buffer[BUFFER_SIZE];

  vtsh_init();

  while (1) {
    printf("%s", vtsh_prompt());
    (void)fflush(stdout);

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
