#include "vtsh.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define FILE_MODE 0644

typedef int file_descriptor_t;
typedef int open_flags_t;

typedef struct {
  int value;
} exit_status_t;

typedef struct {
  pid_t pid;
  int job_id;
  char done_msg[BUFFER_SIZE];
  size_t done_len;
  volatile sig_atomic_t active;
} background_job_t;

#define MAX_BG_JOBS 64

static background_job_t bg_jobs[MAX_BG_JOBS];
static int next_job_id = 1;

/*
    вариант: proc-fork shell-bg cpu-dedup ema-replace-int
*/


static long long now_ms() {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return ((long long)ts.tv_sec * 1000LL) + (long long)(ts.tv_nsec / 1000000LL);
}

static void print_duration_ms(long long elapsed_ms) {
  if (elapsed_ms > 0) {
    (void)fprintf(stderr, "Execution time: %lld ms\n", elapsed_ms);
    (void)fflush(stderr);
  }
}

static void sigchild_handler(int signo) {
  (void)signo;
  int status = 0;
  pid_t pid = 0;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    for (int i = 0; i < MAX_BG_JOBS; i++) {
      if (bg_jobs[i].active && bg_jobs[i].pid == pid) {
        (void)write(STDOUT_FILENO, bg_jobs[i].done_msg, bg_jobs[i].done_len);
        bg_jobs[i].active = 0;
        break;
      }
    }
  }
}

void vtsh_init() {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sigchild_handler;
  sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  sigemptyset(&sa.sa_mask);
  (void)sigaction(SIGCHLD, &sa, NULL);
}

const char* vtsh_prompt() {
  return "vtsh> ";
}

int read_line(char* buffer, int max_len) {
  for (int pos = 0; pos < max_len - 1; pos++) {
    ssize_t result = read(STDIN_FILENO, &buffer[pos], 1);
    if (result <= 0) {
      buffer[pos] = '\0';
      return pos > 0;
    }
    if (buffer[pos] == '\n') {
      buffer[pos] = '\0';
      return 1;
    }
  }
  buffer[max_len - 1] = '\0';
  return 1;
}

static char* trim(char* str) {
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

static int add_arg_to_list(char** args, int* num_args, char* arg_buffer, int* arg_len) {
  if (*arg_len > 0) {
    arg_buffer[*arg_len] = '\0';
    args[*num_args] = strdup(arg_buffer);
    if (!args[*num_args]) {
      for (int j = 0; j < *num_args; j++) {
        free(args[j]);
      }
      return -1;
    }
    (*num_args)++;
    *arg_len = 0;
  }
  return 0;
}

int split_args(const char* cmd, char** args) {
  int num_args = 0;
  int in_quotes = 0;
  char quote_char = 0;
  char arg_buffer[BUFFER_SIZE];
  int arg_len = 0;

  for (int idx = 0; cmd[idx] != '\0'; idx++) {
    char sym = cmd[idx];
    if ((sym == '"' || sym == '\'') && !in_quotes) {
      in_quotes = 1;
      quote_char = sym;
      continue;
    }
    if (sym == quote_char && in_quotes) {
      in_quotes = 0;
      continue;
    }
    if ((sym == ' ' || sym == '\t') && !in_quotes) {
      if (add_arg_to_list(args, &num_args, arg_buffer, &arg_len) < 0) {
          return -1;
      }
      continue;
    }
    if (arg_len < BUFFER_SIZE - 1) {
      arg_buffer[arg_len++] = sym;
    }
  }
  if (add_arg_to_list(args, &num_args, arg_buffer, &arg_len) < 0) {
      return -1;
  }
  args[num_args] = NULL;
  return num_args;
}

static cmd_operator_t get_operator(const char* line, int idx, int len, int* advance) {
  if (idx + 1 < len && line[idx] == '&' && line[idx + 1] == '&') {
    *advance = 2;
    return CMD_AND;
  }
  if (idx + 1 < len && line[idx] == '|' && line[idx + 1] == '|') {
    *advance = 2;
    return CMD_OR;
  }
  if (line[idx] == '|') {
    *advance = 1;
    return CMD_PIPE;
  }
  if (line[idx] == ';') {
    *advance = 1;
    return CMD_SEQ;
  }
  if (line[idx] == '&') {
    *advance = 1;
    return CMD_BG;
  }
  return CMD_NONE;
}

static int add_token_segment(char* line, int start, int end, char** tokens, cmd_operator_t* operators,
                             int token_count, cmd_operator_t operator_type) {
  while (start < end && (line[start] == ' ' || line[start] == '\t')) {
    start++;
  }
  while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t')) {
    end--;
  }
  if (start >= end) {
    return (operator_type == CMD_NONE) ? token_count : -1;
  }
  int cmd_len = end - start;
  char* cmd = malloc((size_t)cmd_len + 1);
  if (!cmd) {
    return -1;
  }
  memcpy(cmd, &line[start], (size_t)cmd_len);
  cmd[cmd_len] = '\0';
  tokens[token_count] = cmd;
  operators[token_count] = operator_type;
  return token_count + 1;
}

int tokenize_line(char* line, char** tokens, cmd_operator_t* operators) {
  int len = (int)strlen(line);
  int idx = 0;
  int token_count = 0;
  int token_start = 0;

  while (idx <= len) {
    int advance = 0;
    cmd_operator_t operator_type = CMD_NONE;
    if (idx < len && (line[idx] == '&' || line[idx] == '|' || line[idx] == ';')) {
      operator_type = get_operator(line, idx, len, &advance);
    }
    if (operator_type == CMD_NONE && idx < len) {
      idx++;
      continue;
    }
    token_count = add_token_segment(line, token_start, idx, tokens, operators, token_count, operator_type);
    if (token_count < 0) {
      return -1;
    }
    if (operator_type == CMD_NONE) {
      break;
    }
    idx += advance;
    while (idx < len && (line[idx] == ' ' || line[idx] == '\t')) {
      idx++;
    }
    token_start = idx;
  }
  return token_count;
}

typedef struct {
  char* input_file;
  char* output_file;
  char* append_file;
  int has_input;
  int has_output;
  int has_append;
  int heredoc_created;
} redirection_info_t;

static int is_redir_op(const char* arg) {
  if (!arg || arg[0] == '\0') {
    return 0;
  }
  int idx = 0;
  while (arg[idx] >= '0' && arg[idx] <= '9') {
    idx++;
  }
  return arg[idx] == '<' || arg[idx] == '>';
}

static int assign_file_once(char** target, int* flag, char* value) {
  if (*flag) {
    return -1;
  }
  *target = value;
  *flag = 1;
  return 0;
  return 0;
}

static int read_separate_file(char** args, int* read_idx, char** target, int* flag) {
  (*read_idx)++;
  if (!args[*read_idx] || is_redir_op(args[*read_idx])) {
    return -1;
  }
  int result = assign_file_once(target, flag, args[*read_idx]);
  (*read_idx)++;
  return result;
}

static int parse_redir_token(char* arg, int len, redirection_info_t* redir, char** args, int* read_idx) {
  if (len > 2 && arg[0] == '>' && arg[1] == '>') {
    return -1;
  }
  if (len > 1 && arg[0] == '<' && arg[1] != '<') {
    if (assign_file_once(&redir->input_file, &redir->has_input, arg + 1) < 0) {
      return -1;
    }
    (*read_idx)++;
    return 1;
  }
  if (len > 1 && arg[0] == '>' && arg[1] != '>') {
    if (redir->has_output || redir->has_append) {
      return -1;
    }
    if (assign_file_once(&redir->output_file, &redir->has_output, arg + 1) < 0) {
      return -1;
    }
    (*read_idx)++;
    return 1;
  }

  char** target = NULL;
  int* flag = NULL;
  if (len == 2 && arg[0] == '>' && arg[1] == '>') {
    if (redir->has_output || redir->has_append) {
      return -1;
    }
    target = &redir->append_file;
    flag = &redir->has_append;
  } else if (len == 1 && arg[0] == '<') {
    target = &redir->input_file;
    flag = &redir->has_input;
  } else if (len == 1 && arg[0] == '>') {
    if (redir->has_output || redir->has_append) {
      return -1;
    }
    target = &redir->output_file;
    flag = &redir->has_output;
  } else {
    return 0;
  }
  return read_separate_file(args, read_idx, target, flag) < 0 ? -1 : 1;
}

static int process_heredoc(const char* delimiter, redirection_info_t* redir) {
  char* content = malloc(BUFFER_SIZE);
  if (!content) {
    return -1;
  }
  int pos = 0;
  char line[BUFFER_SIZE];
  while (1) {
    if (!read_line(line, BUFFER_SIZE)) {
      free(content);
      return -1;
    }
    int len = (int)strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
      line[len - 1] = '\0';
    }
    if (strcmp(line, delimiter) == 0) {
      break;
    }
    if (pos > 0 && pos < BUFFER_SIZE - 1) {
      content[pos++] = '\n';
    }
    for (int i = 0; line[i] != '\0' && pos < BUFFER_SIZE - 1; i++) {
      content[pos++] = line[i];
    }
  }
  char temp_file[] = "/tmp/vtsh_heredoc_XXXXXX";
  int file_descriptor = mkstemp(temp_file);
  if (file_descriptor < 0) {
    free(content);
    return -1;
  }
  if (write(file_descriptor, content, pos) != pos) {
    close(file_descriptor);
    unlink(temp_file);
    free(content);
    return -1;
  }
  close(file_descriptor);
  free(content);
  redir->input_file = strdup(temp_file);
  if (!redir->input_file) {
    unlink(temp_file);
    return -1;
  }
  redir->has_input = 1;
  redir->heredoc_created = 1;
  return 0;
}

int parse_redirections(char** args, redirection_info_t* redir) {
  memset(redir, 0, sizeof(*redir));
  int write_idx = 0;
  int read_idx = 0;
  while (args[read_idx] != NULL) {
    char* arg = args[read_idx];
    int offset = 0;
    while (arg[offset] >= '0' && arg[offset] <= '9') {
      offset++;
    }
    char* redir_arg = arg + offset;
    int len = (int)strlen(redir_arg);
    int handled = parse_redir_token(redir_arg, len, redir, args, &read_idx);
    if (handled < 0) {
      return -1;
    }
    if (handled > 0) {
      continue;
    }
    if (len == 2 && arg[0] == '<' && arg[1] == '<') {
      if (redir->has_input) {
        return -1;
      }
      read_idx++;
      if (!args[read_idx] || process_heredoc(args[read_idx], redir) < 0) {
        return -1;
      }
      read_idx++;
      continue;
    }
    args[write_idx++] = args[read_idx++];
  }
  args[write_idx] = NULL;
  return 0;
}

static int redirect_fd(open_flags_t open_flags, const char* file, file_descriptor_t target_fd) {
  int file_descriptor = open(file, open_flags, FILE_MODE);
  if (file_descriptor < 0) {
    return -1;
  }
  if (dup2(file_descriptor, target_fd) < 0) {
    close(file_descriptor);
    return -1;
  }
  close(file_descriptor);
  return 0;
}

int setup_redirections(redirection_info_t* redir) {
  if (redir->has_input) {
    if (redirect_fd(O_RDONLY, redir->input_file, STDIN_FILENO) < 0) {
      return -1;
    }
  }
  if (redir->has_output) {
    if (redirect_fd(O_WRONLY | O_CREAT | O_TRUNC, redir->output_file, STDOUT_FILENO) < 0) {
      return -1;
    }
  }
  if (redir->has_append) {
    if (redirect_fd(O_WRONLY | O_CREAT | O_APPEND, redir->append_file, STDOUT_FILENO) < 0) {
      return -1;
    }
  }
  return 0;
}

static void free_args(char** args) {
  if (args) {
    for (int j = 0; args[j] != NULL; j++) {
      free(args[j]);
    }
  }
}

static void cleanup_heredoc(redirection_info_t* redir) {
  if (redir && redir->heredoc_created && redir->input_file) {
    unlink(redir->input_file);
    free(redir->input_file);
    redir->input_file = NULL;
    redir->heredoc_created = 0;
  }
}

static int exec_cmd(char** args, redirection_info_t* redir, int is_child) {
  char* args_copy[MAX_ARGS];
  int idx = 0;
  while (args[idx] != NULL && idx < MAX_ARGS - 1) {
    args_copy[idx] = strdup(args[idx]);
    if (!args_copy[idx]) {
      free_args(args_copy);
      return -1;
    }
    idx++;
  }
  args_copy[idx] = NULL;
  if (parse_redirections(args_copy, redir) < 0) {
    cleanup_heredoc(redir);
    free_args(args_copy);
    return -2;
  }
  if (!args_copy[0]) {
    cleanup_heredoc(redir);
    free_args(args_copy);
    return 0;
  }
  if (setup_redirections(redir) < 0) {
    if (is_child) {
      (void)fprintf(stdout, "I/O error\n");
      (void)fflush(stdout);
    }
    cleanup_heredoc(redir);
    free_args(args_copy);
    return -3;
  }
  execvp(args_copy[0], args_copy);
  (void)fprintf(stdout, errno == ENOENT ? "Command not found\n" : "Error executing command\n");
  (void)fflush(stdout);
  cleanup_heredoc(redir);
  free_args(args_copy);
  return -4;
}

static void build_command_repr(char** args, char* buffer, int buffer_size) {
  int pos = 0;
  for (int i = 0; args[i] != NULL && pos < buffer_size - 1; i++) {
    int written = snprintf(buffer + pos, (size_t)(buffer_size - pos), "%s%s", i == 0 ? "" : " ", args[i]);
    if (written < 0) {
      break;
    }
    pos += written;
    if (pos >= buffer_size - 1) {
      break;
    }
  }
  buffer[buffer_size - 1] = '\0';
}

static void add_background_job(pid_t pid, char** args) {
  sigset_t mask;
  sigset_t oldmask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);
  (void)sigprocmask(SIG_BLOCK, &mask, &oldmask);
  for (int i = 0; i < MAX_BG_JOBS; i++) {
    if (!bg_jobs[i].active) {
      bg_jobs[i].pid = pid;
      bg_jobs[i].job_id = next_job_id++;
      if (next_job_id < 1) {
        next_job_id = 1;
      }
      char cmd_buffer[BUFFER_SIZE];
      build_command_repr(args, cmd_buffer, BUFFER_SIZE);
      int formatted = snprintf(bg_jobs[i].done_msg, BUFFER_SIZE,
                               "[%d]  + %d done       %s\n",
                               bg_jobs[i].job_id, bg_jobs[i].pid, cmd_buffer);
      if (formatted < 0) {
        bg_jobs[i].done_len = 0;
      } else if (formatted >= BUFFER_SIZE) {
        bg_jobs[i].done_len = BUFFER_SIZE - 1;
      } else {
        bg_jobs[i].done_len = (size_t)formatted;
      }
      bg_jobs[i].active = 1;
      (void)sigprocmask(SIG_SETMASK, &oldmask, NULL);
      (void)fprintf(stdout, "[%d] %d\n", bg_jobs[i].job_id, bg_jobs[i].pid);
      (void)fflush(stdout);
      return;
    }
  }
  (void)sigprocmask(SIG_SETMASK, &oldmask, NULL);
}

static int run_cmd(char** args, int background) {
  if (!args[0]) {
    return 0;
  }
  if (strcmp("exit", args[0]) == 0) {
    _exit(0);
  }
  redirection_info_t redir;
  pid_t pid = fork();
  if (pid == 0) {
    int result = exec_cmd(args, &redir, 1);
    if (result == -2) {
      (void)fprintf(stdout, "Syntax error\n");
      (void)fflush(stdout);
    }
    _exit(result < 0 ? 1 : 0);
  }
  if (pid < 0) {
    perror("fork");
    return 1;
  }
  if (background) {
    add_background_job(pid, args);
    return 0;
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

int execute_single(char** args) {
  return run_cmd(args, 0);
}

int execute_background(char** args) {
  return run_cmd(args, 1);
}

static void close_pipes(int pipes[][2], int count) {
  for (int i = 0; i < count; i++) {
    close(pipes[i][0]);
    close(pipes[i][1]);
  }
}

static void setup_pipe_fds(int idx, int num_commands, int pipes[][2]) {
  if (idx > 0 && dup2(pipes[idx - 1][0], STDIN_FILENO) < 0) {
    perror("dup2");
    _exit(1);
  }
  if (idx < num_commands - 1 && dup2(pipes[idx][1], STDOUT_FILENO) < 0) {
    perror("dup2");
    _exit(1);
  }
  close_pipes(pipes, num_commands - 1);
}

int execute_pipe(char** args_list, int num_commands) {
  if (num_commands < 2) {
    return 1;
  }
  int pipes[MAX_COMMANDS - 1][2];
  pid_t pids[MAX_COMMANDS];
  for (int i = 0; i < num_commands - 1; i++) {
    if (pipe(pipes[i]) < 0) {
      perror("pipe");
      return 1;
    }
  }
  for (int i = 0; i < num_commands; i++) {
    pids[i] = fork();
    if (pids[i] < 0) {
    perror("fork");
      for (int j = 0; j < i; j++) {
        waitpid(pids[j], NULL, 0);
      }
      close_pipes(pipes, num_commands - 1);
      return 1;
    }
    if (pids[i] == 0) {
      setup_pipe_fds(i, num_commands, pipes);
      char* args[MAX_ARGS];
      if (split_args(args_list[i], args) <= 0) {
        free_args(args);
        _exit(1);
      }
      redirection_info_t redir;
      int result = exec_cmd(args, &redir, 1);
      free_args(args);
      if (result == -2) {
        (void)fprintf(stdout, "Syntax error\n");
        (void)fflush(stdout);
        _exit(1);
      }
      _exit(result < 0 ? 1 : 0);
    }
  }
  close_pipes(pipes, num_commands - 1);
  int status = 0;
  for (int i = 0; i < num_commands; i++) {
    int child_status = 0;
    waitpid(pids[i], &child_status, 0);
    if (i == num_commands - 1 && WIFEXITED(child_status)) {
      status = WEXITSTATUS(child_status);
    }
  }
  return status;
}

static int should_execute_cmd(cmd_operator_t prev_operator, exit_status_t exit_status) {
  if (prev_operator == CMD_SEQ) {
    return 1;
  }
  if (prev_operator == CMD_AND) {
    return exit_status.value == 0;
  }
  if (prev_operator == CMD_OR) {
    return exit_status.value != 0;
  }
  return 1;
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
  for (int i = 0; i < num_tokens;) {
    int pipe_start = i;
    while (i < num_tokens && (i == pipe_start || operators[i - 1] == CMD_PIPE)) {
      i++;
    }
    int pipe_count = i - pipe_start;
    exit_status_t exit_status_wrapper = {.value = status};
    int should_execute = (pipe_start == 0) || should_execute_cmd(operators[pipe_start - 1], exit_status_wrapper);
    if (should_execute) {
      long long start_ms = now_ms();
      if (pipe_count > 1) {
        status = execute_pipe(&tokens[pipe_start], pipe_count);
      } else {
        char* args[MAX_ARGS];
        int num_args = split_args(tokens[pipe_start], args);
        if (num_args > 0) {
          status = (operators[pipe_start] == CMD_BG) ? execute_background(args) : execute_single(args);
        }
        free_args(args);
      }
      long long end_ms = now_ms();
      if (!(pipe_count == 1 && operators[pipe_start] == CMD_BG)) {
        print_duration_ms(end_ms - start_ms);
      }
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
