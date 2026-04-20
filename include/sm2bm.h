#ifndef SM2BM_H
#define SM2BM_H

/* Include your Bitmapset implementation */
/* Note: You'll need to include your bitmapset.h here */

/*
 * Sparsemap API Function Mapping
 *
 * Maps Bitmapset functions to sparsemap test interface using functions
 * that match the sparsemap API signature.
 */

/* Map sparsemap types to Bitmapset */
typedef Bitmapset sparsemap_t;

/* Additional utility constants */
#define SPARSEMAP_MAX_BITS INT_MAX

/*
 * Sparsemap API Implementation using Bitmapset - Optimized Version
 *
 * These functions provide the sparsemap interface while using
 * PostgreSQL's Bitmapset functions directly where possible.
 */

/* Core allocation and initialization functions */
sparsemap_t *
sparsemap(size_t size)
{
  return bms_create(size);
}

sparsemap_t *
sparsemap_copy(sparsemap_t *other)
{
  return bms_copy(other);
}

sparsemap_t *
sparsemap_wrap(uint8_t *data, size_t size)
{
  return bms_create_with_buffer(data, size);
}

extern Bitmapset *bms_init(Bitmapset *map, uint8 *data, size_t size);
void
sparsemap_init(sparsemap_t *map, uint8_t *data, size_t size)
{
  bms_init(map, data, size);
}

void
sparsemap_open(sparsemap_t *map, uint8_t *data, size_t size)
{
  bms_attach_buffer(map, data, size);
}

extern void bms_empty(Bitmapset *map);
void
sparsemap_clear(sparsemap_t *map)
{
  bms_empty(map);
}

sparsemap_t *
sparsemap_set_data_size(sparsemap_t *map, uint8_t *data, size_t size)
{
  return bms_resize(map, data, size);
}

/* Capacity and size functions */
extern double bms_get_utilization(const Bitmapset *map);
double
sparsemap_capacity_remaining(sparsemap_t *map)
{
  return bms_get_utilization(map);
}

size_t
sparsemap_get_capacity(sparsemap_t *map)
{
  return bms_get_capacity(map);
}

size_t
sparsemap_get_size(sparsemap_t *map)
{
  return bms_get_used_size(map);
}

size_t
sparsemap_count(sparsemap_t *map)
{
  return bms_num_members(map);
}

/* Bit operations - direct calls to bms functions */
bool
sparsemap_is_set(sparsemap_t *map, sparsemap_idx_t idx)
{
  return bms_is_member((int)idx, map);
}

sparsemap_idx_t
sparsemap_set(sparsemap_t *map, sparsemap_idx_t idx, bool value)
{
  Bitmapset *result;
  if (value)
    result = bms_add_member(map, (int)idx);
  else
    result = bms_del_member(map, (int)idx);

  if (result == NULL)
    return SPARSEMAP_IDX_MAX;

  /* Update the map structure if it changed */
  if (result != map)
    {
      *map = *result;
      bms_free(result);
    }

  return idx;
}

/* Range and offset functions - direct calls where possible */
sparsemap_idx_t
sparsemap_get_starting_offset(sparsemap_t *map)
{
  int first = bms_first_member(map);
  return (first >= 0) ? (sparsemap_idx_t)first : SPARSEMAP_IDX_MAX;
}

sparsemap_idx_t
sparsemap_get_ending_offset(sparsemap_t *map)
{
  int last = bms_last_member(map);
  return (last >= 0) ? (sparsemap_idx_t)last : SPARSEMAP_IDX_MAX;
}

double
sparsemap_fill_factor(sparsemap_t *map)
{
  return bms_density(map);
}

/* Scanning and iteration */
extern void bms_scan(const Bitmapset *map, void (*scanner) (sm_idx_t bit_indices[], size_t count, void *aux_data), size_t skip_count, void *aux_data);
void
sparsemap_scan(sparsemap_t *map, void (*scanner)(sm_idx_t vec[], size_t n, void *aux), size_t skip, void *aux)
{
  return bms_scan(map, scanner, skip, aux);
}

/* Set operations - direct calls to bms functions */
int
sparsemap_merge(sparsemap_t *destination, sparsemap_t *source)
{
  if (destination == NULL || source == NULL)
    return 0;

  Bitmapset *result = bms_union(destination, source);
  if (result == NULL)
    return -1;

  *destination = *result;
  bms_free(result);
  return 0;
}

sparsemap_idx_t
sparsemap_split(sparsemap_t *map, sparsemap_idx_t offset, sparsemap_t *other)
{
  return bms_split(map, offset, other);
}

/* Advanced query functions - direct calls where possible */
sparsemap_idx_t
sparsemap_select(sparsemap_t *map, sparsemap_idx_t n, bool value)
{
  return bms_select(map, n, value);
}

typedef unsigned int bms_bitvec_t;

extern size_t bms_rank_range(Bitmapset *map, size_t start_bit, size_t end_bit, bool target_value, bms_bitvec_t * work_vector);
size_t
sparsemap_rank(sparsemap_t *map, size_t x, size_t y, bool value)
{
  return bms_rank(map, (int)x, (int)(y + 1), value);
}

size_t
sparsemap_span(sparsemap_t *map, sparsemap_idx_t start, size_t len, bool value)
{
  return bms_find_span(map, (int)start, (int)len, value);
}


/* Test helper functions */
void
sparsemap_print(sparsemap_t *sm)
{
  if (sm == NULL)
    {
      printf("{}");
      return;
    }

  printf("{");
  int bit = bms_first_member(sm);
  bool first = true;

  while (bit >= 0)
    {
      if (!first)
	printf(", ");
      printf("%d", bit);
      first = false;
      bit = bms_next_member(sm, bit);
    }
  printf("}");
}

size_t
sparsemap_memory_usage(sparsemap_t *sm)
{
  if (sm == NULL)
    return 0;

  return sizeof(sparsemap_t) + sm->size;
}

#endif /* SM2BM_H */
