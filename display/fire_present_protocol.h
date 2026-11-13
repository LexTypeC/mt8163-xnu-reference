#ifndef FIRE_PRESENT_PROTOCOL_H
#define FIRE_PRESENT_PROTOCOL_H
#include <stdint.h>
#include "fire_present_buffers.h"
/* MT8163 only. Software qualification for real OVL0 IRQ/MMIO observations.
 * Caller owns the checked route, pool and one IRQ-safe lock. Allocation/cache
 * visibility and FPB lifetime gates precede a request; this file performs no IO.
 * Request at EOF/idle, write L0_ADDR, complement-ack the IRQ and read back a
 * cleared EOF bit. Only a later real EOF/commit/idle event with the requested
 * address can complete. No timer, write readback or stale IRQ substitutes for it.
 * Native433/434 establish source consumption for this exact pipeline. The UI
 * adapter and real acknowledgement readback still require native validation. */
#define FPP_BYTES 4096000u
struct fpp_event {uint64_t irq,at;uint32_t status,flow,address,config_reason;};
struct fpp_request {uint64_t token,irq,at,clear_irq,clear_at;unsigned slot,cleared;};
struct fpp {
    uint32_t addresses[FPB_SLOTS];
    unsigned configured,front,open;
    uint64_t completed_token,last_irq,last_at;
    struct fpp_event event;
    struct fpp_request pending;
};
struct fpp_completion {uint64_t token,irq,at;unsigned old_slot,new_slot;uint32_t extra_status;};
static int fpp_idle(const struct fpp_event *e)
{
    return e->irq && e->at && !e->config_reason && (e->status&3u)==3u &&
        (e->flow&0x3ffu)==2u && (e->flow&0xf0000u)==0xf0000u;
}
static int fpp_init(struct fpp *p,const uint32_t addresses[FPB_SLOTS])
{
    if(addresses[0]!=0x98800000u)return 0;
    for(unsigned i=1;i<FPB_SLOTS;i++){
        uint64_t pa=addresses[i];
        if(!pa || (pa&4095u) || pa+FPP_BYTES>0x100000000ull ||
           (pa<0x98fd0000ull && pa+FPP_BYTES>0x98800000ull))return 0;
        for(unsigned j=1;j<i;j++)if(pa<(uint64_t)addresses[j]+FPP_BYTES && pa+FPP_BYTES>addresses[j])return 0;
    }
    *p=(struct fpp){0};
    for(unsigned i=0;i<FPB_SLOTS;i++)p->addresses[i]=addresses[i];
    p->configured=1;return 1;
}
/* One bounded invocation of the actual IRQ handler. Closing it prevents later
 * callers from reusing an old idle observation to authorize an address write. */
static int fpp_open_event(struct fpp *p,const struct fpp_event *e)
{
    if(!p->configured || p->open || !e->irq || !e->at || e->irq<=p->last_irq || e->at<=p->last_at)return 0;
    p->event=*e;p->last_irq=e->irq;p->last_at=e->at;p->open=1;return 1;
}
static int fpp_close_event(struct fpp *p)
{
    if(!p->open)return 0;p->open=0;return 1;
}
/* This is authorization to perform the one address write, not its completion.
 * A completed transaction and the next request may share one real EOF event. */
static int fpp_request_begin(struct fpp *p,uint64_t token,unsigned slot)
{
    const struct fpp_event *e=&p->event;
    if(!p->configured || !p->open || p->pending.token || !token || token<=p->completed_token ||
       slot>=FPB_SLOTS || slot==p->front || !fpp_idle(e) ||
       e->address!=p->addresses[p->front])return 0;
    p->pending=(struct fpp_request){.token=token,.irq=e->irq,.at=e->at,.slot=slot};
    return 1;
}
/* Read actual INTSTA after the complement-ack/DSB. A stuck EOF cannot arm a
 * new transaction. If clearing takes a later IRQ, require yet another EOF. */
static int fpp_mark_cleared(struct fpp *p,uint64_t token,uint64_t at,uint32_t status)
{
    struct fpp_request *r=&p->pending;
    if(!p->open || !token || token!=r->token || r->cleared ||
       p->event.irq<r->irq || at<p->event.at || (status&2u))return 0;
    r->clear_irq=p->event.irq;r->clear_at=at;r->cleared=1;return 1;
}
static int fpp_complete(struct fpp *p,uint64_t token,struct fpp_completion *out)
{
    struct fpp_request *r=&p->pending;
    const struct fpp_event *e=&p->event;
    if(!p->open || !token || token!=r->token || !r->cleared || !fpp_idle(e) ||
       e->irq<=r->clear_irq || e->at<=r->clear_at || e->address!=p->addresses[r->slot])return 0;
    *out=(struct fpp_completion){token,e->irq,e->at,p->front,r->slot,e->status&~3u};
    p->front=r->slot;p->completed_token=token;
    p->pending=(struct fpp_request){0};return 1;
}
/* Strict measurement guard: only currently owned front or pending scanout.
 * A free/drawing/merely-ready pool address is not admitted by membership. */
static int fpp_address_allowed(const struct fpp *p,uint32_t address)
{
    return p->configured && (address==p->addresses[p->front] ||
        (p->pending.token && address==p->addresses[p->pending.slot]));
}
#endif
