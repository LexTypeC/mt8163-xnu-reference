#ifndef FIRE_PRESENT_H
#define FIRE_PRESENT_H
#include <stdint.h>
/* Internal Fire renderer/board contract; no donor or IOUserClient ABI change.
 * Disabled=0 uses the synchronous boot framebuffer. Active=1 requires owned
 * complete buffers. Failure=-1 must not redirect drawing to a live old buffer.
 * CPU preparation/cache visibility and hardware consumption are distinct. */
#ifdef __cplusplus
extern "C" {
#endif
struct fire_present_frame {uint8_t *pixels;uint64_t serial;uint32_t slot,prepared;};
struct fire_present_read {const uint8_t *pixels;uint64_t serial,content;uint32_t slot;};
int fire_present_mode(void);
int fire_present_address_allowed(uint32_t);
unsigned fire_present_profile_pending(void);
int fire_present_begin(struct fire_present_frame *,unsigned timeout_ms);
#if defined(FIRE_BUFFERED_COVERED_SEED)
int fire_present_begin_covered(struct fire_present_frame *,unsigned timeout_ms,unsigned covered_rows);
#endif
int fire_present_prepare(struct fire_present_frame *);
int fire_present_publish(struct fire_present_frame *,unsigned swap,uint64_t copy_token);
void fire_present_abort(struct fire_present_frame *);
int fire_present_front_begin(struct fire_present_read *);
void fire_present_front_end(struct fire_present_read *);
#ifdef __cplusplus
}
/* The caller holds the framebuffer lock; drawLayer has joined all helpers
 * before prepare/abort. Existing GPU/IOSurface read leases surround this scope. */
class FirePresentDraw {
    uint8_t *&target;
    uint8_t *saved;
    fire_present_frame frame;
    int state;
    bool prepared,finished;
    void abort() {
        if(state>0){fire_present_abort(&frame);target=saved;state=0;}
    }
public:
    FirePresentDraw(uint8_t *&base,unsigned timeout_ms,unsigned covered_rows=0):target(base),saved(base),frame{},state(0),prepared(false),finished(false) {
#if defined(FIRE_BUFFERED_COVERED_SEED)
        state=fire_present_begin_covered(&frame,timeout_ms,covered_rows);
#else
        (void)covered_rows;
        state=fire_present_begin(&frame,timeout_ms);
#endif
        if(state>0){
            if(!frame.pixels || !frame.serial || frame.slot>=4){fire_present_abort(&frame);state=-1;}
            else target=frame.pixels;
        }
    }
    ~FirePresentDraw(){abort();}
    bool valid() const {return !finished && state>=0;}
    bool prepare() {
        if(finished)return false;
        if(state==0)return true;
        if(state<0)return false;
        if(!prepared)prepared=fire_present_prepare(&frame)>0;
        return prepared;
    }
    bool publish(unsigned swap,uint64_t copy_token) {
        if(finished)return false;
        finished=true;
        if(state==0)return true;
        if(state<0)return false;
        if(!prepared){abort();return false;}
        bool okay=fire_present_publish(&frame,swap,copy_token)>0;
        if(!okay)fire_present_abort(&frame);
        target=saved;state=0;return okay;
    }
    FirePresentDraw(const FirePresentDraw&)=delete;
    FirePresentDraw& operator=(const FirePresentDraw&)=delete;
};
/* Front capture uses a board lease, independent of the rendering destination.
 * The adapter must wait for an acknowledged stable view, never return a merely
 * programmed address as proof of the current front. The lease ends after the
 * one frozen image/hash is copied, before USB chunks. */
class FirePresentRead {
    fire_present_read read;
    bool pinned;
public:
    explicit FirePresentRead(const uint8_t *fallback):read{},pinned(false) {
        int mode=fire_present_mode();
        if(mode==0)read.pixels=fallback;
        else if(mode>0)pinned=fire_present_front_begin(&read)>0;
        if(mode!=0 && !pinned)read.pixels=nullptr;
    }
    ~FirePresentRead(){if(pinned)fire_present_front_end(&read);}
    const uint8_t *pixels() const {return read.pixels;}
    uint64_t content() const {return read.content;}
    uint64_t lease() const {return read.serial;}
    uint32_t slot() const {return read.slot;}
    FirePresentRead(const FirePresentRead&)=delete;
    FirePresentRead& operator=(const FirePresentRead&)=delete;
};
/* Same384 sparse samples as the existing presenter observer, now from an
 * independently leased immutable front. This is source memory,not photons. */
static inline void fire_present_front_hashes(const uint8_t *pixels,uint32_t hashes[4]) {
    for(unsigned i=0;i<4;i++)hashes[i]=2166136261u;
    for(unsigned gy=0;gy<24;gy++)for(unsigned gx=0;gx<16;gx++){
        unsigned y=(gy+1)*1280/25,x=(gx+1)*800/17;
        uint32_t pixel=*(const volatile uint32_t *)(pixels+y*3200+x*4);
        hashes[gy/6]=(hashes[gy/6]^(pixel&0xffffffu))*16777619u;
    }
}
#endif
#endif
