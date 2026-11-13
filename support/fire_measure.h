/* Versioned passive native evidence; native ticks, value-only IDs. */
#ifndef FIRE_MEASURE_H
#define FIRE_MEASURE_H
#include <stdint.h>
#ifndef FCP_BUILD_ENABLED
#include "fire_chain_profile_config.h"
#endif
#if defined(FIRE_BUFFERED_COVERED_SEED) && (!defined(FIRE_BUFFERED_PRESENTATION) || !defined(FIRE_PRESENTATION_LINK) || !defined(FIRE_NATIVE_MEASURE))
#error "Covered seed requires the explicit buffered native wire9 observer"
#endif
#if defined(FIRE_BUFFERED_PRESENTATION)
#define FNM_ROWS 65536u
#define FNM_LAST 24u
#elif defined(FIRE_PRESENTATION_LINK)
#define FNM_ROWS 32768u
#define FNM_LAST 15u
#else
#define FNM_ROWS 16384u
#define FNM_LAST FNM_MEMORY
#endif
#define FNM_IRQ 1u
#define FNM_GOODIX 2u
#define FNM_POST 3u
#define FNM_READ 4u
#define FNM_IDLE 5u
#define FNM_MEMORY 6u
#define FNM_PRIVATE_COPY 16u
#define FNM_PRESENT_SEED 17u
#define FNM_PRESENT_META 24u
struct fnm_row {uint64_t at,end,kind,id,a,b,c,d;};
#if defined(FIRE_NATIVE_MEASURE)
extern void fire_measure_event(unsigned,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
extern unsigned fire_measure_enabled(void);
#define fire_measure_clock() mach_absolute_time()
#else
static inline void fire_measure_event(unsigned k,uint64_t at,uint64_t end,uint64_t id,
    uint64_t a,uint64_t b,uint64_t c,uint64_t d){
    (void)k;(void)at;(void)end;(void)id;(void)a;(void)b;(void)c;(void)d;
}
#define fire_measure_enabled() 0u
#define fire_measure_clock() 0ull
#endif
#endif
