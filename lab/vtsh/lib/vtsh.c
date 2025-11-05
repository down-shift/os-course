#include "vtsh.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

const char* vtsh_prompt() {
  return "vtsh> ";
}

int read_line(char* buffer, int max_len) {
  int pos = 0;
  char sym = 0;
  ssize_t result = 0;

  while (pos < max_len - 1) {
    result = read(STDIN_FILENO, &sym, 1);
    if (result <= 0) {
      buffer[pos] = '\0';
      return pos > 0 ? 1 : 0;
    }
    if (sym == '\n') {
      buffer[pos] = '\0';
      return 1;
    }
    buffer[pos++] = sym;
  }
  buffer[pos] = '\0';
  return 1;
}


int parse_line(char* line, char** args, int max_args) {
  int num_args = 0;
  char* lasts = NULL;
  char* sep = " ";
  char* token = strtok_r(line, sep, &lasts);
  while (token != NULL && num_args < max_args - 1) {
    args[num_args++] = token;
    token = strtok_r(NULL, sep, &lasts);
  }
  args[num_args] = NULL;
  return num_args;
}

int execute(char** args) {
  if (args[0] == NULL) {
    return 0;
  }
  if (strcmp("exit", args[0]) == 0) {
    _exit(0);
  }
  pid_t pid = fork();
  if (pid == 0) {
    execvp(args[0], args);
    if (errno == ENOENT) {
      printf("Command not found\n");
      (void)fflush(stdout);
    } else {
      perror("execvp");
    }
    _exit(ENOENT);
  } else if (pid > 0) {
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  } else {
    perror("fork");
    return 1;
  }
}
