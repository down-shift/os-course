#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#define FILE_MODE 0644
#define INT_SIZE ((off_t)sizeof(int32_t))
#define ARGS_REQUIRED 7
#define PARSE_BASE 10

static void usage(const char* prog) {
  (void)fprintf(stderr, "Usage: %s <file> <count> <max_val> <search_value> <replace_value> <repetitions>\n", prog);
}

static long parse_long(const char* str, int* is_valid) {
  char* end = NULL;
  errno = 0;
  long val = strtol(str, &end, PARSE_BASE);
  *is_valid = (errno == 0 && end && *end == '\0');
  return val;
}

static void set_no_cache(int fd) {
  (void)fcntl(fd, F_NOCACHE, 1);
}

static int replace_all(int file_fd, size_t count, int32_t target, int32_t replacement, long* last_found, size_t* replaced) {
  *replaced = 0;
  for (size_t i = 0; i < count; i++) {
    int32_t value = 0;
    if (pread(file_fd, &value, sizeof(value), (off_t)i * INT_SIZE) != (ssize_t)sizeof(value)) {
      perror("pread");
      return -1;
    }
    if (value == target) {
      if (pwrite(file_fd, &replacement, sizeof(replacement), (off_t)i * INT_SIZE) != (ssize_t)sizeof(replacement)) {
        perror("pwrite");
        return -1;
      }
      *last_found = (long)i;
      (*replaced)++;
    }
  }
  return 0;
}

static int perform_replacements(const char* path, size_t count, int32_t max_random, int32_t search32, int32_t replace32, long repetitions, long* last_found) {
  /* Always regenerate file contents */
  int file_fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_SYNC, FILE_MODE);
  if (file_fd < 0) {
    perror("open");
    return -1;
  }
  set_no_cache(file_fd);

  for (size_t i = 0; i < count; i++) {
    /* Fill with a random number in [0, max_random] */
    int32_t value = (int32_t)((int64_t)random() % ((int64_t)max_random + 1));
    if (pwrite(file_fd, &value, sizeof(value), (off_t)i * INT_SIZE) != (ssize_t)sizeof(value)) {
      perror("pwrite");
      close(file_fd);
      return -1;
    }
  }
  (void)fsync(file_fd);
  long last_found_any = -1;
  size_t total_replaced = 0;
  for (long rep = 0; rep < repetitions; rep++) {
    int32_t target = search32;
    int32_t replacement = replace32;
    *last_found = -1;
    size_t replaced = 0;
    if (replace_all(file_fd, count, target, replacement, last_found, &replaced) != 0) {
      close(file_fd);
      return -1;
    }
    if (replaced > 0) {
      total_replaced += replaced;
      last_found_any = *last_found;
    }
    (void)fsync(file_fd);
  }

  close(file_fd);
  *last_found = last_found_any;
  return (total_replaced > 0) ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc != ARGS_REQUIRED) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }
  const char* path = argv[1];
  int parse_ok = 0;
  long count_val = parse_long(argv[2], &parse_ok);
  if (!parse_ok || count_val <= 0) {
    (void)fprintf(stderr, "Invalid count\n");
    return EXIT_FAILURE;
  }
  long max_random_val = parse_long(argv[3], &parse_ok);
  if (!parse_ok || max_random_val <= 0 || max_random_val > INT32_MAX) {
    (void)fprintf(stderr, "Invalid max_val\n");
    return EXIT_FAILURE;
  }
  long search_val = parse_long(argv[4], &parse_ok);
  if (!parse_ok) {
    (void)fprintf(stderr, "Invalid search value\n");
    return EXIT_FAILURE;
  }
  long replace_val = parse_long(argv[5], &parse_ok);
  if (!parse_ok) {
    (void)fprintf(stderr, "Invalid replace value\n");
    return EXIT_FAILURE;
  }
  long repetitions = parse_long(argv[6], &parse_ok);
  if (!parse_ok || repetitions <= 0) {
    (void)fprintf(stderr, "Invalid repetitions\n");
    return EXIT_FAILURE;
  }
  srandom((unsigned)(time(NULL)));

  int32_t search32 = (int32_t)search_val;
  int32_t replace32 = (int32_t)replace_val;
  int32_t max_random = (int32_t)max_random_val;
  size_t count = (size_t)count_val;

  long last_found = -1;
  int status = perform_replacements(path, count, max_random, search32, replace32, repetitions, &last_found);
  if (status == 0 && last_found >= 0) {
    (void)printf("Replaced (last index %ld)\n", last_found);
    return EXIT_SUCCESS;
  }
  (void)printf("Value not found\n");
  return EXIT_FAILURE;
}
