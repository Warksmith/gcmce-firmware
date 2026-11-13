#pragma once
#include <stddef.h>
#include <stdint.h>
typedef volatile uint32_t spin_lock_t;
void sim_lock(void);
void sim_unlock(void);
static inline spin_lock_t *spin_lock_init(unsigned n) { static spin_lock_t l; (void)n; return &l; }
static inline unsigned spin_lock_claim_unused(int required) { (void)required; return 0; }
static inline void spin_lock_unsafe_blocking(spin_lock_t *l) { (void)l; sim_lock(); }
static inline void spin_unlock_unsafe(spin_lock_t *l) { (void)l; sim_unlock(); }
