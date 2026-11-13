#ifndef FIRE_PRESENT_BUFFERS_H
#define FIRE_PRESENT_BUFFERS_H
#include <stdint.h>
/* Board-owned completed-frame ownership, not a hardware latch implementation.
 * Caller serializes each state transition with one ownership lock. Leases
 * span pixel copies; do not hold an IRQ-safe lock during copying or waiting.
 * Slot0 is the borrowed original scanout and is never a drawing destination.
 * Other slots must be private, contiguous, physically addressable, full images.
 * GPU/read leases, worker joins and cache cleaning precede publish().
 * latch()/retire() require real board evidence for the exact queued token.
 * A register write/readback, elapsed delay or synthetic beat is NOT that evidence.
 * Deliberately not wired into the runtime until that evidence is validated. */
#define FPB_SLOTS 4
#define FPB_NONE FPB_SLOTS
#define FPB_READ_LEASES 8
enum fpb_state {FPB_FREE,FPB_DRAWING,FPB_READY,FPB_PENDING,FPB_FRONT,FPB_RETIRING,FPB_RETIRED};
struct fpb_slot {unsigned state,readers;uint64_t content,token;};
struct fpb_read_lease {unsigned slot;uint64_t serial,content;};
struct fpb {
    struct fpb_slot slot[FPB_SLOTS];
    struct fpb_read_lease reads[FPB_READ_LEASES];
    unsigned front,pending,ready,drawing,seed,latest,stopping;
    uint64_t content,token,read_serial;
};
static void fpb_release(struct fpb *p,unsigned i)
{
    if(i<FPB_SLOTS && p->slot[i].state==FPB_RETIRED &&
       !p->slot[i].readers && p->latest!=i)p->slot[i].state=FPB_FREE;
}
static void fpb_init(struct fpb *p)
{
    *p=(struct fpb){0};p->front=p->latest=0;
    p->pending=p->ready=p->drawing=p->seed=FPB_NONE;
    p->slot[0].state=FPB_FRONT;
}
/* Scene/checkpoint/readback consumers pin an immutable complete image. Unique
 * serials make duplicate/stale release refuse, including after slot reuse.
 * These leases are separate from GPU/IOSurface and seed-copy lifetimes. */
static int fpb_read_begin(struct fpb *p,unsigned slot,struct fpb_read_lease *lease)
{
    if(p->stopping || slot>=FPB_SLOTS || p->slot[slot].state==FPB_FREE ||
       p->slot[slot].state==FPB_DRAWING || p->read_serial==UINT64_MAX ||
       p->slot[slot].readers==UINT32_MAX)return 0;
    unsigned i;
    for(i=0;i<FPB_READ_LEASES;i++)if(!p->reads[i].serial)break;
    if(i==FPB_READ_LEASES)return 0;
    p->reads[i]=(struct fpb_read_lease){slot,++p->read_serial,p->slot[slot].content};
    p->slot[slot].readers++;*lease=p->reads[i];return 1;
}
static int fpb_read_end(struct fpb *p,const struct fpb_read_lease *lease)
{
    if(!lease->serial || lease->slot>=FPB_SLOTS)return 0;
    for(unsigned i=0;i<FPB_READ_LEASES;i++)if(p->reads[i].serial==lease->serial){
        unsigned slot=lease->slot;
        if(p->reads[i].slot!=slot || p->reads[i].content!=lease->content ||
           p->slot[slot].content!=lease->content || !p->slot[slot].readers)return 0;
        p->reads[i].serial=0;p->slot[slot].readers--;fpb_release(p,slot);return 1;
    }
    return 0;
}
/* Reserve a destination and a read lease on its complete predecessor. The
 * destination must be fully seeded before any partial-layer/dirty-row writes. */
static int fpb_begin(struct fpb *p,unsigned *destination,unsigned *seed)
{
    if(p->stopping || p->drawing!=FPB_NONE || p->ready!=FPB_NONE || p->latest>=FPB_SLOTS)return 0;
    unsigned i;
    for(i=1;i<FPB_SLOTS;i++)if(p->slot[i].state==FPB_FREE)break;
    if(i==FPB_SLOTS)return 0;
    p->drawing=i;p->seed=p->latest;p->slot[i].state=FPB_DRAWING;
    p->slot[p->seed].readers++;*destination=i;*seed=p->seed;return 1;
}
/* Only after the seed copy has finished; retiring its source must not race it. */
static int fpb_seed_done(struct fpb *p,unsigned destination)
{
    if(destination!=p->drawing || destination>=FPB_SLOTS || p->seed>=FPB_SLOTS)return 0;
    unsigned i=p->seed;p->slot[i].readers--;p->seed=FPB_NONE;fpb_release(p,i);return 1;
}
static int fpb_publish(struct fpb *p,unsigned destination)
{
    if(p->stopping || destination>=FPB_SLOTS || destination!=p->drawing ||
       p->seed!=FPB_NONE || p->ready!=FPB_NONE || p->content==UINT64_MAX)return 0;
    unsigned previous=p->latest;
    p->slot[destination].content=++p->content;p->slot[destination].state=FPB_READY;
    p->ready=p->latest=destination;p->drawing=FPB_NONE;fpb_release(p,previous);return 1;
}
/* Call only once all CPU drawers have stopped, including on shutdown. */
static int fpb_abort_draw(struct fpb *p,unsigned destination)
{
    if(destination>=FPB_SLOTS || destination!=p->drawing)return 0;
    if(p->seed!=FPB_NONE)fpb_seed_done(p,destination);
    p->slot[destination].state=FPB_FREE;p->drawing=FPB_NONE;return 1;
}
static int fpb_queue(struct fpb *p,uint64_t *token)
{
    if(p->stopping || p->pending!=FPB_NONE || p->ready>=FPB_SLOTS || p->token==UINT64_MAX)return 0;
    unsigned i=p->ready;p->slot[i].state=FPB_PENDING;p->slot[i].token=++p->token;
    p->pending=i;p->ready=FPB_NONE;*token=p->token;return 1;
}
/* Exact hardware latch acknowledgement. The old front remains owned until a
 * separate real retirement acknowledgement, even if software sees the new PA. */
static int fpb_latch(struct fpb *p,uint64_t token)
{
    if(p->pending>=FPB_SLOTS || !token || p->slot[p->pending].token!=token)return 0;
    p->slot[p->front].state=FPB_RETIRING;
    p->front=p->pending;p->pending=FPB_NONE;p->slot[p->front].state=FPB_FRONT;return 1;
}
/* Token identifies that slot's last scanout use, not the replacement's token.
 * Original slot0 uses token0 for its initial inherited scanout. */
static int fpb_retire(struct fpb *p,unsigned i,uint64_t token)
{
    if(i>=FPB_SLOTS || p->slot[i].state!=FPB_RETIRING || p->slot[i].token!=token)return 0;
    p->slot[i].state=FPB_RETIRED;fpb_release(p,i);return 1;
}
static void fpb_stop(struct fpb *p)
{
    p->stopping=1;unsigned previous=p->latest;p->latest=FPB_NONE;
    if(p->ready<FPB_SLOTS){
        unsigned i=p->ready;p->slot[i].state=FPB_RETIRED;p->ready=FPB_NONE;
        fpb_release(p,i);
    }
    fpb_release(p,previous);
}
/* Drain an existing pending handoff first. Never redirect midway through a
 * scan or while drawers run, and never infer success from this queue operation. */
static int fpb_restore(struct fpb *p,uint64_t *token)
{
    if(!p->stopping || p->drawing!=FPB_NONE || p->pending!=FPB_NONE ||
       p->front==0 || p->slot[0].state!=FPB_FREE || p->slot[0].readers || p->token==UINT64_MAX)return 0;
    p->slot[0].state=FPB_PENDING;p->slot[0].token=++p->token;
    p->pending=0;*token=p->token;return 1;
}
static int fpb_drained(const struct fpb *p)
{
    if(!p->stopping || p->front!=0 || p->pending!=FPB_NONE || p->ready!=FPB_NONE || p->drawing!=FPB_NONE)return 0;
    for(unsigned i=0;i<FPB_SLOTS;i++)if(p->slot[i].readers || (i && p->slot[i].state!=FPB_FREE))return 0;
    return p->slot[0].state==FPB_FRONT;
}
#endif
