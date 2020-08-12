
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


void
ngx_spinlock(ngx_atomic_t *lock, ngx_atomic_int_t value, ngx_uint_t spin)
{

#if (NGX_HAVE_ATOMIC_OPS)

    ngx_uint_t  i, n;

    for ( ;; ) {

        if (*lock == 0 && ngx_atomic_cmp_set(lock, 0, value)) {
            return;
        }

        /*
         * QUESTION: 为什么只在 cpu 的个数大于 1 的时候自旋？
         *
         * NOTE: 如果只有 1 个 cpu，那么由于 worker 的个数和 cpu 的个数是一致的，
         *       所以自旋的话，就没法处理网络请求了
         */
        if (ngx_ncpu > 1) {

            for (n = 1; n < spin; n <<= 1) {

                for (i = 0; i < n; i++) {
                    ngx_cpu_pause();
                }

                if (*lock == 0 && ngx_atomic_cmp_set(lock, 0, value)) {
                    return;
                }
            }
        }

        ngx_sched_yield();
    }

#else

#if (NGX_THREADS)

#error ngx_spinlock() or ngx_atomic_cmp_set() are not defined !

#endif

#endif

}
