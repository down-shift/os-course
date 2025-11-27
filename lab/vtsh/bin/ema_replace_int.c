#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define FILE_MODE 0644
#define INT_SIZE ((off_t)sizeof(int32_t))
#define ARGS_REQUIRED 6
#define PARSE_BASE 10

static void usage(const char* prog) {
  (void)fprintf(stderr, "Usage: %s <file> <count> <search_value> <replace_value> <repetitions>\n", prog);
}

static long parse_long(const char* str, int* is_valid) {
  char* end = NULL;
  errno = 0;
  long val = strtol(str, &end, PARSE_BASE);
  *is_valid = (errno == 0 && end && *end == '\0');
  return val;
}

static int replace_first(int file_fd, size_t count, int32_t target, int32_t replacement, long* last_found) {
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
      return 0;
    }
  }
  return 0;
}

static int perform_replacements(const char* path, size_t count, int32_t search32, int32_t replace32, long repetitions, long* last_found) {
  int file_fd = open(path, O_RDWR | O_CREAT, FILE_MODE);
  if (file_fd < 0) {
    perror("open");
    return -1;
  }

  struct stat file_stat;
  if (fstat(file_fd, &file_stat) < 0) {
    perror("fstat");
    close(file_fd);
    return -1;
  }
  if (file_stat.st_size % (off_t)sizeof(int32_t) != 0) {
    (void)fprintf(stderr, "Invalid file size (not multiple of int32)\n");
    close(file_fd);
    return -1;
  }

  size_t current_count = (size_t)(file_stat.st_size / INT_SIZE);
  if ((size_t)file_stat.st_size < count * INT_SIZE) {
    for (size_t i = current_count; i < count; i++) {
      int32_t value = (int32_t)i;
      if (pwrite(file_fd, &value, sizeof(value), (off_t)i * INT_SIZE) != (ssize_t)sizeof(value)) {
        perror("pwrite");
        close(file_fd);
        return -1;
      }
    }
  }

  *last_found = -1;
  for (long rep = 0; rep < repetitions; rep++) {
    int32_t target = (rep % 2 == 0) ? search32 : replace32;
    int32_t replacement = (rep % 2 == 0) ? replace32 : search32;
    *last_found = -1;
    if (replace_first(file_fd, count, target, replacement, last_found) != 0) {
      close(file_fd);
      return -1;
    }
  }

  close(file_fd);
  return 0;
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
  long search_val = parse_long(argv[3], &parse_ok);
  if (!parse_ok) {
    (void)fprintf(stderr, "Invalid search value\n");
    return EXIT_FAILURE;
  }
  long replace_val = parse_long(argv[4], &parse_ok);
  if (!parse_ok) {
    (void)fprintf(stderr, "Invalid replace value\n");
    return EXIT_FAILURE;
  }
  long repetitions = parse_long(argv[5], &parse_ok);
  if (!parse_ok || repetitions <= 0) {
    (void)fprintf(stderr, "Invalid repetitions\n");
    return EXIT_FAILURE;
  }
  int32_t search32 = (int32_t)search_val;
  int32_t replace32 = (int32_t)replace_val;
  size_t count = (size_t)count_val;

  long last_found = -1;
  if (perform_replacements(path, count, search32, replace32, repetitions, &last_found) != 0) {
    return EXIT_FAILURE;
  }

  if (last_found >= 0) {
    (void)printf("Replaced at index %ld\n", last_found);
  } else {
    (void)printf("Value not found\n");
  }
  return EXIT_SUCCESS;
}
