#define _POSIX_C_SOURCE 200809L
#include "test_harness.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TEST_FILES 256

static int run_single(const char *title, const char *dir, const char *arg,
                      bool (*test_fn)(const char *)) {
  char filepath[512];
  if (strstr(arg, dir) == arg) {
    snprintf(filepath, sizeof(filepath), "%s", arg);
  } else {
    snprintf(filepath, sizeof(filepath), "%s/%s", dir, arg);
  }
  printf("=== %s ===\n", title);
  printf("Running single test: %s\n", filepath);
  bool ok = test_fn(filepath);
  printf("\n=== Summary ===\n");
  printf("Test %s\n", ok ? "PASSED" : "FAILED");
  return ok ? 0 : 1;
}

int run_test_suite(int argc, char *argv[], const char *title, const char *dir,
                   const char *suffix, bool (*test_fn)(const char *)) {
  if (argc > 1) {
    return run_single(title, dir, argv[1], test_fn);
  }

  printf("=== %s ===\n", title);

  DIR *d = opendir(dir);
  if (!d) {
    fprintf(stderr, "Failed to open directory: %s\n", dir);
    return 1;
  }

  char *files[MAX_TEST_FILES];
  int file_count = 0;
  struct dirent *entry;
  while ((entry = readdir(d)) != NULL && file_count < MAX_TEST_FILES) {
    if (strstr(entry->d_name, suffix)) {
      char filepath[512];
      snprintf(filepath, sizeof(filepath), "%s/%s", dir, entry->d_name);
      files[file_count++] = strdup(filepath);
    }
  }
  closedir(d);

  if (file_count == 0) {
    fprintf(stderr, "No '*%s' files found in %s\n", suffix, dir);
    return 1;
  }

  int passed = 0;
  for (int i = 0; i < file_count; i++) {
    if (test_fn(files[i]))
      passed++;
    free(files[i]);
  }

  printf("\n=== Summary ===\n");
  printf("Tests passed: %d/%d\n", passed, file_count);
  return (passed == file_count) ? 0 : 1;
}
