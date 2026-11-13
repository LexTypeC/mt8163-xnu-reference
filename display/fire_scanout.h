#ifndef FIRE_SCANOUT_H
#define FIRE_SCANOUT_H
#include <stdint.h>
/* Single IRQ writer, concurrent stage controls/readers. No allocation, waits,
 * rendering policy or panel-rate assumption. Units are native13MHz ticks. */
#define FSI_LOAD(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define FSI_STORE(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
#define FSI_ADD(p,v) __atomic_fetch_add((p),(v),__ATOMIC_RELAXED)
struct fsi_stage {
    uint64_t begin,end,started,completed,late,first,last,frames,ends,abnormal,underflow,target;
    uint64_t intervals,sum,min,max,hist[8];
    unsigned closed;
};
struct fsi_state {
    unsigned armed,active,initial,reason;
    uint64_t calls,initial_status,spurious,window,window_calls,handler_ticks,handler_max;
    struct fsi_stage stages[3];
};
static void fsi_begin(struct fsi_state *s,unsigned op,uint64_t at)
{
    if(!FSI_LOAD(&s->armed) || op<1 || op>3 || FSI_LOAD(&s->active))return;
    struct fsi_stage *p=&s->stages[op-1];
    if(FSI_LOAD(&p->begin))return; /* one original replay per boot */
    FSI_STORE(&p->begin,at);FSI_STORE(&s->active,op);
}
static void fsi_end(struct fsi_state *s,unsigned op,uint64_t at)
{
    if(op<1 || op>3 || FSI_LOAD(&s->active)!=op)return;
    FSI_STORE(&s->active,0);struct fsi_stage *p=&s->stages[op-1];
    FSI_STORE(&p->end,at);FSI_STORE(&p->closed,1);
}
/* Return0 means acknowledge only this observed,known status. Nonzero means
 * mask our GIC SPI without touching unknown peripheral status. First delivery
 * is deliberately excluded because its latched bits can predate attachment. */
static unsigned fsi_event(struct fsi_state *s,unsigned status,uint64_t at)
{
    if(!FSI_LOAD(&s->armed))return 1;
    FSI_ADD(&s->calls,1);
    if(!s->window || at<s->window || at-s->window>=13000){s->window=at;s->window_calls=0;}
    if(++s->window_calls>64){FSI_STORE(&s->reason,2);FSI_STORE(&s->armed,0);return 2;}
    if(status&~63u){FSI_STORE(&s->reason,3);FSI_STORE(&s->armed,0);return 3;}
    if(!s->initial){FSI_STORE(&s->initial_status,status);FSI_STORE(&s->initial,1);return 0;}
    if(!status){FSI_ADD(&s->spurious,1);return 0;}
    unsigned op=FSI_LOAD(&s->active);if(op<1 || op>3)return 0;
    struct fsi_stage *p=&s->stages[op-1];if(at<FSI_LOAD(&p->begin))return 0;
    FSI_ADD(&p->started,1);
    FSI_ADD(&p->ends,(status>>2)&1);FSI_ADD(&p->abnormal,(status>>3)&1);
    FSI_ADD(&p->underflow,(status>>4)&1);FSI_ADD(&p->target,(status>>5)&1);
    if(status&2){
        uint64_t last=FSI_LOAD(&p->last);
        if(last){
            if(at<=last)FSI_ADD(&p->late,1);
            else{
                uint64_t dt=at-last;unsigned bucket=0;
                const uint64_t limits[7]={13000,104000,195000,234000,325000,455000,650000};
                while(bucket<7 && dt>=limits[bucket])bucket++;
                FSI_ADD(&p->hist[bucket],1);FSI_ADD(&p->intervals,1);FSI_ADD(&p->sum,dt);
                if(!FSI_LOAD(&p->min) || dt<FSI_LOAD(&p->min))FSI_STORE(&p->min,dt);
                if(dt>FSI_LOAD(&p->max))FSI_STORE(&p->max,dt);
            }
        }else FSI_STORE(&p->first,at);
        FSI_STORE(&p->last,at);FSI_ADD(&p->frames,1);
    }
    if(FSI_LOAD(&s->active)!=op || FSI_LOAD(&p->closed))FSI_ADD(&p->late,1);
    /* A reader acquiring completed must also see every counter/histogram
     * update from this event,including a concurrent-close refusal. */
    __atomic_fetch_add(&p->completed,1,__ATOMIC_RELEASE);return 0;
}
#endif
