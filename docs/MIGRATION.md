# Migrating to sparsemap 3.0.0

3.0.0 is the first formal public release.  If you already vendor a
copy of sparsemap (e.g. inside a PostgreSQL extension), this is the
checklist for syncing to 3.0.0.

The library is now exactly two files: `sm.h` (public API) and `sm.c`
(implementation, with the portability shims folded in).  Vendoring is
a two-file copy; `contrib/pg_tre_sync.sh` and
`contrib/postgres_undo_sync.sh` automate it.

## Source-level breaking changes

Both are mechanical search-and-replace:

1. **The opaque type is `sm_t`, not `sparsemap_t`.**

   ```sh
   sed -i 's/\bsparsemap_t\b/sm_t/g' your_files.c your_files.h
   ```

   The struct *tag* (`struct sparsemap`) is unchanged, so any code
   that wrote `struct sparsemap *` keeps working; only the typedef
   name changed.

2. **The optional vendoring prefix macro is `SPARSEMAP_PREFIX`, not
   `SM_PREFIX`.**  If you build a renamed copy to avoid symbol
   collisions, rename the define:

   ```c
   #define SPARSEMAP_PREFIX myapp_
   #include <sparsemap/sm.h>
   ```

That is the entire source-level break.  Every `sm_*` function keeps
its name, signature, and behavior.

## What did *not* change

- **Function names, signatures, and semantics** of all `sm_*` public
  functions.
- **The serialized wire format** (`sm_serialize` / `sm_deserialize` /
  `sm_open`).  Bytes written by a pre-3.0 vendored copy deserialize
  unchanged under 3.0.0, and the format is held stable across the 3.x
  series.
- **Allocation lineage and disposal rules** (see [API.md](API.md)):
  `sm_free()` disposes any map regardless of lineage; libc `free()`
  still works for the owned-contiguous maps returned by `sm_create()`
  / `sm_copy()`.

## ABI

`sm_t` is opaque.  Do not embed it inline, do not `sizeof` it for an
on-disk format, and recompile (not just relink) after upgrading.
`sizeof(sm_t)` and field offsets are not part of the stable contract;
the wire format is.

## Allocators

The allocator is process-global, set with `sm_set_allocator(hooks)`
(CRoaring's `roaring_init_memory_hook` model).  `sm_allocator_t` is a
minimal `{ malloc, realloc, free }` triple; an all-zero struct means
"use libc".  There is no per-map allocator.

5.0.0 note: earlier 4.x releases also offered
`sm_create_with_allocator(n, hooks)` for a per-map override and a
larger hook struct (`alloc_zero`, `aligned_alloc`, `aux`).  Those were
removed in 5.0.0 to keep `sm_t` to three words; route everything
through `sm_set_allocator` instead.

## Pre-3.0 history

The granular development history (the library matured while vendored
inside other projects) is archived on the `archive/v2.3.0` tag.  The
3.0.0 published history is a clean import; there is no `v2.x` release
series on this repository to upgrade *from* in the usual sense -- you
are migrating a vendored copy, and the two `sed` rules above cover it.
