//
// fountain_decoder.c
//
// Copyright © 2025 Krux Contributors
// Licensed under the "BSD-2-Clause Plus Patent License"
//
// Implementation of fountain code decoder for multi-part data reassembly.
// Based on the specification:
// https://github.com/BlockchainCommons/Research/blob/master/papers/bcr-2020-006-urtypes.md
//
// This is an independent implementation written using
// foundation-ur-py as a reference for testing and validation.
//

#include "fountain_decoder.h"
#include "crc32.h"
#include "fountain_utils.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>

// Forward declarations
typedef struct mixed_parts_hash mixed_parts_hash_t;

// Lightweight hash set for duplicate detection (stores only hashes)
typedef struct {
  uint32_t *hashes;
  size_t count;
  size_t capacity;
} hash_set_t;

// Queue for processing parts
typedef struct {
  decoder_part_t *parts;
  size_t front;
  size_t rear;
  size_t count;
  size_t capacity;
} part_queue_t;

// Fountain decoder structure
struct fountain_decoder {
  part_indexes_t received_part_indexes;
  part_indexes_t *last_part_indexes;
  size_t processed_parts_count;
  fountain_decoder_result_t *result;
  part_indexes_t *expected_part_indexes;
  size_t expected_fragment_len;
  size_t expected_message_len;
  uint32_t expected_checksum;

  // Simple parts storage (key: single index, value: data)
  struct {
    size_t *keys;
    decoder_part_t *values;
    size_t *value_lens;
    size_t count;
    size_t capacity;
  } simple_parts;

  // Hash-based mixed parts storage
  mixed_parts_hash_t *mixed_parts_hash;

  // Lightweight duplicate detection (stores only hashes, not full parts)
  hash_set_t received_fragments_hashes;

  // Processing queue
  part_queue_t queue;

  // Cached degree sampler (avoids repeated allocation per fountain fragment)
  random_sampler_t degree_sampler;

  // Duplicate detection: store last fragment sequence number
  uint32_t last_fragment_seq_num;
  bool has_received_fragment;

#ifdef DEBUG_STATS
  // Statistics for resource tracking
  size_t maximum_mixed_parts;
  size_t mixed_from_fragments; // Mixed parts directly from received fragments
  size_t mixed_from_reduction; // Mixed parts created by reduce_mixed_by
  size_t mixed_from_cross_reduction; // Mixed parts created by
                                     // reduce_mixed_against_mixed
  size_t mixed_parts_useful;         // Mixed parts that led to simple parts
#endif
};

// Configuration constants
#define QUEUE_INITIAL_CAPACITY 8
#define SIMPLE_PARTS_INITIAL_CAPACITY 4
#define INDEXES_INITIAL_CAPACITY 4
#define HASH_MIN_CAPACITY 64
#define HASH_CAPACITY_MULTIPLIER 1
#define MAX_MIXED_PARTS 256 // Limit mixed parts to prevent memory explosion
#define MAX_DUPLICATE_TRACKING 512 // Limit duplicate tracking set size

#ifdef ENABLE_CROSS_REDUCTION
#define CROSS_REDUCTION_MAX_ITERATIONS 7
#endif
#define FNV1A_OFFSET_BASIS 2166136261u
#define FNV1A_PRIME 16777619u

// Hash table for mixed parts - key is the set of indexes
typedef struct hash_entry {
  part_indexes_t key;
  decoder_part_t value;
  size_t key_hash; // Cached hash for fast collision filtering
  struct hash_entry *next;
} hash_entry_t;

struct mixed_parts_hash {
  hash_entry_t **buckets;
  size_t count;
  size_t capacity;
};

// Hash function for part indexes using FNV-1a algorithm
static size_t hash_indexes(const part_indexes_t *indexes) {
  if (!indexes || indexes->count == 0)
    return 0;

  size_t hash = FNV1A_OFFSET_BASIS;
  for (size_t i = 0; i < indexes->count; i++) {
    hash ^= indexes->indexes[i];
    hash *= FNV1A_PRIME;
  }
  return hash;
}

// Hash set operations for lightweight duplicate detection

// Initialize hash set
static bool hash_set_init(hash_set_t *set, size_t capacity) {
  if (!set || capacity == 0)
    return false;

  set->hashes = safe_malloc(capacity * sizeof(uint32_t));
  if (!set->hashes)
    return false;

  set->count = 0;
  set->capacity = capacity;
  return true;
}

// Free hash set
static void hash_set_free(hash_set_t *set) {
  if (!set)
    return;

  safe_free(set->hashes);
  set->count = 0;
  set->capacity = 0;
}

// Check if hash set contains a hash using binary search (sorted array)
static bool hash_set_contains(const hash_set_t *set, uint32_t hash) {
  if (!set || !set->hashes || set->count == 0)
    return false;

  size_t lo = 0, hi = set->count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (set->hashes[mid] == hash)
      return true;
    if (set->hashes[mid] < hash)
      lo = mid + 1;
    else
      hi = mid;
  }
  return false;
}

// Add hash to set in sorted order (returns false if already exists or on error)
static bool hash_set_add(hash_set_t *set, uint32_t hash) {
  if (!set)
    return false;

  // Binary search for insertion point
  size_t lo = 0, hi = set->count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (set->hashes[mid] == hash)
      return false; // Already exists
    if (set->hashes[mid] < hash)
      lo = mid + 1;
    else
      hi = mid;
  }

  // Limit growth to prevent unbounded memory usage on embedded devices
  if (set->count >= MAX_DUPLICATE_TRACKING) {
    return false;
  }

  // Expand if needed (but respect MAX_DUPLICATE_TRACKING limit)
  if (set->count >= set->capacity) {
    size_t new_capacity = set->capacity * 2;
    if (new_capacity > MAX_DUPLICATE_TRACKING) {
      new_capacity = MAX_DUPLICATE_TRACKING;
    }
    uint32_t *new_hashes =
        safe_realloc(set->hashes, new_capacity * sizeof(uint32_t));
    if (!new_hashes)
      return false;

    set->hashes = new_hashes;
    set->capacity = new_capacity;
  }

  // Shift elements to maintain sorted order
  for (size_t i = set->count; i > lo; i--) {
    set->hashes[i] = set->hashes[i - 1];
  }
  set->hashes[lo] = hash;
  set->count++;
  return true;
}

// Initialize hash table with dynamic capacity
static bool mixed_hash_init(mixed_parts_hash_t *hash, size_t capacity) {
  if (!hash || capacity == 0)
    return false;

  hash->buckets = calloc(capacity, sizeof(hash_entry_t *));
  if (!hash->buckets)
    return false;

  hash->count = 0;
  hash->capacity = capacity;
  return true;
}

// Free hash table
static void mixed_hash_free(mixed_parts_hash_t *hash) {
  if (!hash || !hash->buckets)
    return;

  for (size_t i = 0; i < hash->capacity; i++) {
    hash_entry_t *entry = hash->buckets[i];
    while (entry) {
      hash_entry_t *next = entry->next;
      decoder_part_free(&entry->value);
      if (entry->key.indexes) {
        free(entry->key.indexes);
      }
      free(entry);
      entry = next;
    }
  }

  safe_free(hash->buckets);
  hash->count = 0;
}

// Add or update entry in hash table
static bool mixed_hash_put(mixed_parts_hash_t *hash, const part_indexes_t *key,
                           const decoder_part_t *value) {
  if (!hash || !hash->buckets || !key || !value || hash->capacity == 0)
    return false;

  size_t key_hash = hash_indexes(key);
  size_t bucket = key_hash % hash->capacity;

  // Check if already exists
  hash_entry_t *entry = hash->buckets[bucket];
  while (entry) {
    // Fast path: compare cached hash first
    if (entry->key_hash == key_hash && part_indexes_equal(&entry->key, key)) {
      // Already exists, don't add duplicate
      return false;
    }
    entry = entry->next;
  }

  hash_entry_t *new_entry = calloc(1, sizeof(hash_entry_t));
  if (!new_entry)
    return false;

  new_entry->key_hash = key_hash; // Cache the hash

  if (!part_indexes_copy(key, &new_entry->key)) {
    free(new_entry);
    return false;
  }

  if (!decoder_part_copy(value, &new_entry->value)) {
    if (new_entry->key.indexes) {
      free(new_entry->key.indexes);
    }
    free(new_entry);
    return false;
  }

  new_entry->next = hash->buckets[bucket];
  hash->buckets[bucket] = new_entry;
  hash->count++;

  return true;
}

static bool queue_init(part_queue_t *queue, size_t capacity) {
  if (!queue || capacity == 0)
    return false;

  queue->parts = safe_malloc(capacity * sizeof(decoder_part_t));
  if (!queue->parts)
    return false;

  queue->front = 0;
  queue->rear = 0;
  queue->count = 0;
  queue->capacity = capacity;

  return true;
}

static void queue_free(part_queue_t *queue) {
  if (!queue)
    return;

  if (queue->parts) {
    for (size_t i = 0; i < queue->count; i++) {
      size_t idx = (queue->front + i) % queue->capacity;
      decoder_part_free(&queue->parts[idx]);
    }
    free(queue->parts);
  }

  memset(queue, 0, sizeof(part_queue_t));
}

static void decoder_part_move(decoder_part_t *src, decoder_part_t *dst) {
  if (!src || !dst)
    return;

  decoder_part_free(dst);
  *dst = *src;
  *src = (decoder_part_t){0};
}

static bool queue_enqueue(part_queue_t *queue, decoder_part_t *part) {
  if (!queue || !part || queue->count >= queue->capacity)
    return false;

  decoder_part_move(part, &queue->parts[queue->rear]);
  queue->rear = (queue->rear + 1) % queue->capacity;
  queue->count++;

  return true;
}

static bool queue_dequeue(part_queue_t *queue, decoder_part_t *part) {
  if (!queue || !part || queue->count == 0)
    return false;

  decoder_part_move(&queue->parts[queue->front], part);
  queue->front = (queue->front + 1) % queue->capacity;
  queue->count--;

  return true;
}

static bool queue_is_empty(const part_queue_t *queue) {
  return !queue || queue->count == 0;
}

void decoder_part_free(decoder_part_t *part) {
  if (!part)
    return;

  safe_free(part->indexes.indexes);
  part->indexes.count = 0;
  part->indexes.capacity = 0;

  safe_free(part->data);
  part->data_len = 0;
}

bool decoder_part_copy(const decoder_part_t *src, decoder_part_t *dst) {
  if (!src || !dst)
    return false;

  decoder_part_free(dst);

  dst->indexes.indexes = NULL;
  dst->indexes.count = 0;
  dst->indexes.capacity = 0;

  if (!part_indexes_copy(&src->indexes, &dst->indexes)) {
    return false;
  }

  if (src->data && src->data_len > 0) {
    dst->data = safe_malloc_uninit(src->data_len);
    if (!dst->data) {
      safe_free(dst->indexes.indexes);
      dst->indexes.count = 0;
      dst->indexes.capacity = 0;
      return false;
    }
    memcpy(dst->data, src->data, src->data_len);
    dst->data_len = src->data_len;
  } else {
    dst->data = NULL;
    dst->data_len = 0;
  }

  return true;
}

part_indexes_t *part_indexes_new(void) {
  part_indexes_t *indexes = safe_malloc(sizeof(part_indexes_t));
  if (indexes) {
    indexes->indexes = NULL;
    indexes->count = 0;
    indexes->capacity = 0;
  }
  return indexes;
}

void part_indexes_free(part_indexes_t *indexes) {
  if (indexes) {
    if (indexes->indexes) {
      free(indexes->indexes);
    }
    free(indexes);
  }
}

bool part_indexes_add(part_indexes_t *indexes, size_t index) {
  if (!indexes)
    return false;

  // Binary search for insertion point (keeps array sorted)
  size_t left = 0, right = indexes->count;
  while (left < right) {
    size_t mid = left + (right - left) / 2;
    if (indexes->indexes[mid] == index) {
      return true; // Already exists
    }
    if (indexes->indexes[mid] < index) {
      left = mid + 1;
    } else {
      right = mid;
    }
  }
  // 'left' is now the insertion point

  if (indexes->count >= indexes->capacity) {
    size_t new_capacity = indexes->capacity == 0 ? INDEXES_INITIAL_CAPACITY
                                                 : indexes->capacity * 2;
    size_t *new_indexes =
        safe_realloc(indexes->indexes, sizeof(size_t) * new_capacity);
    if (!new_indexes)
      return false;

    indexes->indexes = new_indexes;
    indexes->capacity = new_capacity;
  }

  // Shift elements to make room for new index
  for (size_t i = indexes->count; i > left; i--) {
    indexes->indexes[i] = indexes->indexes[i - 1];
  }
  indexes->indexes[left] = index;
  indexes->count++;
  return true;
}

bool part_indexes_contains(const part_indexes_t *indexes, size_t index) {
  if (!indexes || indexes->count == 0)
    return false;

  // Binary search on sorted array - O(log n)
  size_t left = 0, right = indexes->count;
  while (left < right) {
    size_t mid = left + (right - left) / 2;
    if (indexes->indexes[mid] == index) {
      return true;
    }
    if (indexes->indexes[mid] < index) {
      left = mid + 1;
    } else {
      right = mid;
    }
  }
  return false;
}

void part_indexes_clear(part_indexes_t *indexes) {
  if (indexes) {
    indexes->count = 0;
  }
}

fountain_decoder_t *fountain_decoder_new(void) {
  fountain_decoder_t *decoder = safe_malloc(sizeof(fountain_decoder_t));
  if (!decoder)
    return NULL;

  if (!queue_init(&decoder->queue, QUEUE_INITIAL_CAPACITY)) {
    free(decoder);
    return NULL;
  }

  return decoder;
}

void fountain_decoder_free(fountain_decoder_t *decoder) {
  if (!decoder)
    return;

  if (decoder->received_part_indexes.indexes) {
    free(decoder->received_part_indexes.indexes);
  }

  if (decoder->last_part_indexes) {
    part_indexes_free(decoder->last_part_indexes);
  }

  if (decoder->expected_part_indexes) {
    part_indexes_free(decoder->expected_part_indexes);
  }

  if (decoder->result) {
    if (decoder->result->data) {
      free(decoder->result->data);
    }
    free(decoder->result);
  }

  if (decoder->simple_parts.keys)
    free(decoder->simple_parts.keys);
  if (decoder->simple_parts.value_lens)
    free(decoder->simple_parts.value_lens);
  if (decoder->simple_parts.values) {
    for (size_t i = 0; i < decoder->simple_parts.count; i++) {
      decoder_part_free(&decoder->simple_parts.values[i]);
    }
    free(decoder->simple_parts.values);
  }

  // Free hash table for mixed parts
  if (decoder->mixed_parts_hash) {
    mixed_hash_free(decoder->mixed_parts_hash);
    safe_free(decoder->mixed_parts_hash);
  }

  // Free hash set for duplicate detection
  hash_set_free(&decoder->received_fragments_hashes);

  // Free cached degree sampler
  random_sampler_free(&decoder->degree_sampler);

  queue_free(&decoder->queue);

  free(decoder);
}

static bool create_decoder_part_from_encoder_part(
    fountain_encoder_part_t *const encoder_part,
    decoder_part_t *const decoder_part, random_sampler_t *cached_sampler) {
  if (!encoder_part || !decoder_part) {
    return false;
  }

  *decoder_part = (decoder_part_t){0};

  if (!choose_fragments_cached(encoder_part->seq_num, encoder_part->seq_len,
                               encoder_part->checksum, &decoder_part->indexes,
                               cached_sampler)) {
    return false;
  }

  if (encoder_part->data && encoder_part->data_len > 0) {
    // Move data from encoder_part instead of copying
    decoder_part->data = encoder_part->data;
    decoder_part->data_len = encoder_part->data_len;
    encoder_part->data = NULL;
    encoder_part->data_len = 0;
  }

  return true;
}

typedef enum {
  MIXED_SOURCE_FRAGMENT,
  MIXED_SOURCE_REDUCTION,
  MIXED_SOURCE_CROSS_REDUCTION
} mixed_part_source_t;

static bool is_simple_part(const decoder_part_t *const part) {
  return part && part->indexes.count == 1;
}

static size_t get_part_index(const decoder_part_t *const part) {
  if (!part || part->indexes.count == 0)
    return 0;
  return part->indexes.indexes[0];
}

static bool add_simple_part(fountain_decoder_t *const decoder,
                            const decoder_part_t *const part) {
  if (!decoder || !part || !is_simple_part(part))
    return false;

  size_t index = get_part_index(part);

  for (size_t i = 0; i < decoder->simple_parts.count; i++) {
    if (decoder->simple_parts.keys[i] == index) {
      return true;
    }
  }

  if (decoder->simple_parts.count >= decoder->simple_parts.capacity) {
    size_t new_capacity = decoder->simple_parts.capacity == 0
                              ? SIMPLE_PARTS_INITIAL_CAPACITY
                              : decoder->simple_parts.capacity * 2;

    // Realloc one field at a time, committing each to the struct before the
    // next attempt. safe_realloc frees the old block on success, so if a
    // later realloc fails we must never leave the struct pointing at a
    // freed block.
    size_t *new_keys =
        safe_realloc(decoder->simple_parts.keys, sizeof(size_t) * new_capacity);
    if (!new_keys)
      return false;
    decoder->simple_parts.keys = new_keys;

    decoder_part_t *new_values = safe_realloc(
        decoder->simple_parts.values, sizeof(decoder_part_t) * new_capacity);
    if (!new_values)
      return false;
    decoder->simple_parts.values = new_values;

    size_t *new_lens = safe_realloc(decoder->simple_parts.value_lens,
                                    sizeof(size_t) * new_capacity);
    if (!new_lens)
      return false;
    decoder->simple_parts.value_lens = new_lens;

    for (size_t i = decoder->simple_parts.capacity; i < new_capacity; i++) {
      new_values[i].indexes.indexes = NULL;
      new_values[i].indexes.count = 0;
      new_values[i].indexes.capacity = 0;
      new_values[i].data = NULL;
      new_values[i].data_len = 0;
    }

    decoder->simple_parts.capacity = new_capacity;
  }

  decoder_part_t *stored_part =
      &decoder->simple_parts.values[decoder->simple_parts.count];
  if (!decoder_part_copy(part, stored_part)) {
    return false;
  }

  decoder->simple_parts.keys[decoder->simple_parts.count] = index;
  decoder->simple_parts.value_lens[decoder->simple_parts.count] =
      part->data_len;
  decoder->simple_parts.count++;

  return true;
}

static bool reduce_part_by_part(const decoder_part_t *const a,
                                const decoder_part_t *const b,
                                decoder_part_t *const result) {
  if (!a || !b || !result)
    return false;

  // Only reduce if b's indexes are a strict subset of a's indexes
  if (!part_indexes_is_strict_subset(&b->indexes, &a->indexes)) {
    return decoder_part_copy(a, result);
  }

  *result = (decoder_part_t){0};

  if (!part_indexes_difference(&a->indexes, &b->indexes, &result->indexes)) {
    return false;
  }

  result->data = safe_malloc_uninit(a->data_len);
  if (!result->data) {
    if (result->indexes.indexes) {
      free(result->indexes.indexes);
    }
    return false;
  }

  result->data_len = a->data_len;
  for (size_t i = 0; i < a->data_len; i++) {
    result->data[i] = a->data[i] ^ b->data[i];
  }
  return true;
}

static bool add_mixed_part(fountain_decoder_t *const decoder,
                           const decoder_part_t *const part,
                           const mixed_part_source_t source) {
  if (!decoder || !part || is_simple_part(part) || !decoder->mixed_parts_hash)
    return false;

  // Limit mixed parts to prevent memory explosion on embedded devices
  // When limit is reached, skip adding new mixed parts but continue processing
  if (decoder->mixed_parts_hash->count >= MAX_MIXED_PARTS) {
    return false; // Limit reached, skip adding this mixed part
  }

  // Try to add to hash table (which automatically checks for duplicates)
  if (!mixed_hash_put(decoder->mixed_parts_hash, &part->indexes, part)) {
    return false; // Duplicate or error
  }

#ifdef DEBUG_STATS
  // Track the source of this mixed part
  switch (source) {
  case MIXED_SOURCE_FRAGMENT:
    decoder->mixed_from_fragments++;
    break;
  case MIXED_SOURCE_REDUCTION:
    decoder->mixed_from_reduction++;
    break;
  case MIXED_SOURCE_CROSS_REDUCTION:
    decoder->mixed_from_cross_reduction++;
    break;
  }

  // Update maximum tracking
  if (decoder->mixed_parts_hash->count > decoder->maximum_mixed_parts) {
    decoder->maximum_mixed_parts = decoder->mixed_parts_hash->count;
  }
#endif
  (void)source;

  return true;
}

static void fountain_decoder_clear_initialization(fountain_decoder_t *decoder) {
  if (!decoder)
    return;

  if (decoder->expected_part_indexes) {
    part_indexes_free(decoder->expected_part_indexes);
    decoder->expected_part_indexes = NULL;
  }
  if (decoder->mixed_parts_hash) {
    mixed_hash_free(decoder->mixed_parts_hash);
    safe_free(decoder->mixed_parts_hash);
  }
  hash_set_free(&decoder->received_fragments_hashes);
  random_sampler_free(&decoder->degree_sampler);

  decoder->expected_fragment_len = 0;
  decoder->expected_message_len = 0;
  decoder->expected_checksum = 0;
}

static void reduce_mixed_by(fountain_decoder_t *const decoder,
                            const decoder_part_t *const part) {
  if (!decoder || !part || !decoder->mixed_parts_hash ||
      decoder->mixed_parts_hash->count == 0)
    return;

  mixed_parts_hash_t *hash = decoder->mixed_parts_hash;

  // Entries that reduce to a new bucket are unlinked and threaded onto
  // rehash_list via entry->next, then reinserted after the walk. This
  // avoids the per-call malloc for a temp pointer array.
  hash_entry_t *rehash_list = NULL;

  for (size_t i = 0; i < hash->capacity; i++) {
    hash_entry_t *entry = hash->buckets[i];
    hash_entry_t *prev = NULL;

    while (entry) {
      hash_entry_t *next = entry->next;

      if (!part_indexes_is_strict_subset(&part->indexes, &entry->key)) {
        prev = entry;
        entry = next;
        continue;
      }

      part_indexes_t new_indexes = {0};
      if (!part_indexes_difference(&entry->key, &part->indexes, &new_indexes)) {
        prev = entry;
        entry = next;
        continue;
      }

      // XOR the data in-place.
      for (size_t j = 0; j < entry->value.data_len && j < part->data_len; j++) {
        entry->value.data[j] ^= part->data[j];
      }

      free(entry->key.indexes);
      entry->key = new_indexes;
      entry->key_hash = hash_indexes(&entry->key);

      free(entry->value.indexes.indexes);
      entry->value.indexes = (part_indexes_t){0};
      part_indexes_copy(&new_indexes, &entry->value.indexes);

      if (is_simple_part(&entry->value)) {
#ifdef DEBUG_STATS
        decoder->mixed_parts_useful++;
#endif
        size_t fragment_idx = get_part_index(&entry->value);
        if (!part_indexes_contains(&decoder->received_part_indexes,
                                   fragment_idx)) {
          queue_enqueue(&decoder->queue, &entry->value);
        }

        // Unlink from this bucket and free.
        if (prev) {
          prev->next = next;
        } else {
          hash->buckets[i] = next;
        }
        decoder_part_free(&entry->value);
        free(entry->key.indexes);
        free(entry);
        hash->count--;
        entry = next;
        continue;
      }

      size_t new_bucket = entry->key_hash % hash->capacity;
      if (new_bucket != i) {
        // Unlink and stash for reinsertion below.
        if (prev) {
          prev->next = next;
        } else {
          hash->buckets[i] = next;
        }
        entry->next = rehash_list;
        rehash_list = entry;
        entry = next;
        continue;
      }

      prev = entry;
      entry = next;
    }
  }

  // Reinsert stashed entries at the head of their new bucket. A revisit
  // during this call isn't possible — the outer bucket walk has already
  // finished by the time we get here.
  while (rehash_list) {
    hash_entry_t *e = rehash_list;
    rehash_list = rehash_list->next;
    size_t b = e->key_hash % hash->capacity;
    e->next = hash->buckets[b];
    hash->buckets[b] = e;
  }

#ifdef DEBUG_STATS
  if (hash->count > decoder->maximum_mixed_parts) {
    decoder->maximum_mixed_parts = hash->count;
  }
#endif
}

#ifdef ENABLE_CROSS_REDUCTION
static bool create_symmetric_diff(const decoder_part_t *const a,
                                  const decoder_part_t *const b,
                                  decoder_part_t *const result) {
  if (!a || !b || !result || a->data_len != b->data_len)
    return false;

  *result = (decoder_part_t){0};

  if (!part_indexes_symmetric_difference(&a->indexes, &b->indexes,
                                         &result->indexes)) {
    return false;
  }

  if (result->indexes.count == 0) {
    safe_free(result->indexes.indexes);
    return false;
  }

  result->data = safe_malloc_uninit(a->data_len);
  if (!result->data) {
    safe_free(result->indexes.indexes);
    result->indexes.count = 0;
    result->indexes.capacity = 0;
    return false;
  }

  result->data_len = a->data_len;
  for (size_t i = 0; i < a->data_len; i++) {
    result->data[i] = a->data[i] ^ b->data[i];
  }

  return true;
}

static void gaussian_reduce_with_new_part(fountain_decoder_t *const decoder,
                                          const decoder_part_t *const pivot);

static void reduce_mixed_against_mixed(fountain_decoder_t *const decoder) {
  if (!decoder || !decoder->mixed_parts_hash ||
      decoder->mixed_parts_hash->count < 2) {
    return;
  }

  bool made_progress = true;
  int iteration = 0;

  while (made_progress && iteration < CROSS_REDUCTION_MAX_ITERATIONS) {
    made_progress = false;
    iteration++;

    // Collect all current hash entries into an array for iteration
    size_t entry_count = decoder->mixed_parts_hash->count;
    hash_entry_t **entries = safe_malloc(sizeof(hash_entry_t *) * entry_count);
    if (!entries)
      return;

    size_t idx = 0;
    for (size_t b = 0;
         b < decoder->mixed_parts_hash->capacity && idx < entry_count; b++) {
      hash_entry_t *entry = decoder->mixed_parts_hash->buckets[b];
      while (entry && idx < entry_count) {
        entries[idx++] = entry;
        entry = entry->next;
      }
    }

    for (size_t i = 0; i < entry_count && !made_progress; i++) {
      for (size_t offset = 1;
           offset <= entry_count && (i + offset) < entry_count; offset++) {
        size_t j = i + offset;

        if (part_indexes_have_intersection(&entries[i]->key,
                                           &entries[j]->key)) {

          decoder_part_t new_part = {0};

          if (create_symmetric_diff(&entries[i]->value, &entries[j]->value,
                                    &new_part)) {

            bool is_simpler = new_part.indexes.count < entries[i]->key.count ||
                              new_part.indexes.count < entries[j]->key.count;

            if (!is_simpler) {
              decoder_part_free(&new_part);
              continue;
            }

            // Hash table automatically checks for duplicates in add_mixed_part
            if (new_part.indexes.count > 0) {
              if (is_simple_part(&new_part)) {
#ifdef DEBUG_STATS
                decoder->mixed_parts_useful++; // Cross-reduction led to simple
                                               // part!
#endif
                size_t fragment_idx = get_part_index(&new_part);
                if (!part_indexes_contains(&decoder->received_part_indexes,
                                           fragment_idx)) {
                  queue_enqueue(&decoder->queue, &new_part);
                  made_progress = true;
                }
                decoder_part_free(&new_part);
                break;
              } else {
                if (add_mixed_part(decoder, &new_part,
                                   MIXED_SOURCE_CROSS_REDUCTION)) {
                  made_progress = true;
                  gaussian_reduce_with_new_part(decoder, &new_part);
                }
                decoder_part_free(&new_part);
              }
              break;
            } else {
              decoder_part_free(&new_part);
            }
          }
        }
      }
    }

    free(entries);
  }
}

static void gaussian_reduce_with_new_part(fountain_decoder_t *const decoder,
                                          const decoder_part_t *const pivot) {
  if (!decoder || !pivot || is_simple_part(pivot) || !decoder->mixed_parts_hash)
    return;

  // We iterate over buckets directly to avoid stale pointer issues.
  // When we remove an entry and add a new one, we need to be careful:
  // - The new entry goes to the HEAD of its bucket
  // - We only continue with 'next' which was valid before the modification
  for (size_t b = 0; b < decoder->mixed_parts_hash->capacity; b++) {
    hash_entry_t *entry = decoder->mixed_parts_hash->buckets[b];
    hash_entry_t *prev = NULL;

    while (entry) {
      // Save next pointer BEFORE any potential modification
      hash_entry_t *next = entry->next;

      // Skip if this is the pivot itself
      if (part_indexes_equal(&entry->key, &pivot->indexes)) {
        prev = entry;
        entry = next;
        continue;
      }

      if (part_indexes_is_strict_subset(&pivot->indexes, &entry->key)) {
        decoder_part_t reduced = {0};

        if (reduce_part_by_part(&entry->value, pivot, &reduced)) {
          // Unlink entry from bucket chain BEFORE freeing
          if (prev) {
            prev->next = next;
          } else {
            decoder->mixed_parts_hash->buckets[b] = next;
          }

          // Free the entry
          decoder_part_free(&entry->value);
          if (entry->key.indexes) {
            free(entry->key.indexes);
          }
          free(entry);
          decoder->mixed_parts_hash->count--;

          // Handle reduced part
          if (is_simple_part(&reduced)) {
            size_t fragment_idx = get_part_index(&reduced);
            if (!part_indexes_contains(&decoder->received_part_indexes,
                                       fragment_idx)) {
              queue_enqueue(&decoder->queue, &reduced);
            }
          } else {
            // Add reduced mixed part back
            add_mixed_part(decoder, &reduced, MIXED_SOURCE_REDUCTION);
          }
          decoder_part_free(&reduced);

          // Don't update prev - it still points to the previous valid entry
          // (or is NULL if we removed the head)
          entry = next;
          continue;
        }
      }

      prev = entry;
      entry = next;
    }
  }
}
#endif // ENABLE_CROSS_REDUCTION

static void process_simple_part(fountain_decoder_t *const decoder,
                                const decoder_part_t *const part) {
  if (!decoder || !part || !is_simple_part(part))
    return;

  size_t fragment_index = get_part_index(part);

  if (part_indexes_contains(&decoder->received_part_indexes, fragment_index)) {
    return;
  }

  if (!add_simple_part(decoder, part)) {
    return;
  }

  if (!part_indexes_add(&decoder->received_part_indexes, fragment_index)) {
    return;
  }

  if (decoder->expected_part_indexes &&
      part_indexes_equal(&decoder->received_part_indexes,
                         decoder->expected_part_indexes)) {

    size_t part_count = decoder->simple_parts.count;

    // Sort simple_parts by fragment index in place (paired insertion
    // sort across the three parallel arrays). part_count <= seq_len,
    // which is small in practice, so O(n²) is fine and saves the three
    // temporary arrays the earlier qsort-based path allocated.
    for (size_t i = 1; i < part_count; i++) {
      size_t key = decoder->simple_parts.keys[i];
      decoder_part_t val = decoder->simple_parts.values[i];
      size_t val_len = decoder->simple_parts.value_lens[i];
      size_t j = i;
      while (j > 0 && decoder->simple_parts.keys[j - 1] > key) {
        decoder->simple_parts.keys[j] = decoder->simple_parts.keys[j - 1];
        decoder->simple_parts.values[j] = decoder->simple_parts.values[j - 1];
        decoder->simple_parts.value_lens[j] =
            decoder->simple_parts.value_lens[j - 1];
        j--;
      }
      decoder->simple_parts.keys[j] = key;
      decoder->simple_parts.values[j] = val;
      decoder->simple_parts.value_lens[j] = val_len;
    }

    uint8_t *message = safe_malloc_uninit(decoder->expected_message_len);
    if (!message) {
      return;
    }

    // Inline join: same semantics as fountain_utils' join_fragments but
    // reads directly from the sorted simple_parts without a pointer array.
    {
      size_t offset = 0;
      for (size_t i = 0;
           i < part_count && offset < decoder->expected_message_len; i++) {
        decoder_part_t *p = &decoder->simple_parts.values[i];
        if (!p->data || p->data_len == 0)
          continue;
        size_t copy_len = p->data_len;
        if (offset + copy_len > decoder->expected_message_len)
          copy_len = decoder->expected_message_len - offset;
        memcpy(message + offset, p->data, copy_len);
        offset += copy_len;
      }

      uint32_t checksum =
          crc32_calculate(message, decoder->expected_message_len);

      if (checksum == decoder->expected_checksum) {
        decoder->result = safe_malloc(sizeof(fountain_decoder_result_t));
        if (decoder->result) {
          decoder->result->data = message;
          decoder->result->data_len = decoder->expected_message_len;
          decoder->result->is_success = true;
          decoder->result->is_error = false;
          message = NULL;
        }
#ifdef DEBUG_STATS
        printf("=== Mixed Parts Statistics ===\n");
        printf("Maximum mixed parts reached: %zu\n",
               decoder->maximum_mixed_parts);
        printf("  From fragments: %zu\n", decoder->mixed_from_fragments);
        printf("  From reduction: %zu\n", decoder->mixed_from_reduction);
        printf("  From cross-reduction: %zu\n",
               decoder->mixed_from_cross_reduction);
        printf("  Total created: %zu\n",
               decoder->mixed_from_fragments + decoder->mixed_from_reduction +
                   decoder->mixed_from_cross_reduction);
        printf("Mixed parts that were useful: %zu\n",
               decoder->mixed_parts_useful);
#endif
      } else {
        decoder->result = safe_malloc(sizeof(fountain_decoder_result_t));
        if (decoder->result) {
          decoder->result->data = NULL;
          decoder->result->data_len = 0;
          decoder->result->is_success = false;
          decoder->result->is_error = true;
        }
      }
    }

    if (message)
      free(message);
  }
}

static void process_mixed_part(fountain_decoder_t *const decoder,
                               const decoder_part_t *const part) {
  if (!decoder || !part || is_simple_part(part) || !decoder->mixed_parts_hash)
    return;

  decoder_part_t reduced_part = {0};

  if (!decoder_part_copy(part, &reduced_part)) {
    return;
  }

  for (size_t i = 0; i < decoder->simple_parts.count; i++) {
    decoder_part_t temp = {0};

    if (reduce_part_by_part(&reduced_part, &decoder->simple_parts.values[i],
                            &temp)) {
      decoder_part_free(&reduced_part);
      reduced_part = temp;
    }
  }

  for (size_t i = 0; i < decoder->mixed_parts_hash->capacity; i++) {
    hash_entry_t *entry = decoder->mixed_parts_hash->buckets[i];
    while (entry) {
      decoder_part_t temp = {0};

      if (reduce_part_by_part(&reduced_part, &entry->value, &temp)) {
        decoder_part_free(&reduced_part);
        reduced_part = temp;
      }
      entry = entry->next;
    }
  }

  if (is_simple_part(&reduced_part)) {
    queue_enqueue(&decoder->queue, &reduced_part);
  } else {
    reduce_mixed_by(decoder, &reduced_part);
    add_mixed_part(decoder, &reduced_part, MIXED_SOURCE_FRAGMENT);
  }

  decoder_part_free(&reduced_part);
}

static void process_queue_item(fountain_decoder_t *const decoder) {
  if (!decoder || queue_is_empty(&decoder->queue))
    return;

  decoder_part_t part = {0};

  if (!queue_dequeue(&decoder->queue, &part))
    return;

  if (is_simple_part(&part)) {
    process_simple_part(decoder, &part);
    reduce_mixed_by(decoder, &part);
  } else {
    process_mixed_part(decoder, &part);
  }

  decoder_part_free(&part);
}

bool fountain_decoder_receive_part(fountain_decoder_t *decoder,
                                   fountain_encoder_part_t *part) {
  if (!decoder || !part) {
    return false;
  }

  if (fountain_decoder_is_complete(decoder)) {
    return false;
  }

  if (decoder->has_received_fragment &&
      part->seq_num == decoder->last_fragment_seq_num) {
    return true;
  }

  if (decoder->expected_part_indexes == NULL) {
    decoder->expected_part_indexes = part_indexes_new();
    if (!decoder->expected_part_indexes)
      return false;

    for (size_t i = 0; i < part->seq_len; i++) {
      if (!part_indexes_add(decoder->expected_part_indexes, i)) {
        fountain_decoder_clear_initialization(decoder);
        return false;
      }
    }

    decoder->expected_checksum = part->checksum;
    decoder->expected_fragment_len = part->data_len;
    decoder->expected_message_len = part->message_len;

    size_t hash_capacity =
        part->seq_len < (HASH_MIN_CAPACITY / HASH_CAPACITY_MULTIPLIER)
            ? HASH_MIN_CAPACITY
            : part->seq_len * HASH_CAPACITY_MULTIPLIER;

    decoder->mixed_parts_hash = safe_malloc(sizeof(mixed_parts_hash_t));
    if (!decoder->mixed_parts_hash) {
      fountain_decoder_clear_initialization(decoder);
      return false;
    }

    if (!mixed_hash_init(decoder->mixed_parts_hash, hash_capacity)) {
      fountain_decoder_clear_initialization(decoder);
      return false;
    }

    if (!hash_set_init(&decoder->received_fragments_hashes, hash_capacity)) {
      fountain_decoder_clear_initialization(decoder);
      return false;
    }

    if (part->seq_len > 0) {
      double *degree_probs = safe_malloc(part->seq_len * sizeof(double));
      if (!degree_probs) {
        fountain_decoder_clear_initialization(decoder);
        return false;
      }
      for (size_t i = 0; i < part->seq_len; i++) {
        degree_probs[i] = 1.0 / (i + 1);
      }
      if (!random_sampler_init(&decoder->degree_sampler, degree_probs,
                               part->seq_len)) {
        free(degree_probs);
        fountain_decoder_clear_initialization(decoder);
        return false;
      }
      free(degree_probs);
    }
  }

  decoder_part_t decoder_part;
  if (!create_decoder_part_from_encoder_part(part, &decoder_part,
                                             &decoder->degree_sampler)) {
    return false;
  }

  uint32_t fragment_hash = (uint32_t)hash_indexes(&decoder_part.indexes);
  if (hash_set_contains(&decoder->received_fragments_hashes, fragment_hash)) {
    decoder_part_free(&decoder_part);
    return true;
  }

  hash_set_add(&decoder->received_fragments_hashes, fragment_hash);

  if (decoder->last_part_indexes) {
    part_indexes_free(decoder->last_part_indexes);
  }
  decoder->last_part_indexes = part_indexes_new();
  if (decoder->last_part_indexes) {
    part_indexes_copy(&decoder_part.indexes, decoder->last_part_indexes);
  }

  if (!queue_enqueue(&decoder->queue, &decoder_part)) {
    decoder_part_free(&decoder_part);
    return false;
  }

  while (!fountain_decoder_is_complete(decoder) &&
         !queue_is_empty(&decoder->queue)) {
    process_queue_item(decoder);
  }

  decoder->processed_parts_count++;
  decoder->last_fragment_seq_num = part->seq_num;
  decoder->has_received_fragment = true;

  decoder_part_free(&decoder_part);

  return true;
}

bool fountain_decoder_is_complete(fountain_decoder_t *decoder) {
  return decoder && decoder->result != NULL;
}

bool fountain_decoder_is_success(fountain_decoder_t *decoder) {
  return decoder && decoder->result && decoder->result->is_success;
}

size_t fountain_decoder_expected_part_count(fountain_decoder_t *decoder) {
  if (!decoder || !decoder->expected_part_indexes)
    return 0;
  return decoder->expected_part_indexes->count;
}

double
fountain_decoder_estimated_percent_complete(fountain_decoder_t *decoder) {
  if (!decoder)
    return 0.0;
  if (fountain_decoder_is_complete(decoder))
    return 1.0;
  if (!decoder->expected_part_indexes)
    return 0.0;

  double estimated_input_parts =
      fountain_decoder_expected_part_count(decoder) * 1.75;
  double progress =
      (double)decoder->processed_parts_count / estimated_input_parts;
  return progress > 0.99 ? 0.99 : progress;
}

uint8_t *fountain_decoder_result_message(fountain_decoder_t *decoder) {
  if (!decoder || !decoder->result)
    return NULL;
  return decoder->result->data;
}

uint8_t *fountain_decoder_take_result_message(fountain_decoder_t *decoder) {
  if (!decoder || !decoder->result)
    return NULL;
  uint8_t *data = decoder->result->data;
  decoder->result->data = NULL;
  return data;
}

size_t fountain_decoder_result_message_len(fountain_decoder_t *decoder) {
  if (!decoder || !decoder->result)
    return 0;
  return decoder->result->data_len;
}

size_t fountain_decoder_processed_parts_count(fountain_decoder_t *decoder) {
  if (!decoder)
    return 0;
  return decoder->processed_parts_count;
}
