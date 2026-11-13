#pragma once
#include <stdint.h>
typedef struct { uint32_t timerawh; uint32_t timerawl; } sim_timer_t;
/* Every look at the clock moves it on, so that lockouts run out. */
sim_timer_t *sim_timer_tick(void);
#define timer_hw (sim_timer_tick())
uint64_t time_us_64(void);
