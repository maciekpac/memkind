// SPDX-License-Identifier: BSD-2-Clause
/* Copyright (C) 2022 Intel Corporation. */

#include "memkind/internal/mtt_internals.h"
#include "memkind/internal/bigary.h"
#include "memkind/internal/memkind_log.h"
#include "memkind/internal/memkind_private.h"
#include "memkind/internal/mmap_tracing_queue.h"
#include "memkind/internal/pagesizes.h"
#include "memkind/internal/ranking.h"
#include "memkind/internal/ranking_utils.h"

#include "assert.h"
#include "stdint.h"
#include "string.h"
#include "stdint.h"
#include <numa.h>
#include <numaif.h>

#define min(a, b) (((a) < (b)) ? (a) : (b))

#if USE_FAST_POOL
#define POOL_ALLOCATOR_TYPE(x) fast_pool_allocator_##x
#else
#define POOL_ALLOCATOR_TYPE(x) pool_allocator_##x
#endif

// with 4kB traced page, 512kB per cycle
#define MAX_TO_JUGGLE (1024ul)

// static functions -----------------------------------------------------------

#if 0
static void promote_hottest_pmem(MttInternals *internals)
{
    // promote (PMEM->DRAM)
    ranking_pop_hottest(internals->pmemRanking, internals->tempMetadataHandle);
    uintptr_t start_addr =
        ranking_get_page_address(internals->tempMetadataHandle);
    ranking_add_page(internals->dramRanking, internals->tempMetadataHandle);
    int ret = move_page_metadata(start_addr, DRAM);
    assert(ret == MEMKIND_SUCCESS && "mbind data movement failed");
}

static void demote_coldest_dram(MttInternals *internals)
{
    // demote (DRAM->PMEM)
    ranking_pop_coldest(internals->dramRanking, internals->tempMetadataHandle);
    uintptr_t start_addr =
        ranking_get_page_address(internals->tempMetadataHandle);
    ranking_add_page(internals->pmemRanking, internals->tempMetadataHandle);
    int ret = move_page_metadata(start_addr, DAX_KMEM);
    assert(ret == MEMKIND_SUCCESS && "mbind data movement failed");
}
#endif

static size_t mtt_internals_find_how_many_to_juggle(MttInternals *internals,
                                                    void **pages_to_demote,
                                                    void **pages_to_promote,
                                                    const size_t max_to_juggle)
{
    ranking_coldest_iterator coldest_dram_it;
    ranking_hottest_iterator hottest_pmem_it;
    bool success_dram =
        ranking_get_coldest_iterator(internals->dramRanking, &coldest_dram_it);
    bool success_pmem =
        ranking_get_hottest_iterator(internals->pmemRanking, &hottest_pmem_it);

    const size_t system_pages_in_traced_page =
        TRACED_PAGESIZE / traced_pagesize_get_sysytem_pagesize();
    assert(system_pages_in_traced_page == 1ul && "some code still depends on this being true");
    size_t juggle_it = 0ul;
    size_t traced_pages_to_juggle = 0ul;
    while (juggle_it + system_pages_in_traced_page - 1ul < max_to_juggle &&
           success_dram && success_pmem &&
           ranking_hottest_iterator_get_hotness(hottest_pmem_it) >
               ranking_coldest_iterator_get_hotness(coldest_dram_it)) {
        size_t end_system_page = juggle_it + system_pages_in_traced_page;
        for (; juggle_it < end_system_page; ++juggle_it) {
            pages_to_demote[juggle_it] =
                (void *)ranking_coldest_iterator_get_address(coldest_dram_it);
            pages_to_promote[juggle_it] =
                (void *)ranking_hottest_iterator_get_address(hottest_pmem_it);
        }
        success_dram = ranking_coldest_iterator_advance(coldest_dram_it);
        success_pmem = ranking_hottest_iterator_advance(hottest_pmem_it);
        ++traced_pages_to_juggle;
    }
    return traced_pages_to_juggle;
}

static size_t mtt_internals_move_between_rankings(
    metadata_handle temp_handle, ranking_handle from, ranking_handle to,
    void **pages_to_move, int *status, size_t reset_value, size_t count)
{
    size_t success_count = 0ul;
    for (size_t i = 0ul; i < count; ++i) {
        if (status[i] >= 0 && status[i] != reset_value) {
            uintptr_t caddr = (uintptr_t)pages_to_move[i];
            ranking_pop_address(from, temp_handle, caddr);
            ranking_add_page(to, temp_handle);
            ++success_count;
        } // else if (status[i] < 0) {
        //    assert(false);
        //}
        status[i] = reset_value; // reset the status
    }

    return success_count;
}

size_t mtt_internals_demote_pages(MttInternals *internals, void **pages,
                                  size_t count, int reset_value)
{

    static int nodes_pmem[MAX_TO_JUGGLE] = {2}; // TODO take the first pmem node
    int status[MAX_TO_JUGGLE] = {reset_value};

    long ret = move_pages(0, count, pages, nodes_pmem, status, MPOL_MF_MOVE);
    assert(ret >= 0);
    size_t success_count = mtt_internals_move_between_rankings(
        internals->tempMetadataHandle, internals->dramRanking,
        internals->pmemRanking, pages, status, reset_value, count);
    // count -= ret;
    //assert(count == success_count &&
    //       "Internal error - number of moved pages does not match");

    return success_count;
}

size_t mtt_internals_promote_pages(MttInternals *internals, void **pages,
                                   size_t count, int reset_value)
{

    static int nodes_dram[MAX_TO_JUGGLE] = {0}; // TODO take the first pmem node
    int status[MAX_TO_JUGGLE] = {reset_value};

    long ret = move_pages(0, count, pages, nodes_dram, status, MPOL_MF_MOVE);
    assert(ret >= 0);
    size_t success_count = mtt_internals_move_between_rankings(
        internals->tempMetadataHandle, internals->pmemRanking,
        internals->dramRanking, pages, status, reset_value, count);
    // count -= ret;
    // assert(count == success_count &&
    //       "Internal error - number of moved pages does not match");

    return success_count;
}

/// @brief Move data bettween hot and cold tiers without changing proportions
///
/// Move data between tiers - demote cold pages from DRAM and promote hot pages
/// from PMEM; the same number of promotions is performed as demotions, hence
/// DRAM/PMEM *proportions* do not change
///
/// @note This function might temporarily increase PMEM usage, but should not
/// increase DRAM usage and should not cause DRAM limit overflow
static void mtt_internals_tiers_juggle(MttInternals *internals,
                                       atomic_bool *interrupt)
{
    // Handle hottest on PMEM vs coldest on dram page movement
    // different approaches possible, e.g.
    //      a) move when avg (hottest dram, coldest dram) < hottest dram
    //      b) move while coldest dram is colder than hottest pmem
    // Approach b) was chosen, at least for now

    // check for some arbitrary, low limit
    assert(MAX_TO_JUGGLE * 3ul < 12 * 1024ul &&
           "stack array bigger than 12 kB, MAX_TO_JUGGLE too high!");

    const size_t MAX_NUMNODES = numa_num_possible_nodes();
    void *pages_to_demote[MAX_TO_JUGGLE];
    void *pages_to_promote[MAX_TO_JUGGLE];
    // TODO initialize nodes !!!
    // move_pages operates in batches
    // how to:
    // 1. Iterate to find out the number of pages to juggle; collect the
    // addresses
    // 2. Demote pages to demote
    // 3. Check demotion status and handle it
    //  1) exchange between rankings
    //  2) reduce the number of pages to promote by the number of failed
    //  demotions
    // 4. Promote pages to promote
    // 5. Check promotion status
    //  1) exchange between rankings
    //  2) What with the failed promotions ? Just leave as is, will be
    //  promoted the next time

    size_t to_juggle = mtt_internals_find_how_many_to_juggle(
        internals, pages_to_demote, pages_to_promote, MAX_TO_JUGGLE);

    // demote pages
    size_t demoted = mtt_internals_demote_pages(internals, pages_to_demote,
                                                to_juggle, MAX_NUMNODES);
    assert(demoted <= to_juggle);
    size_t promoted = mtt_internals_promote_pages(internals, pages_to_promote, demoted,
                                      MAX_NUMNODES);
    assert(promoted <= demoted);
}

static size_t mtt_internals_ranking_balance_internal(MttInternals *internals,
                                                     atomic_size_t *used_dram,
                                                     size_t dram_limit)
{
    size_t ret = 0ul;
    const size_t MAX_NUMNODES = numa_num_possible_nodes();
    const size_t BATCH_SIZE = 1024ul;
    // Handle soft and hard limit - move pages dram vs pmem
    size_t temp_dram_size = atomic_load(used_dram);
    if (dram_limit < temp_dram_size) {
        // move (demote) DRAM to PMEM
        size_t size_to_move = temp_dram_size - dram_limit;
        assert(size_to_move % TRACED_PAGESIZE == 0 &&
               "ranking size is not a multiply of TRACED_PAGESIZE");
        size_t dram_ranking_size =
            ranking_get_total_size(internals->dramRanking);
        if (size_to_move > dram_ranking_size) {
            ret = size_to_move - dram_ranking_size;
            size_to_move = dram_ranking_size;
        }
        assert(size_to_move <= ranking_get_total_size(internals->dramRanking) &&
               "a demotion bigger than DRAM ranking requested, case not handled"
               " hard/soft_limit too low,"
               " queued mmap(s) should go directly to pmem");
        assert(*used_dram >= ranking_get_total_size(internals->dramRanking) &&
               "internal error; dram size, including queued data,"
               " should not be higher than dram ranking size");
        ranking_coldest_iterator iterator;
        void *pages_to_demote[BATCH_SIZE];
        size_t nof_pages_to_move = size_to_move / TRACED_PAGESIZE;

        size_t page_idx = 0ul;
        size_t zero_demoted_count = 0ul;
        const size_t ZERO_DEMOTED_LIMIT = 1024ul * 1024ul;

        while (page_idx < nof_pages_to_move) {
            bool success =
                ranking_get_coldest_iterator(internals->dramRanking, &iterator);
            // iterate batch
            size_t cbatch_size = nof_pages_to_move <= BATCH_SIZE
                ? nof_pages_to_move
                : BATCH_SIZE;
            // move
            size_t page_batch_idx = 0ul;
            for (page_batch_idx = 0ul;
                 page_batch_idx < cbatch_size && success; ++page_batch_idx) {
                pages_to_demote[page_batch_idx] =
                    (void *)ranking_coldest_iterator_get_address(iterator);
                success = ranking_coldest_iterator_advance(iterator);
            }
            assert(
                success &&
                "Error in code - advancing iterator failed even though it should have succeeded");
            size_t demoted = mtt_internals_demote_pages(
                internals, pages_to_demote, page_batch_idx, MAX_NUMNODES);
            page_idx += demoted;
            if (demoted == 0ul) {
                ++zero_demoted_count;
                assert(zero_demoted_count <= ZERO_DEMOTED_LIMIT &&
                       "pages cannot be demoted!");
            }
        }
        atomic_fetch_sub(used_dram, size_to_move);
        assert((int64_t)*used_dram >= 0 && "internal error, moved too much");
    } else if (temp_dram_size + TRACED_PAGESIZE < internals->limits.lowLimit) {

        // move (promote) PMEM to DRAM
        // incease dram_size before movement to avoid race conditions &
        // hard_limit overflow

        //         TODO would be perfect to perform the check
        //         assert(size_to_move % TRACED_PAGESIZE == 0 &&
        //                "ranking size is not a multiply of TRACED_PAGESIZE");

        // TODO ADAPT
        ranking_hottest_iterator iterator;
        void *pages_to_promote[BATCH_SIZE];
        size_t size_to_move =
            internals->limits.lowLimit - (temp_dram_size + TRACED_PAGESIZE);

        size_t page_idx = 0ul;
        size_t zero_promoted_count = 0ul;
        const size_t ZERO_PROMOTED_LIMIT = 1024ul * 1024ul;

        size_t new_dram_size =
            atomic_fetch_add(used_dram, size_to_move) + size_to_move;
        if (new_dram_size > internals->limits.lowLimit) {
            // an allocation was made in the background
            size_t to_lower = new_dram_size - internals->limits.lowLimit;
            (void)atomic_fetch_sub(used_dram, size_to_move);
            size_to_move -= to_lower;
        }
        size_t nof_pages_to_move = size_to_move / TRACED_PAGESIZE;

        // TODO update nof_pages_to_move if new_dram_size exceeds soft limit!
        size_t total_promoted = 0ul;
        while (page_idx < nof_pages_to_move) {
            bool success =
                ranking_get_hottest_iterator(internals->pmemRanking, &iterator);
            // iterate batch
            size_t cbatch_size = nof_pages_to_move <= BATCH_SIZE
                ? nof_pages_to_move
                : BATCH_SIZE;
            // move
            size_t page_batch_idx = 0ul;
            for (page_batch_idx = 0ul;
                 page_batch_idx < cbatch_size && success; ++page_batch_idx) {
                pages_to_promote[page_batch_idx] =
                    (void *)ranking_hottest_iterator_get_address(iterator);
                success = ranking_hottest_iterator_advance(iterator);
            }
            // ignore failed moves, we're doing it best-effort'
            size_t promoted = mtt_internals_promote_pages(
                internals, pages_to_promote, page_batch_idx, MAX_NUMNODES);
            total_promoted += promoted;
            page_idx += cbatch_size;
            if (promoted == 0ul) {
                ++zero_promoted_count;
                assert(zero_promoted_count <= ZERO_PROMOTED_LIMIT &&
                       "pages cannot be promoted!");
            }
            // TODO lower used_dram by the number of failed promotions
        }

        // eof TODO ADAPT

        //         while (ranking_get_total_size(internals->pmemRanking) > 0ul
        //         &&
        //                temp_dram_size < internals->limits.lowLimit) {
        //             promote_hottest_pmem(internals);
        //             temp_dram_size =
        //                 atomic_fetch_add(used_dram, TRACED_PAGESIZE) +
        //                 TRACED_PAGESIZE;
        //         }
        // last iteration increases dram_size, but movement is not performed -
        // the value should be decreased back
        (void)atomic_fetch_sub(used_dram, TRACED_PAGESIZE);
    }

    return ret;
}

static ssize_t mtt_internals_process_queued(MttInternals *internals)
{
    MMapTracingNode *node =
        mmap_tracing_queue_multithreaded_take_all(&internals->mmapTracingQueue);
    uintptr_t start_addr;
    size_t nof_pages;
    MMapTracingEvent_e event;
    ssize_t dram_pages_added = 0l;
    while (mmap_tracing_queue_process_one(&node, &start_addr, &nof_pages,
                                          &event)) {
        switch (event) {
            case MMAP_TRACING_EVENT_MMAP:
                ranking_add_pages(internals->dramRanking, start_addr, nof_pages,
                                  internals->lastTimestamp);
                break;
            case MMAP_TRACING_EVENT_RE_MMAP:
                for (uintptr_t tstart_addr = start_addr;
                     tstart_addr < start_addr + nof_pages * TRACED_PAGESIZE;
                     tstart_addr += TRACED_PAGESIZE) {
                    memory_type_t mem_type = get_page_memory_type(tstart_addr);
                    ranking_handle ranking_to_add =
                        NULL; // initialize - silence gcc warning
                    switch (mem_type) {
                        case DRAM:
                            ranking_to_add = internals->dramRanking;
                            ++dram_pages_added;
                            break;
                        case DAX_KMEM:
                            ranking_to_add = internals->pmemRanking;
                            break;
                    }
                    ranking_add_pages(ranking_to_add, tstart_addr, 1ul,
                                      internals->lastTimestamp);
                }
                break;
            case MMAP_TRACING_EVENT_MUNMAP:
                dram_pages_added -= (ssize_t)ranking_try_remove_pages(
                    internals->dramRanking, start_addr, nof_pages);
                (void)ranking_try_remove_pages(internals->pmemRanking,
                                               start_addr, nof_pages);
                break;
        }
    }

    return dram_pages_added;
}

// global functions -----------------------------------------------------------

MEMKIND_EXPORT int mtt_internals_create(MttInternals *internals,
                                        uint64_t timestamp,
                                        const MTTInternalsLimits *limits,
                                        MmapCallback *user_mmap)
{
    // TODO think about default limits value
    internals->usedDram = 0ul;
    internals->lastTimestamp = timestamp;
    assert(limits->lowLimit <= limits->softLimit &&
           "low limit (movement PMEM -> DRAM occurs below) "
           " has to be lower than or equal to "
           " soft limit (movement DRAM -> PMEM occurs above)");
    assert(limits->softLimit <= limits->hardLimit &&
           "soft limit (movement DRAM -> PMEM occurs above) "
           " has to be lower than or equal to "
           " hard limit (any allocation that surpasses this limit "
           " should be placed on PMEM TODO not implemented)");
    assert(limits->softLimit > 0ul &&
           "soft & hard limits have to be positive!");
    assert(limits->hardLimit % TRACED_PAGESIZE == 0 &&
           "hard limit is not a multiple of TRACED_PAGESIZE");
    assert(limits->softLimit % TRACED_PAGESIZE == 0 &&
           "soft limit is not a multiple of TRACED_PAGESIZE");
    assert(limits->lowLimit % TRACED_PAGESIZE == 0 &&
           "low limit is not a multiple of TRACED_PAGESIZE");
    internals->limits = *limits;
    ranking_create(&internals->dramRanking);
    ranking_create(&internals->pmemRanking);
    ranking_metadata_create(&internals->tempMetadataHandle);
    mmap_tracing_queue_create(&internals->mmapTracingQueue);

    int ret = POOL_ALLOCATOR_TYPE(create)(&internals->pool, user_mmap);

    return ret;
}

MEMKIND_EXPORT void mtt_internals_destroy(MttInternals *internals)
{
    ranking_metadata_destroy(internals->tempMetadataHandle);
    ranking_destroy(internals->pmemRanking);
    ranking_destroy(internals->dramRanking);
    mmap_tracing_queue_destroy(&internals->mmapTracingQueue);
    // internals are already destroyed, no need to track mmap/munmap
    POOL_ALLOCATOR_TYPE(destroy)(&internals->pool, &gStandardMmapCallback);
}

MEMKIND_EXPORT void *mtt_internals_malloc(MttInternals *internals, size_t size,
                                          MmapCallback *user_mmap)
{
    void *ret =
        POOL_ALLOCATOR_TYPE(malloc_mmap)(&internals->pool, size, user_mmap);
    return ret;
}

MEMKIND_EXPORT void *mtt_internals_realloc(MttInternals *internals, void *ptr,
                                           size_t size, MmapCallback *user_mmap)
{
    void *ret = POOL_ALLOCATOR_TYPE(realloc_mmap)(&internals->pool, ptr, size,
                                                  user_mmap);
    return ret;
}

MEMKIND_EXPORT void mtt_internals_free(MttInternals *internals, void *ptr)
{
    // TODO add & handle unmap during productization stage!
    // we don't unmap pages, the data is not handled
    POOL_ALLOCATOR_TYPE(free)(&internals->pool, ptr);
}

MEMKIND_EXPORT size_t mtt_internals_usable_size(MttInternals *internals,
                                                void *ptr)
{
    return POOL_ALLOCATOR_TYPE(usable_size)(&internals->pool, ptr);
}

MEMKIND_EXPORT void mtt_internals_touch(MttInternals *internals,
                                        uintptr_t address)
{
    // touch both - if ranking does not contain the page,
    // the touch will be ignored
    //
    // touch on unknown address is immediately dropped in O(1) time
    ranking_touch(internals->dramRanking, address);
    ranking_touch(internals->pmemRanking, address);
}

MEMKIND_EXPORT size_t mtt_internals_ranking_balance(MttInternals *internals,
                                                    atomic_size_t *used_dram,
                                                    size_t dram_limit)
{
    // update dram ranking
    ssize_t diff_in_dram_used = mtt_internals_process_queued(internals);
    // handle issues with signed -> unsigned casting
    if (diff_in_dram_used >= 0) {
        atomic_fetch_add(used_dram, diff_in_dram_used);
    } else {
        atomic_fetch_sub(used_dram, -diff_in_dram_used);
    }
    return mtt_internals_ranking_balance_internal(internals, used_dram,
                                                  dram_limit);
}

MEMKIND_EXPORT void mtt_internals_ranking_update(MttInternals *internals,
                                                 uint64_t timestamp,
                                                 atomic_size_t *used_dram,
                                                 atomic_bool *interrupt)
{
    internals->lastTimestamp = timestamp;
    // 1. Add new mappings to rankings
    ssize_t additional_dram_used = mtt_internals_process_queued(internals);
    // handle issues with signed -> unsigned casting
    if (additional_dram_used >= 0) {
        atomic_fetch_add(used_dram, additional_dram_used);
    } else {
        atomic_fetch_sub(used_dram, -additional_dram_used);
    }
    // 2. Update both rankings - hotness
    if (atomic_load(interrupt))
        return;
    ranking_update(internals->dramRanking, timestamp);
    if (atomic_load(interrupt))
        return;
    ranking_update(internals->pmemRanking, timestamp);
    // 3. Handle soft and hard limit - move pages dram vs pmem
    size_t surplus = mtt_internals_ranking_balance(internals, used_dram,
                                                   internals->limits.softLimit);
    if (surplus)
        log_info("Not able to balance DRAM usage down to softLimit at once,"
                 " %lu left unmoved;"
                 " was a single allocation > lowLimit requested?",
                 surplus);
    if (atomic_load(interrupt))
        return;
    // 4. Handle hottest on PMEM vs coldest on dram page movement
    mtt_internals_tiers_juggle(internals, interrupt);
}

MEMKIND_EXPORT void
mtt_internals_tracing_multithreaded_push(MttInternals *internals,
                                         uintptr_t addr, size_t nof_pages,
                                         MMapTracingEvent_e event)
{
    mmap_tracing_queue_multithreaded_push(&internals->mmapTracingQueue, addr,
                                          nof_pages, event);
}
