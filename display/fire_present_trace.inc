/* Included in the native observer translation unit, outside board ownership.
 * One writer admission keeps the value row and its full-width TID together
 * across stage close. No board lock/MMIO/copies/waits or timer wakeups here. */
#include "fire_present_trace.h"
__attribute__((used)) const char fire_present_trace_identity[]="fire-buffered-present-trace-v1";
void fire_present_trace(const struct fire_present_trace_event *e)
{
    if(!e || !fire_measure_enabled())return;
    FIRE_PROFILE_WRITER_SCOPE;
    unsigned kind=FNM_PRESENT_SEED+e->kind-1;
    if(e->kind<FPR_TRACE_SEED || e->kind>FPR_TRACE_CLEARED || e->slot>=4 || !e->token || !e->tid){
        fire_measure_event(FNM_LAST+1,e->at,e->end,e->token,0,0,0,0);return;
    }
    fire_measure_event(kind,e->at,e->end,e->token,e->copy_token,
        e->swap|((uint64_t)e->slot<<32),e->pa|((uint64_t)e->status<<32),e->flow);
    fire_measure_event(FNM_PRESENT_META,e->at,e->end,e->token,kind,e->slot,e->tid,0);
}
