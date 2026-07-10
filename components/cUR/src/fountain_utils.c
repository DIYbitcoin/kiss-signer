//
// fountain_utils.c
//
// Copyright © 2025 Krux Contributors
// Licensed under the "BSD-2-Clause Plus Patent License"
//
// Utility functions for fountain code operations.
//
// This is an independent implementation written using
// foundation-ur-py as a reference for testing and validation.
//

#include "fountain_utils.h"
#include "sha256/sha256_compat.h"
#include "utils.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void compute_sha256(const uint8_t *input, size_t len,
                           uint8_t output[32]) {
  ur_sha256(input, len, output);
}

static uint64_t rotl(const uint64_t x, int k) {
  return (x << k) | (x >> (64 - k));
}

void prng_init_from_bytes(prng_state_t *prng, const uint8_t *seed,
                          size_t seed_len) {
  if (!prng || !seed)
    return;

  uint8_t hash[32];
  compute_sha256(seed, seed_len, hash);

  for (int i = 0; i < 4; i++) {
    prng->state[i] = 0;
    for (int j = 0; j < 8; j++) {
      prng->state[i] <<= 8;
      prng->state[i] |= hash[i * 8 + j];
    }
  }
}

static uint64_t prng_next_uint64(prng_state_t *prng) {
  const uint64_t result = rotl(prng->state[1] * 5, 7) * 9;
  const uint64_t t = prng->state[1] << 17;

  prng->state[2] ^= prng->state[0];
  prng->state[3] ^= prng->state[1];
  prng->state[1] ^= prng->state[2];
  prng->state[0] ^= prng->state[3];

  prng->state[2] ^= t;
  prng->state[3] = rotl(prng->state[3], 45);

  return result;
}

uint32_t prng_next_int(prng_state_t *prng, uint32_t min, uint32_t max) {
  if (!prng || min > max)
    return min;

  double range = (double)(max - min + 1);
  double rand_val = prng_next_double(prng);
  uint64_t result = (uint64_t)(rand_val * range + min);

  return (uint32_t)(result & 0xFFFFFFFFULL);
}

double prng_next_double(prng_state_t *prng) {
  if (!prng)
    return 0.0;

  uint64_t val = prng_next_uint64(prng);
  return (double)val / (double)(0xFFFFFFFFFFFFFFFFULL + 1.0);
}

bool random_sampler_init(random_sampler_t *sampler, double *probs,
                         size_t count) {
  if (!sampler || !probs || count == 0)
    return false;

  double total = 0.0;
  for (size_t i = 0; i < count; i++) {
    if (probs[i] <= 0.0)
      return false;
    total += probs[i];
  }
  if (total <= 0.0)
    return false;

  sampler->probs = safe_malloc(count * sizeof(double));
  sampler->aliases = safe_malloc(count * sizeof(int));
  if (!sampler->probs || !sampler->aliases) {
    random_sampler_free(sampler);
    return false;
  }
  sampler->count = count;

  double *P = safe_malloc(count * sizeof(double));
  if (!P) {
    random_sampler_free(sampler);
    return false;
  }

  for (size_t i = 0; i < count; i++) {
    P[i] = (probs[i] * count) / total;
  }

  int *small = safe_malloc(count * sizeof(int));
  int *large = safe_malloc(count * sizeof(int));
  if (!small || !large) {
    free(P);
    free(small);
    free(large);
    random_sampler_free(sampler);
    return false;
  }

  size_t small_size = 0, large_size = 0;

  for (int i = (int)count - 1; i >= 0; i--) {
    if (P[i] < 1.0) {
      small[small_size++] = i;
    } else {
      large[large_size++] = i;
    }
  }

  while (small_size > 0 && large_size > 0) {
    int a = small[--small_size];
    int g = large[--large_size];

    sampler->probs[a] = P[a];
    sampler->aliases[a] = g;
    P[g] = P[g] + P[a] - 1.0;

    if (P[g] < 1.0) {
      small[small_size++] = g;
    } else {
      large[large_size++] = g;
    }
  }

  while (large_size > 0) {
    sampler->probs[large[--large_size]] = 1.0;
  }
  while (small_size > 0) {
    sampler->probs[small[--small_size]] = 1.0;
  }

  free(P);
  free(small);
  free(large);
  return true;
}

void random_sampler_free(random_sampler_t *sampler) {
  if (!sampler)
    return;
  safe_free(sampler->probs);
  safe_free(sampler->aliases);
  sampler->count = 0;
}

int random_sampler_next(random_sampler_t *sampler, prng_state_t *rng) {
  if (!sampler || !sampler->probs || !sampler->aliases || sampler->count == 0)
    return 0;

  double r1 = prng_next_double(rng);
  double r2 = prng_next_double(rng);
  int i = (int)((double)sampler->count * r1);

  if (i >= (int)sampler->count)
    i = (int)sampler->count - 1;

  return (r2 < sampler->probs[i]) ? i : sampler->aliases[i];
}

static size_t choose_degree(size_t seq_len, prng_state_t *prng,
                            random_sampler_t *cached_sampler) {
  if (seq_len == 0)
    return 1;

  if (cached_sampler && cached_sampler->probs) {
    int degree_index = random_sampler_next(cached_sampler, prng);
    return (size_t)degree_index + 1;
  }

  double *degree_probs = safe_malloc(seq_len * sizeof(double));
  if (!degree_probs)
    return 1;

  for (size_t i = 0; i < seq_len; i++) {
    degree_probs[i] = 1.0 / (i + 1);
  }

  random_sampler_t sampler = {0};
  if (!random_sampler_init(&sampler, degree_probs, seq_len)) {
    free(degree_probs);
    return 1;
  }

  int degree_index = random_sampler_next(&sampler, prng);
  size_t degree = degree_index + 1;

  random_sampler_free(&sampler);
  free(degree_probs);

  return degree;
}

static bool choose_fragments_internal(uint32_t seq_num, size_t seq_len,
                                      uint32_t checksum, part_indexes_t *result,
                                      random_sampler_t *cached_sampler) {
  if (!result || seq_len == 0)
    return false;

  part_indexes_clear(result);

  if (seq_num <= seq_len) {
    return part_indexes_add(result, seq_num - 1);
  }

  uint8_t seed[8];
  seed[0] = (seq_num >> 24) & 0xff;
  seed[1] = (seq_num >> 16) & 0xff;
  seed[2] = (seq_num >> 8) & 0xff;
  seed[3] = seq_num & 0xff;
  seed[4] = (checksum >> 24) & 0xff;
  seed[5] = (checksum >> 16) & 0xff;
  seed[6] = (checksum >> 8) & 0xff;
  seed[7] = checksum & 0xff;

  prng_state_t rng;
  prng_init_from_bytes(&rng, seed, 8);

  size_t degree = choose_degree(seq_len, &rng, cached_sampler);

  size_t *remaining_indexes = safe_malloc(seq_len * sizeof(size_t));
  if (!remaining_indexes)
    return false;

  for (size_t i = 0; i < seq_len; i++) {
    remaining_indexes[i] = i;
  }

  // Only the first `degree` drawn values matter. Stop after `degree`
  // draws instead of shuffling the whole sequence. The shift-based
  // removal is preserved to keep the PRNG call pattern identical to the
  // Python reference implementation.
  size_t remaining_count = seq_len;
  size_t draws = (degree < seq_len) ? degree : seq_len;
  for (size_t i = 0; i < draws && remaining_count > 0; i++) {
    uint32_t idx = prng_next_int(&rng, 0, remaining_count - 1);
    if (!part_indexes_add(result, remaining_indexes[idx])) {
      free(remaining_indexes);
      return false;
    }
    for (size_t j = idx; j < remaining_count - 1; j++) {
      remaining_indexes[j] = remaining_indexes[j + 1];
    }
    remaining_count--;
  }

  free(remaining_indexes);
  return true;
}

bool choose_fragments(uint32_t seq_num, size_t seq_len, uint32_t checksum,
                      part_indexes_t *result) {
  return choose_fragments_internal(seq_num, seq_len, checksum, result, NULL);
}

bool choose_fragments_cached(uint32_t seq_num, size_t seq_len,
                             uint32_t checksum, part_indexes_t *result,
                             random_sampler_t *cached_sampler) {
  return choose_fragments_internal(seq_num, seq_len, checksum, result,
                                   cached_sampler);
}

bool part_indexes_is_strict_subset(const part_indexes_t *a,
                                   const part_indexes_t *b) {
  if (!a || !b)
    return false;

  if (a->count >= b->count)
    return false;

  size_t i = 0, j = 0;
  while (i < a->count && j < b->count) {
    if (a->indexes[i] == b->indexes[j]) {
      i++;
      j++;
    } else if (a->indexes[i] > b->indexes[j]) {
      j++;
    } else {
      return false;
    }
  }

  return i == a->count;
}

static bool part_indexes_append_sorted(part_indexes_t *indexes, size_t value) {
  if (indexes->count >= indexes->capacity) {
    size_t new_capacity = indexes->capacity == 0 ? 4 : indexes->capacity * 2;
    size_t *new_idx =
        safe_realloc(indexes->indexes, new_capacity * sizeof(size_t));
    if (!new_idx)
      return false;
    indexes->indexes = new_idx;
    indexes->capacity = new_capacity;
  }
  indexes->indexes[indexes->count++] = value;
  return true;
}

bool part_indexes_difference(const part_indexes_t *a, const part_indexes_t *b,
                             part_indexes_t *result) {
  if (!a || !b || !result)
    return false;

  part_indexes_clear(result);

  size_t i = 0, j = 0;
  while (i < a->count) {
    if (j >= b->count || a->indexes[i] < b->indexes[j]) {
      if (!part_indexes_append_sorted(result, a->indexes[i])) {
        return false;
      }
      i++;
    } else if (a->indexes[i] == b->indexes[j]) {
      i++;
      j++;
    } else {
      j++;
    }
  }

  return true;
}

bool part_indexes_equal(const part_indexes_t *a, const part_indexes_t *b) {
  if (!a || !b)
    return false;

  if (a->count != b->count)
    return false;

  for (size_t i = 0; i < a->count; i++) {
    if (a->indexes[i] != b->indexes[i]) {
      return false;
    }
  }

  return true;
}

bool part_indexes_copy(const part_indexes_t *src, part_indexes_t *dst) {
  if (!src || !dst)
    return false;

  part_indexes_clear(dst);

  if (src->count == 0)
    return true;

  if (dst->capacity < src->count) {
    size_t *new_idx = safe_realloc(dst->indexes, src->count * sizeof(size_t));
    if (!new_idx)
      return false;
    dst->indexes = new_idx;
    dst->capacity = src->count;
  }

  memcpy(dst->indexes, src->indexes, src->count * sizeof(size_t));
  dst->count = src->count;
  return true;
}

bool join_fragments(uint8_t **fragments, size_t *fragment_lens,
                    size_t fragment_count, size_t message_len,
                    uint8_t *result) {
  if (!fragments || !fragment_lens || !result || fragment_count == 0)
    return false;

  size_t offset = 0;
  for (size_t i = 0; i < fragment_count; i++) {
    if (fragments[i] && fragment_lens[i] > 0) {
      size_t copy_len = fragment_lens[i];
      if (offset + copy_len > message_len)
        copy_len = message_len - offset;
      memcpy(result + offset, fragments[i], copy_len);
      offset += copy_len;
      if (offset >= message_len)
        break;
    }
  }

  return true;
}

#ifdef ENABLE_CROSS_REDUCTION
bool part_indexes_have_intersection(const part_indexes_t *a,
                                    const part_indexes_t *b) {
  if (!a || !b || a->count == 0 || b->count == 0)
    return false;

  size_t i = 0, j = 0;
  while (i < a->count && j < b->count) {
    if (a->indexes[i] == b->indexes[j]) {
      return true;
    } else if (a->indexes[i] < b->indexes[j]) {
      i++;
    } else {
      j++;
    }
  }
  return false;
}

bool part_indexes_symmetric_difference(const part_indexes_t *a,
                                       const part_indexes_t *b,
                                       part_indexes_t *result) {
  if (!a || !b || !result)
    return false;

  part_indexes_clear(result);

  size_t i = 0, j = 0;
  while (i < a->count && j < b->count) {
    if (a->indexes[i] < b->indexes[j]) {
      if (!part_indexes_append_sorted(result, a->indexes[i]))
        return false;
      i++;
    } else if (a->indexes[i] > b->indexes[j]) {
      if (!part_indexes_append_sorted(result, b->indexes[j]))
        return false;
      j++;
    } else {
      i++;
      j++;
    }
  }

  while (i < a->count) {
    if (!part_indexes_append_sorted(result, a->indexes[i]))
      return false;
    i++;
  }

  while (j < b->count) {
    if (!part_indexes_append_sorted(result, b->indexes[j]))
      return false;
    j++;
  }

  return true;
}
#endif // ENABLE_CROSS_REDUCTION
