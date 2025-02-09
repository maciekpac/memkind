// SPDX-License-Identifier: BSD-2-Clause
/* Copyright (C) 2022 Intel Corporation. */

#include "memkind/internal/slab_tracker.h"
#include "memkind/internal/critnib.h"
#include "memkind/internal/memkind_private.h"

#include <cassert>
#include <memory>
#include <mutex>
#include <unordered_map>

struct SlabTrackerInternals {
    critnib addrToSlab;
    SlabTrackerInternals()
    {
        critnib_create(&this->addrToSlab);
    }
    ~SlabTrackerInternals()
    {
        critnib_destroy(&this->addrToSlab);
    }
};

MEMKIND_EXPORT void fast_slab_tracker_create(SlabTracker **slab_tracker)
{
    *slab_tracker = new SlabTrackerInternals();
}

MEMKIND_EXPORT void fast_slab_tracker_destroy(SlabTracker *slab_tracker)
{
    delete static_cast<SlabTrackerInternals *>(slab_tracker);
}

MEMKIND_EXPORT void
fast_slab_tracker_register(SlabTracker *slab_tracker, uintptr_t addr,
                           FastSlabAllocator *fast_slab_allocator)
{
    assert(fast_slab_allocator && "fast_slab_allocator cannot be NULL!");
    SlabTrackerInternals *self =
        static_cast<SlabTrackerInternals *>(slab_tracker);
    int ret = critnib_insert(&self->addrToSlab, addr, fast_slab_allocator, 0);
    assert(ret == 0lu &&
           "critnib_insert failed; ret is either EEXIST or ENOMEM");
}

MEMKIND_EXPORT FastSlabAllocator *
fast_slab_tracker_get_fast_slab(SlabTracker *slab_tracker, uintptr_t addr)
{
    SlabTrackerInternals *self =
        static_cast<SlabTrackerInternals *>(slab_tracker);
    FastSlabAllocator *slab = static_cast<FastSlabAllocator *>(
        critnib_find_le(&self->addrToSlab, addr));
    uintptr_t area_start = reinterpret_cast<uintptr_t>(slab->mappedMemory.area);
    uintptr_t area_end = area_start + slab->mappedMemory.top;
    assert(area_start <= addr);
    return addr < area_end ? slab : nullptr;
}
