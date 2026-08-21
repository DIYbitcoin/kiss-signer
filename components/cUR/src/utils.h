#ifndef UR_UTILS_H
#define UR_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// String manipulation utilities

/**
 * Check if string has prefix
 * @param str String to check
 * @param prefix Prefix to look for
 * @return true if str has prefix, false otherwise
 */
bool str_has_prefix(const char *str, const char *prefix);

/**
 * Convert string to lowercase (in-place)
 * @param str String to convert
 */
void str_to_lower(char *str);

/**
 * Split string by delimiter
 * @param str String to split
 * @param delimiter Delimiter character
 * @param parts Output array of string parts
 * @param max_parts Maximum number of parts
 * @return Number of parts found
 */
size_t str_split(const char *str, char delimiter, char **parts,
                 size_t max_parts);

/**
 * Check if string is a valid UR type
 * @param type Type string to validate
 * @return true if valid UR type, false otherwise
 */
bool is_ur_type(const char *type);

/**
 * Parse UR string into components
 * @param ur_str UR string to parse
 * @param type Output type string (allocated)
 * @param components Output components array (allocated)
 * @param component_count Output component count
 * @return true on success, false on error
 */
bool parse_ur_string(const char *ur_str, char **type, char ***components,
                     size_t *component_count);

/**
 * Parse sequence component (e.g., "1-5" -> seq_num=1, seq_len=5)
 * @param seq_str Sequence string
 * @param seq_num Output sequence number
 * @param seq_len Output sequence length
 * @return true on success, false on error
 */
bool parse_sequence_component(const char *seq_str, uint32_t *seq_num,
                              size_t *seq_len);

/**
 * Free string array
 * @param strings Array of strings
 * @param count Number of strings
 */
void free_string_array(char **strings, size_t count);

// Memory utilities

/**
 * Safe malloc with zero initialization
 * @param size Size to allocate
 * @return Allocated memory or NULL on error
 */
void *safe_malloc(size_t size);

/**
 * Safe malloc without zero initialization (for data buffers that will be
 * immediately overwritten)
 * @param size Size to allocate
 * @return Allocated memory or NULL on error
 */
void *safe_malloc_uninit(size_t size);

/**
 * Wrapped realloc — a hook point for platform allocators, not a safer
 * realloc. Semantics match standard realloc(3):
 *   - On success: returns a new pointer; the old block has been freed.
 *   - On failure: returns NULL; the OLD pointer is still valid and must
 *     be freed by the caller if no longer needed.
 *
 * Callers that reassign a struct field from the old pointer (e.g.
 * `obj->arr = safe_realloc(obj->arr, ...)`) MUST NULL-check the return
 * before assigning, or the struct field will dangle on failure. When
 * reallocating multiple fields in a row, commit each successful result
 * to the struct before attempting the next — otherwise a later failure
 * leaves the struct pointing at a block the earlier successful realloc
 * has already freed.
 *
 * @param ptr Pointer to reallocate (may be NULL — equivalent to malloc)
 * @param size New size (must be > 0; passing 0 is implementation-defined)
 * @return New pointer on success, NULL on failure (old pointer unchanged)
 */
void *safe_realloc(void *ptr, size_t size);

/**
 * Safe string duplication
 * @param str String to duplicate
 * @return Duplicated string or NULL on error
 */
char *safe_strdup(const char *str);

/**
 * Free a pointer and null it out. Expands the argument once; pass a
 * pointer lvalue, not an expression with side effects. free(NULL) is a
 * no-op per the C standard, so a NULL guard is not needed.
 */
#define safe_free(p)                                                           \
  do {                                                                         \
    free(p);                                                                   \
    (p) = NULL;                                                                \
  } while (0)

// Allocation-failure injection, for the desktop stress test. The hook exists
// only in the dedicated host-test build; production keeps the plain allocation
// wrappers and no mutable injector state. Three knobs, used together:
//
//   ur_alloc_arm(site, at)  — arm the hook: the `at`-th allocation (1-based)
//                             made at `site` fails, and the hook then disarms
//                             itself (fail-on-Nth, single shot). A failed
//                             safe_realloc keeps the old pointer, matching
//                             realloc(3), which the two-pass difference and
//                             the copy-before-mutate reduction rely on.
//   ur_alloc_hits()         — matching allocations attempted since the arm.
//                             The test asserts this to prove the armed site
//                             was actually reached, so a test can no longer
//                             pass by failing an allocation the target code
//                             never runs.
//   ur_site_enter/leave     — allocation-site tagging: push a site at
//                             function entry, restore the previous one at
//                             exit. An arm aimed at a site only counts (and
//                             can only fail) allocations made while that
//                             site is current, so the test does not have to
//                             count the allocations on the way to its target.
enum {
  UR_SITE_NONE = 0,   // untagged allocations; never the fail target
  UR_SITE_DIFF = 1,   // part_indexes_difference's count-exact allocation
  UR_SITE_COPY = 2,   // part_indexes_copy's value-indexes allocation
  UR_SITE_ENC_COPY = 3, // fountain encoder's last-part index copy
};
#ifdef UR_ALLOC_FAIL_TEST
void ur_alloc_arm(int site, int at);
void ur_alloc_disarm(void);
unsigned ur_alloc_hits(void);
int ur_site_enter(int site);
void ur_site_leave(int prev);
#else
// Keep the reduction source identical in production while compiling the site
// markers completely away. The site constants remain available so the call
// sites do not grow test-only branches of their own.
#define ur_site_enter(site) ((void)(site), UR_SITE_NONE)
#define ur_site_leave(prev) ((void)(prev))
#endif

#endif // UR_UTILS_H
