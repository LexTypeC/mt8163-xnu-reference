#ifndef FIRE_VBLANK_CLOCK_H
#define FIRE_VBLANK_CLOCK_H
#include <stdint.h>
/* One IRQ writer;bounded lock-free readers. All fields use SC atomics so a
 * successful equal/even sequence read cannot combine two publications. The
 * counter counts observed real frame-start edges,not timer predictions. */
struct fvc_clock {uint64_t sequence,at;unsigned invalid;};
struct fvc_view {uint64_t counter,at;};
static inline int fvc_publish(struct fvc_clock *s,uint64_t at)
{
    uint64_t seq=__atomic_load_n(&s->sequence,__ATOMIC_SEQ_CST);
    if(__atomic_load_n(&s->invalid,__ATOMIC_SEQ_CST))return 0;
    if(!at || (seq&1) || seq>~(uint64_t)0-2 || at<=__atomic_load_n(&s->at,__ATOMIC_SEQ_CST)){
        __atomic_store_n(&s->invalid,1u,__ATOMIC_SEQ_CST);return 0;
    }
    __atomic_store_n(&s->sequence,seq+1,__ATOMIC_SEQ_CST);
    __atomic_store_n(&s->at,at,__ATOMIC_SEQ_CST);
    __atomic_store_n(&s->sequence,seq+2,__ATOMIC_SEQ_CST);return 1;
}
static inline int fvc_read(const struct fvc_clock *s,struct fvc_view *out)
{
    for(unsigned i=0;i<4;i++){
        if(__atomic_load_n(&s->invalid,__ATOMIC_SEQ_CST))return 0;
        uint64_t before=__atomic_load_n(&s->sequence,__ATOMIC_SEQ_CST);
        uint64_t at=__atomic_load_n(&s->at,__ATOMIC_SEQ_CST);
        uint64_t after=__atomic_load_n(&s->sequence,__ATOMIC_SEQ_CST);
        if(before && !(before&1) && before==after && !__atomic_load_n(&s->invalid,__ATOMIC_SEQ_CST)){
            out->counter=before/2;out->at=at;return at!=0;
        }
    }
    return 0;
}
static inline int fvc_deadline(uint64_t last,uint64_t now,uint64_t period,uint64_t slack,uint64_t *out)
{
    uint64_t maximum=~(uint64_t)0;
    if(!last || !period || !slack || last>now || last>maximum-period || last+period>maximum-slack)return 0;
    uint64_t next=last+period+slack;
    if(next<=now){if(now>maximum-slack)return 0;next=now+slack;}
    *out=next;return 1;
}
enum {FVC_WAIT=0,FVC_READY=1,FVC_STOPPED=2,FVC_FAILED=3};
enum {FVC_DEADLINE=0,FVC_ARM_SOURCE=1,FVC_CALLBACK_SOURCE=2,FVC_SEND_TIMEOUT=3,FVC_SEND_OTHER=4,FVC_FAULTS=5};
enum {FVC_SENT=0,FVC_POLL=1,FVC_STALE=2,FVC_START_WAIT=3,FVC_MISSED=4,FVC_ERROR=5,FVC_NOTES=6};
#ifdef __cplusplus
extern "C" {
#endif
int fire_display_vblank_snapshot(struct fvc_view *);
void fire_display_vblank_note(unsigned,uint64_t);
void fire_display_vblank_fault(unsigned,uint32_t);
#ifdef __cplusplus
}
#endif
#endif
