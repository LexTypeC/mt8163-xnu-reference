#ifndef FIRE_PRESENT_TRACE_H
#define FIRE_PRESENT_TRACE_H
#include <stdint.h>
/* Internal immutable records. PREPARED is private CPU preparation; REQUEST is
 * an address programming request; only CONSUMED identifies a qualified later
 * hardware event. None alone establishes whole-frame atomicity or photons.
 * Snapshot under the ownership lock and emit after releasing it. The native
 * telemetry sink/decoder must be implemented before enabling the adapter. */
enum fire_present_trace_kind {
    FPR_TRACE_SEED=1,FPR_TRACE_CACHE,FPR_TRACE_PREPARED,FPR_TRACE_ABORT,
    FPR_TRACE_CONSUMED,FPR_TRACE_REQUEST,FPR_TRACE_CLEARED
};
struct fire_present_trace_event {
    uint32_t kind,slot,status,flow;
    uint64_t at,end,token,copy_token;
    uint32_t swap,pa;
    uint64_t tid;
};
void fire_present_trace(const struct fire_present_trace_event *);
#endif
