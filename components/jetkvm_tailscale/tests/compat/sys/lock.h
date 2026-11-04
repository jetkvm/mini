#pragma once
/* Host tests run single-threaded. */
typedef int _lock_t;
static inline void _lock_acquire(_lock_t *lock) { (void)lock; }
static inline void _lock_release(_lock_t *lock) { (void)lock; }
