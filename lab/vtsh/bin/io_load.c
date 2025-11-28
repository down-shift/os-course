#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FILE_MODE 0644
#define ALIGNMENT 4096
#define MAX_ARGS 8

typedef enum { MODE_READ, MODE_WRITE } io_mode_t;

typedef enum { ACCESS_SEQUENCE, ACCESS_RANDOM } access_type_t;

static long parse_long(const char* str, int* ok) {
  char* end = NULL;
  errno = 0;
  long val = strtol(str, &end, 10);
  *ok = (errno == 0 && end && *end == '\0');
  return val;
}

static int parse_range(const char* str, off_t* start, off_t* end) {
  const char* dash = strchr(str, '-');
  if (!dash) {
    return -1;
  }

  size_t start_len = (size_t)(dash - str);
  if (start_len == 0) {
    return -1;
  }

  char* start_str = strndup(str, start_len);
  if (!start_str) {
    return -1;
  }
  char* end_str = strdup(dash + 1);
  if (!end_str) {
    free(start_str);
    return -1;
  }

  int ok = 0;
  long start_val = parse_long(start_str, &ok);
  if (!ok || start_val < 0) {
    free(start_str);
    free(end_str);
    return -1;
  }
  long end_val = parse_long(end_str, &ok);
  free(start_str);
  free(end_str);
  if (!ok || end_val < 0) {
    return -1;
  }

  *start = (off_t)start_val;
  *end = (off_t)end_val;
  if (*end != 0 && *end < *start) {
    return -1;
  }
  return 0;
}

static void fill_buffer(char* buf, size_t size, int seed) {
  for (size_t i = 0; i < size; i++) {
    buf[i] = (char)((i + seed) & 0xFF);
  }
}

static off_t choose_offset(
    access_type_t type, off_t start, off_t end, size_t block_size, long iter
) {
  if (type == ACCESS_SEQUENCE) {
    return start + (off_t)iter * (off_t)block_size;
  }
  long span = (long)(end - start - (off_t)block_size + 1);
  if (span <= 0) {
    return start;
  }
  long rnd = rand() % span;
  return start + (off_t)rnd;
}

static void usage(const char* prog) {
  (void)fprintf(
      stderr,
      "Usage: %s <read|write> <block_size> <block_count> <file> <start-end> "
      "<direct:on|off> <sequence|random>\n",
      prog
  );
}

int main(int argc, char** argv) {
  if (argc != MAX_ARGS) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  io_mode_t mode;
  if (strcmp(argv[1], "read") == 0) {
    mode = MODE_READ;
  } else if (strcmp(argv[1], "write") == 0) {
    mode = MODE_WRITE;
  } else {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  int ok = 0;
  long block_size_val = parse_long(argv[2], &ok);
  if (!ok || block_size_val <= 0) {
    (void)fprintf(stderr, "Invalid block size\n");
    return EXIT_FAILURE;
  }

  long block_count_val = parse_long(argv[3], &ok);
  if (!ok || block_count_val <= 0) {
    (void)fprintf(stderr, "Invalid block count\n");
    return EXIT_FAILURE;
  }

  const char* path = argv[4];

  off_t range_start = 0;
  off_t range_end = 0;
  if (parse_range(argv[5], &range_start, &range_end) < 0) {
    (void)fprintf(stderr, "Invalid range\n");
    return EXIT_FAILURE;
  }

  int direct = 0;
  if (strcmp(argv[6], "on") == 0) {
    direct = 1;
  } else if (strcmp(argv[6], "off") == 0) {
    direct = 0;
  } else {
    (void)fprintf(stderr, "Invalid direct flag (use on/off)\n");
    return EXIT_FAILURE;
  }

  access_type_t access_type;
  if (strcmp(argv[7], "sequence") == 0) {
    access_type = ACCESS_SEQUENCE;
  } else if (strcmp(argv[7], "random") == 0) {
    access_type = ACCESS_RANDOM;
  } else {
    (void)fprintf(stderr, "Invalid access type (sequence/random)\n");
    return EXIT_FAILURE;
  }

  int flags = (mode == MODE_READ) ? O_RDONLY : (O_WRONLY | O_CREAT);
  if (direct) {
    flags |= F_NOCACHE;  // O_DIRECT
  }

  int fd = open(path, flags, FILE_MODE);
  if (fd < 0) {
    perror("open");
    return EXIT_FAILURE;
  }

  struct stat st;
  if (fstat(fd, &st) < 0) {
    perror("fstat");
    close(fd);
    return EXIT_FAILURE;
  }

  size_t block_size = (size_t)block_size_val;
  long block_count = block_count_val;

  off_t effective_end = range_end;
  if (effective_end == 0) {
    if (mode == MODE_READ) {
      effective_end = st.st_size;
    } else {
      effective_end = range_start + (off_t)block_size * (off_t)block_count;
    }
  }

  if (effective_end < range_start ||
      effective_end < (off_t)block_size + range_start) {
    (void)fprintf(stderr, "Range is too small for requested block size\n");
    close(fd);
    return EXIT_FAILURE;
  }

  void* buffer = NULL;
  if (direct) {
    if (posix_memalign(&buffer, ALIGNMENT, block_size) != 0) {
      (void)fprintf(stderr, "Buffer allocation failed\n");
      close(fd);
      return EXIT_FAILURE;
    }
  } else {
    buffer = malloc(block_size);
  }
  if (!buffer) {
    (void)fprintf(stderr, "Buffer allocation failed\n");
    close(fd);
    return EXIT_FAILURE;
  }

  srand((unsigned int)time(NULL));

  ssize_t io_size = (ssize_t)block_size;
  for (long iter = 0; iter < block_count; iter++) {
    off_t offset = choose_offset(
        access_type, range_start, effective_end, block_size, iter
    );
    if (offset + (off_t)block_size > effective_end) {
      break;
    }

    if (mode == MODE_WRITE) {
      fill_buffer((char*)buffer, block_size, (int)iter);
      if (pwrite(fd, buffer, block_size, offset) != io_size) {
        perror("pwrite");
        free(buffer);
        close(fd);
        return EXIT_FAILURE;
      }
    } else {
      if (pread(fd, buffer, block_size, offset) != io_size) {
        perror("pread");
        free(buffer);
        close(fd);
        return EXIT_FAILURE;
      }
    }
  }

  (void)printf("Completed %ld operations\n", block_count);

  free(buffer);
  close(fd);
  return EXIT_SUCCESS;
}
