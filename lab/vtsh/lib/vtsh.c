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

char* trim(char* str) {
  while (*str == ' ' || *str == '\t') {
    str++;
  }

  int len = (int)strlen(str);
  while (len > 0 && (str[len - 1] == ' ' || str[len - 1] == '\t')) {
    len--;
  }
  str[len] = '\0';

  return str;
}

int split_args(const char* cmd, char** args) {
  int num_args = 0;
  char* arg_start = NULL;
  int in_quotes = 0;
  char quote_char = 0;
  char arg_buffer[BUFFER_SIZE];
  int arg_len = 0;

  int idx = 0;
  while (cmd[idx] != '\0') {
    char sym = cmd[idx];

    if ((sym == '"' || sym == '\'') && !in_quotes) {
      in_quotes = 1;
      quote_char = sym;
      idx++;
      continue;
    }
    if (sym == quote_char && in_quotes) {
      in_quotes = 0;
      idx++;
      continue;
    }

    if ((sym == ' ' || sym == '\t') && !in_quotes) {
      if (arg_len > 0) {
        arg_buffer[arg_len] = '\0';
        args[num_args] = strdup(arg_buffer);
        if (!args[num_args]) {
          return -1;
        }
        num_args++;
        arg_len = 0;
      }
      idx++;
      continue;
    }

    if (arg_len < BUFFER_SIZE - 1) {
      arg_buffer[arg_len++] = sym;
    }
    idx++;
  }

  if (arg_len > 0) {
    arg_buffer[arg_len] = '\0';
    args[num_args] = strdup(arg_buffer);
    if (!args[num_args]) {
      return -1;
    }
    num_args++;
  }

  args[num_args] = NULL;
  return num_args;
}

int tokenize_line(char* line, char** tokens, cmd_operator_t* operators) {
  int token_count = 0;
  int idx = 0;
  int len = (int)strlen(line);
  int token_start = 0;

  while (idx <= len) {
    if (idx < len &&
        (line[idx] == '&' || line[idx] == '|' || line[idx] == ';')) {
      int token_end = idx;

      while (token_end > token_start &&
             (line[token_end - 1] == ' ' || line[token_end - 1] == '\t')) {
        token_end--;
      }

      if (token_end > token_start) {
        int cmd_len = token_end - token_start;
        char* cmd = malloc(cmd_len + 1);
        if (!cmd) {
          return -1;
        }

        strncpy(cmd, &line[token_start], cmd_len);
        cmd[cmd_len] = '\0';

        tokens[token_count] = cmd;

        if (idx + 1 < len && line[idx] == '&' && line[idx + 1] == '&') {
          operators[token_count] = CMD_AND;
          idx += 2;
        } else if (idx + 1 < len && line[idx] == '|' && line[idx + 1] == '|') {
          operators[token_count] = CMD_OR;
          idx += 2;
        } else if (line[idx] == ';') {
          operators[token_count] = CMD_SEQ;
          idx++;
        } else if (line[idx] == '&') {
          operators[token_count] = CMD_BG;
          idx++;
        } else {
          return -1;
        }

        token_count++;

        while (idx < len && (line[idx] == ' ' || line[idx] == '\t')) {
          idx++;
        }
        token_start = idx;
      } else {
        return -1;
      }
    } else {
      idx++;
    }
  }

  if (token_start < len) {
    char* cmd_str = &line[token_start];
    cmd_str = trim(cmd_str);

    if (strlen(cmd_str) > 0) {
      tokens[token_count] = strdup(cmd_str);
      if (!tokens[token_count]) {
        return -1;
      }

      operators[token_count] = CMD_NONE;
      token_count++;
    }
  }

  return token_count;
}

int execute_single(char** args) {
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
      (void)fprintf(stdout, "Command not found\n");
      (void)fflush(stdout);
      _exit(ENOENT);
    } else {
      (void)fprintf(stdout, "Error executing command\n");
      (void)fflush(stdout);
      _exit(1);
    }
  } else if (pid > 0) {
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
      return WEXITSTATUS(status);
    }
    return 1;
  } else {
    perror("fork");
    return 1;
  }
}

int execute_background(char** args) {
  if (args[0] == NULL) {
    return 0;
  }

  pid_t pid = fork();
  if (pid == 0) {
    execvp(args[0], args);
    _exit(1);
  } else if (pid > 0) {
    return 0;
  } else {
    perror("fork");
    return 1;
  }
}

int execute(char* line) {
  char* line_copy = strdup(line);
  if (!line_copy) {
    return 1;
  }

  char* tokens[MAX_COMMANDS];
  cmd_operator_t operators[MAX_COMMANDS];

  int num_tokens = tokenize_line(line_copy, tokens, operators);
  free(line_copy);

  if (num_tokens <= 0) {
    return 0;
  }

  int status = 0;

  for (int i = 0; i < num_tokens; i++) {
    char* args[MAX_ARGS];
    int num_args = split_args(tokens[i], args);

    if (num_args <= 0) {
      continue;
    }

    int should_execute = 1;

    if (i > 0) {
      cmd_operator_t prev_op = operators[i - 1];

      if (prev_op == CMD_AND) {
        should_execute = (status == 0);
      } else if (prev_op == CMD_OR) {
        should_execute = (status != 0);
      } else if (prev_op == CMD_SEQ) {
        should_execute = 1;
      }
    }

    if (should_execute) {
      if (operators[i] == CMD_BG) {
        execute_background(args);
        status = 0;
      } else {
        status = execute_single(args);
      }
    }

    for (int j = 0; args[j] != NULL; j++) {
      free(args[j]);
    }
  }

  for (int i = 0; i < num_tokens; i++) {
    free(tokens[i]);
  }

  return status;
}

int parse_line(char* line, char** args, int max_args) {
  int num_args = 0;
  char* lasts = NULL;
  char* sep = " \t";
  char* token = strtok_r(line, sep, &lasts);

  while (token != NULL && num_args < max_args - 1) {
    args[num_args++] = token;
    token = strtok_r(NULL, sep, &lasts);
  }

  args[num_args] = NULL;
  return num_args;
}
