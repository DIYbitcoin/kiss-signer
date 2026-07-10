#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdbool.h>

/*
 * Run `test_fn` across every file in `dir` whose name contains `suffix`,
 * or against a single file if one was passed as argv[1]. Prints a summary
 * banner and a "Tests passed: N/M" line. Returns 0 on full pass, 1 otherwise.
 *
 *   argc, argv — forwarded from main; argv[1] may name a single case.
 *   title      — banner line, e.g. "UR Decoder Test (bytes)".
 *   dir        — directory to scan, e.g. "tests/test_cases/bytes".
 *   suffix     — substring a filename must contain, e.g. ".UR_fragments.txt".
 *   test_fn    — invoked per case; returns true on pass.
 */
int run_test_suite(int argc, char *argv[], const char *title, const char *dir,
                   const char *suffix, bool (*test_fn)(const char *filepath));

#endif // TEST_HARNESS_H
