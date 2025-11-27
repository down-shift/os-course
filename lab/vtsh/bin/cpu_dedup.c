#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define BASE 10

static int deduplicate_array(int* arr, int size) {
  int unique_count = 0;

  for (int i = 0; i < size; i++) {
    int is_duplicate = 0;
    for (int j = 0; j < unique_count; j++) {
      if (arr[i] == arr[j]) {
        is_duplicate = 1;
        break;
      }
    }

    if (!is_duplicate) {
      arr[unique_count++] = arr[i];
    }
  }

  return unique_count;
}

static long parse_long(const char* str, int* ok) {
  char* end = NULL;
  errno = 0;
  long val = strtol(str, &end, BASE);
  *ok = (errno == 0 && end && *end == '\0');
  return val;
}

static void fill_random(int* arr, int size) {
  for (int i = 0; i < size; i++) {
    arr[i] = rand();
  }
}

int main(int argc, char** argv) {
  if (argc != 3) {
    (void)fprintf(stderr, "Usage: %s <array_size> <repetitions>\n", argv[0]);
    return EXIT_FAILURE;
  }

  int ok = 0;
  long size_val = parse_long(argv[1], &ok);
  if (!ok || size_val <= 0) {
    (void)fprintf(stderr, "Invalid array size\n");
    return EXIT_FAILURE;
  }
  long reps_val = parse_long(argv[2], &ok);
  if (!ok || reps_val <= 0) {
    (void)fprintf(stderr, "Invalid repetitions\n");
    return EXIT_FAILURE;
  }

  int size = (int)size_val;
  int repetitions = (int)reps_val;

  int* data = malloc((size_t)size * sizeof(int));
  if (!data) {
    (void)fprintf(stderr, "Allocation failed\n");
    return EXIT_FAILURE;
  }

  srand((unsigned int)time(NULL));

  int last_unique = 0;
  for (int r = 0; r < repetitions; r++) {
    fill_random(data, size);
    last_unique = deduplicate_array(data, size);
  }

  (void)printf("Last unique count: %d\n", last_unique);

  free(data);
  return EXIT_SUCCESS;
}
