/* Opt-in exact iPod4,1/10B329 diagnostic lineage. Value-only wire identities. */
#ifndef FIRE_LINK_H
#define FIRE_LINK_H
#include "fire_measure.h"
#ifdef __cplusplus
extern "C" {
#endif
#if defined(FIRE_PRESENTATION_LINK)
uint64_t fire_link_copy_begin(unsigned,unsigned);
void fire_link_copy_end(uint64_t);
void fire_link_store_complete(void);
void fire_link_irq(uint64_t,uint64_t,uint64_t,unsigned,unsigned);
void fire_link_render(unsigned);
void fire_link_process_exit(unsigned,unsigned);
uint64_t fire_link_send_begin(uint64_t,unsigned,unsigned,unsigned);
void fire_link_send_end(uint64_t,unsigned);
unsigned fire_link_trace(unsigned,unsigned,uint64_t,uint64_t,uint64_t,uint64_t);
#else
static inline uint64_t fire_link_copy_begin(unsigned s,unsigned r){(void)s;(void)r;return 0;}
static inline void fire_link_copy_end(uint64_t t){(void)t;}
static inline void fire_link_store_complete(void){}
static inline void fire_link_render(unsigned s){(void)s;}
#endif
#ifdef __cplusplus
}
#endif
#endif
