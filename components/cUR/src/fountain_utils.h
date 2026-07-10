#ifndef FOUNTAIN_UTILS_H
#define FOUNTAIN_UTILS_H

#include "fountain_types.h"

// Enable cross-reduction between mixed parts (slower but may use fewer
// fragments). Uncomment to enable:
// #define ENABLE_CROSS_REDUCTION
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Simple PRNG for fountain coding (Xoshiro256**)
 */
typedef struct {
  uint64_t state[4];
} prng_state_t;

/**
 * Random sampler for degree selection using alias method
 */
typedef struct {
  double *probs;
  int *aliases;
  size_t count;
} random_sampler_t;

/**
 * Initialize PRNG with seed using SHA256 (matching Python behavior)
 * @param prng PRNG state
 * @param seed Seed bytes
 * @param seed_len Length of seed
 */
void prng_init_from_bytes(prng_state_t *prng, const uint8_t *seed,
                          size_t seed_len);

/**
 * Generate next random integer in range [min, max]
 * @param prng PRNG state
 * @param min Minimum value (inclusive)
 * @param max Maximum value (inclusive)
 * @return Random integer
 */
uint32_t prng_next_int(prng_state_t *prng, uint32_t min, uint32_t max);

/**
 * Generate next random double in range [0.0, 1.0)
 * @param prng PRNG state
 * @return Random double
 */
double prng_next_double(prng_state_t *prng);

/**
 * Initialize random sampler with probabilities
 * @param sampler Random sampler instance
 * @param probs Array of probabilities
 * @param count Number of probabilities
 * @return true on success
 */
bool random_sampler_init(random_sampler_t *sampler, double *probs,
                         size_t count);

/**
 * Free random sampler resources
 * @param sampler Random sampler instance
 */
void random_sampler_free(random_sampler_t *sampler);

/**
 * Get next sample from random sampler
 * @param sampler Random sampler instance
 * @param rng PRNG instance
 * @return Selected index
 */
int random_sampler_next(random_sampler_t *sampler, prng_state_t *rng);

/**
 * Choose fragments for a fountain encoder part
 * @param seq_num Sequence number
 * @param seq_len Total sequence length
 * @param checksum Message checksum
 * @param result Output part indexes
 * @return true on success
 */
bool choose_fragments(uint32_t seq_num, size_t seq_len, uint32_t checksum,
                      part_indexes_t *result);

/**
 * Choose fragments with a pre-initialized degree sampler (avoids repeated
 * sampler allocation)
 * @param seq_num Sequence number
 * @param seq_len Total sequence length
 * @param checksum Message checksum
 * @param result Output part indexes
 * @param cached_sampler Pre-initialized sampler (or NULL to create one)
 * @return true on success
 */
bool choose_fragments_cached(uint32_t seq_num, size_t seq_len,
                             uint32_t checksum, part_indexes_t *result,
                             random_sampler_t *cached_sampler);

/**
 * Check if part_indexes_a is strict subset of part_indexes_b
 * @param a First set
 * @param b Second set
 * @return true if a is strict subset of b
 */
bool part_indexes_is_strict_subset(const part_indexes_t *a,
                                   const part_indexes_t *b);

/**
 * Set difference: result = a - b
 * @param a First set
 * @param b Second set
 * @param result Output set (a - b)
 * @return true on success
 */
bool part_indexes_difference(const part_indexes_t *a, const part_indexes_t *b,
                             part_indexes_t *result);

/**
 * Check if two part_indexes are equal
 * @param a First set
 * @param b Second set
 * @return true if equal
 */
bool part_indexes_equal(const part_indexes_t *a, const part_indexes_t *b);

/**
 * Copy part_indexes
 * @param src Source
 * @param dst Destination
 * @return true on success
 */
bool part_indexes_copy(const part_indexes_t *src, part_indexes_t *dst);

#ifdef ENABLE_CROSS_REDUCTION
/**
 * Check if two part_indexes have any intersection
 * @param a First set
 * @param b Second set
 * @return true if they share any indexes
 */
bool part_indexes_have_intersection(const part_indexes_t *a,
                                    const part_indexes_t *b);

/**
 * Calculate symmetric difference between two part_indexes sets
 * @param a First set
 * @param b Second set
 * @param result Output set (A ⊕ B)
 * @return true on success
 */
bool part_indexes_symmetric_difference(const part_indexes_t *a,
                                       const part_indexes_t *b,
                                       part_indexes_t *result);
#endif // ENABLE_CROSS_REDUCTION

/**
 * Join fragments into a single message, taking only message_len bytes
 * @param fragments Array of fragment pointers
 * @param fragment_lens Array of fragment lengths
 * @param fragment_count Number of fragments
 * @param message_len Expected message length
 * @param result Output buffer (allocated by caller)
 * @return true on success
 */
bool join_fragments(uint8_t **fragments, size_t *fragment_lens,
                    size_t fragment_count, size_t message_len, uint8_t *result);

#endif // FOUNTAIN_UTILS_H
