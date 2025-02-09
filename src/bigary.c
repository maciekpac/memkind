// SPDX-License-Identifier: BSD-2-Clause
/* Copyright (C) 2021-2022 Intel Corporation. */

#include "memkind/internal/bigary.h"
#include "memkind/internal/memkind_log.h"
#include "memkind/internal/pagesizes.h"

#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define BIGARY_DEFAULT_MAX (64 * 1024 * 1024 * 1024ULL)

static void die(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_fatal(fmt, args);
    va_end(args);
    abort();
}

/***************************/
/* initialize a new bigary */
/***************************/

void bigary_init(bigary *restrict m_bigary, int fd, int flags, size_t max)
{
    assert((BIGARY_PAGESIZE % TRACED_PAGESIZE) == 0 &&
           "bigary pagesize and traced pagesize are not aligned! "
           "BIGARY_PAGESIZE should be a multiply of TRACED_PAGESIZE");
    if (!max)
        max = BIGARY_DEFAULT_MAX;
    // round *max* up to pagesize
    size_t last_page_size = max % BIGARY_PAGESIZE;
    if (last_page_size) {
        size_t pages = max / BIGARY_PAGESIZE + 1;
        max = pages * BIGARY_PAGESIZE;
    }
    int ret = pthread_mutex_init(&m_bigary->enlargement, NULL);
    if (ret != 0)
        die("mutex init failed\n");
    m_bigary->declared = max;
    m_bigary->fd = fd;
    m_bigary->flags = flags;
    if ((m_bigary->area = mmap(0, max, PROT_NONE, flags, fd, 0)) == MAP_FAILED)
        die("mmapping bigary(%zd) failed: %m\n", max);

    if (mmap(m_bigary->area, BIGARY_PAGESIZE, PROT_READ | PROT_WRITE,
             MAP_FIXED | flags, fd, 0) == MAP_FAILED) {
        die("bigary alloc of %zd failed: %m\n", BIGARY_PAGESIZE);
    }
    m_bigary->top = BIGARY_PAGESIZE;
}

void bigary_init_mmap(bigary *restrict m_bigary, int fd, int flags, size_t max,
                      const MmapCallback *m_mmap)
{
    assert((BIGARY_PAGESIZE % TRACED_PAGESIZE) == 0 &&
           "bigary pagesize and traced pagesize are not aligned! "
           "BIGARY_PAGESIZE should be a multiply of TRACED_PAGESIZE");
    if (!max)
        max = BIGARY_DEFAULT_MAX;
    // round *max* up to pagesize
    size_t last_page_size = max % BIGARY_PAGESIZE;
    if (last_page_size) {
        size_t pages = max / BIGARY_PAGESIZE + 1;
        max = pages * BIGARY_PAGESIZE;
    }
    int ret = pthread_mutex_init(&m_bigary->enlargement, NULL);
    max += TRACED_PAGESIZE; // required to handle start address alignment
    if (ret != 0)
        die("mutex init failed\n");
    // use regular mmap instead of user_mmap - only address space reservation
    if ((m_bigary->area = mmap(0, max, PROT_NONE, flags, fd, 0)) == MAP_FAILED)
        die("mmapping bigary(%zd) failed: %m\n", max);

    // align start address to TRACED_PAGESIZE
    size_t traced_page_offset = ((uintptr_t)m_bigary->area) % TRACED_PAGESIZE;
    if (traced_page_offset) {
        size_t to_unmap = TRACED_PAGESIZE - traced_page_offset;
        munmap(m_bigary->area, to_unmap);
        m_bigary->area = ((uint8_t *)m_bigary->area) + to_unmap;
        max -= to_unmap;
    }
    m_bigary->declared = max;
    m_bigary->fd = fd;
    m_bigary->flags = flags;

    if (m_mmap->wrapped_mmap(m_mmap->arg, m_bigary->area, BIGARY_PAGESIZE,
                             PROT_READ | PROT_WRITE, MAP_FIXED | flags, fd,
                             0) == MAP_FAILED) {
        die("bigary alloc of %zd failed: %m\n", BIGARY_PAGESIZE);
    }
    m_bigary->top = BIGARY_PAGESIZE;
}

void bigary_destroy(bigary *restrict m_bigary)
{
    int ret1 = pthread_mutex_destroy(&m_bigary->enlargement);
    int ret2 = munmap(m_bigary->area, m_bigary->declared);
    assert(ret1 == 0 && "mutex destruction failed!");
    assert(ret2 == 0 && "unmap failed!");
}

void bigary_destroy_mmap(bigary *restrict m_bigary, const MmapCallback *m_mmap)
{
    int ret1 = pthread_mutex_destroy(&m_bigary->enlargement);
    int ret2 =
        m_mmap->wrapped_munmap(m_mmap->arg, m_bigary->area, m_bigary->declared);
    assert(ret1 == 0 && "mutex destruction failed!");
    assert(ret2 == 0 && "unmap failed!");
}

/********************************************************************/
/* ensure there's at least X space allocated                        */
/* (you may want MAP_POPULATE to ensure the space is actually there */
/********************************************************************/
void bigary_alloc(bigary *restrict m_bigary, size_t top)
{
    if (m_bigary->top >= top)
        return;
    pthread_mutex_lock(&m_bigary->enlargement);
    if (m_bigary->top >= top) // re-check
        goto done;
    top = (top + BIGARY_PAGESIZE - 1) & ~(BIGARY_PAGESIZE - 1); // align up
    if (top > m_bigary->declared)
        die("bigary's max is %zu, %zu requested.\n", m_bigary->declared, top);
    if (mmap(m_bigary->area + m_bigary->top, top - m_bigary->top,
             PROT_READ | PROT_WRITE, MAP_FIXED | m_bigary->flags, m_bigary->fd,
             0ul) == MAP_FAILED) {
        die("in-bigary alloc of %zu to %zu failed: %m\n", top - m_bigary->top,
            top);
    }
    m_bigary->top = top;
done:
    pthread_mutex_unlock(&m_bigary->enlargement);
}

/********************************************************************/
/* ensure there's at least X space allocated                        */
/* (you may want MAP_POPULATE to ensure the space is actually there */
/********************************************************************/
void bigary_alloc_mmap(bigary *restrict m_bigary, size_t top,
                       const MmapCallback *m_mmap)
{
    if (m_bigary->top >= top)
        return;
    pthread_mutex_lock(&m_bigary->enlargement);
    if (m_bigary->top >= top) // re-check
        goto done;
    top = (top + BIGARY_PAGESIZE - 1) & ~(BIGARY_PAGESIZE - 1); // align up
    if (top > m_bigary->declared)
        die("bigary's max is %zd, %zd requested.\n", m_bigary->declared, top);
    if (m_mmap->wrapped_mmap(m_mmap->arg, m_bigary->area + m_bigary->top,
                             top - m_bigary->top, PROT_READ | PROT_WRITE,
                             MAP_FIXED | m_bigary->flags, m_bigary->fd,
                             0ul) == MAP_FAILED) {
        die("in-bigary alloc of %zd to %zd failed: %m\n", top - m_bigary->top,
            top);
    }
    m_bigary->top = top;
done:
    pthread_mutex_unlock(&m_bigary->enlargement);
}
