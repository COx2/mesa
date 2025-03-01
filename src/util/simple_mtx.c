/*
 * Copyright 2022 Yonggang Luo
 * SPDX-License-Identifier: MIT
 *
 */

#include "simple_mtx.h"

#if !UTIL_FUTEX_SUPPORTED

void _simple_mtx_plain_init_once(simple_mtx_t *mtx)
{
   mtx_init(&mtx->mtx, mtx_plain);
}

void
simple_mtx_init(simple_mtx_t *mtx, ASSERTED int type)
{
   const once_flag once = ONCE_FLAG_INIT;
   assert(type == mtx_plain);
   mtx->initialized = false;
   mtx->once = once;
   _simple_mtx_init_with_once(mtx);
}

void
simple_mtx_destroy(simple_mtx_t *mtx)
{
   if (mtx->initialized) {
      mtx_destroy(&mtx->mtx);
   }
}

#endif /* !UTIL_FUTEX_SUPPORTED */
