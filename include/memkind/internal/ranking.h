// SPDX-License-Identifier: BSD-2-Clause
/* Copyright (C) 2022 Intel Corporation. */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "stdbool.h"
#include "stddef.h"
#include "stdint.h"

typedef void *ranking_handle;
typedef void *metadata_handle;
typedef void *ranking_hottest_iterator;
typedef void *ranking_coldest_iterator;

extern void ranking_create(ranking_handle *handle);
extern void ranking_destroy(ranking_handle handle);

extern void ranking_metadata_create(metadata_handle *handle);
extern void ranking_metadata_destroy(metadata_handle handle);

extern bool ranking_touch(ranking_handle handle, uintptr_t address);
extern void ranking_update(ranking_handle handle, uint64_t timestamp);
extern void ranking_add_pages(ranking_handle handle, uintptr_t start_address,
                              size_t nof_pages, uint64_t timestamp);
/// check if each page exists in ranking; remove only those that do exist
/// @return number of removed pages
extern size_t ranking_try_remove_pages(ranking_handle handle,
                                       uintptr_t start_address,
                                       size_t nof_pages);
/// @param[out] hotness highest hotness value in Ranking
/// @return
///     bool: true if not empty (hotness valid)
extern bool ranking_get_hottest(ranking_handle handle, double *hotness);
/// @param[out] hotness lowest hotness value in Ranking
/// @return
///     bool: true if not empty (hotness valid)
extern bool ranking_get_coldest(ranking_handle handle, double *hotness);
extern void ranking_pop_coldest(ranking_handle handle, metadata_handle page);
extern void ranking_pop_hottest(ranking_handle handle, metadata_handle page);
extern void ranking_pop_address(ranking_handle handle, metadata_handle page,
                                uintptr_t address);
extern void ranking_add_page(ranking_handle handle, metadata_handle page);
extern uintptr_t ranking_get_page_address(metadata_handle page);
/// @return traced size, in bytes
extern size_t ranking_get_total_size(ranking_handle handle);
/// @return true if @p[out] iterator valid, false otherwise
/// @warning calling any API other than:
///     - ranking_get_coldest_iterator
///     - ranking_(hottest|coldest)_iterator_(advance|get_hotness|get_address)
/// invalidates the iterator!
/// @note no need to free the associated memory
extern bool ranking_get_hottest_iterator(ranking_handle handle,
                                         ranking_hottest_iterator *iterator);
/// @return true if @p iterator valid, false otherwise
extern bool ranking_hottest_iterator_advance(ranking_hottest_iterator iterator);
/// @return hotness of @p iterator
extern double
ranking_hottest_iterator_get_hotness(ranking_hottest_iterator iterator);
extern uintptr_t
ranking_hottest_iterator_get_address(ranking_hottest_iterator iterator);
/// @return true if @p[out] iterator valid, false otherwise
/// @warning calling any API other than:
///     - ranking_get_hottest_iterator
///     - ranking_(hottest|coldest)_iterator_(advance|get_hotness|get_address)
/// invalidates the iterator!
/// @note no need to free the associated memory
extern bool ranking_get_coldest_iterator(ranking_handle handle,
                                         ranking_coldest_iterator *iterator);

extern double
ranking_coldest_iterator_get_hotness(ranking_coldest_iterator iterator);
extern uintptr_t
ranking_coldest_iterator_get_address(ranking_coldest_iterator iterator);
extern bool ranking_coldest_iterator_advance(ranking_hottest_iterator iterator);

#ifdef __cplusplus
}
#endif
