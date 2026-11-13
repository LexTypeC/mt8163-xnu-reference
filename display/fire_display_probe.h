#ifndef FIRE_DISPLAY_PROBE_H
#define FIRE_DISPLAY_PROBE_H
#include <stdint.h>
/* Exact karnak/MT8163 read-only configuration observer. There is deliberately
 * no write callback, polling, IRQ attachment or power/clock enable path. */
#define FDP_SPM 0x10006000u
#define FDP_INFRA 0x10001000u
#define FDP_TOP 0x10000000u
#define FDP_MM 0x14000000u
#define FDP_RDMA 0x14009000u
#define FDP_OVL 0x14007000u
struct fire_display_io {void *context;int (*read)(void *,uint32_t,unsigned,uint32_t *);};
struct fire_display_snapshot {
    unsigned gate,rdma_gate,ovl_gate,seen;
    uint32_t status,status2,power,top,protect,protect_status,cg0,cg1;
    uint32_t rdma[10],ovl[4],layer[4][6],route[10];
};
/* status/power/bus are always-on banks. Test them before every module bank;
 * a refused guard must never cause a powered-off module read. */
static unsigned fire_display_prerequisites(const struct fire_display_io *io,struct fire_display_snapshot *s)
{
#define FDP_READ(base,off,field) do {if(!io->read(io->context,base,off,&s->field))return 6;}while(0)
    FDP_READ(FDP_SPM,0x60c,status);FDP_READ(FDP_SPM,0x610,status2);FDP_READ(FDP_SPM,0x23c,power);
    s->seen|=1;
    if(!(s->status&8u) || !(s->status2&8u))return 1;
    if((s->power&0x1f1fu)!=0xdu)return 2;
    FDP_READ(FDP_TOP,0x40,top);s->seen|=2;
    if(s->top&(1u<<31))return 3;
    FDP_READ(FDP_INFRA,0x220,protect);FDP_READ(FDP_INFRA,0x228,protect_status);s->seen|=4;
    if((s->protect|s->protect_status)&2u)return 4;
    return 0;
#undef FDP_READ
}
static unsigned fire_display_module_gate(const struct fire_display_io *io,struct fire_display_snapshot *s,uint32_t bit)
{
    unsigned gate=fire_display_prerequisites(io,s);if(gate)return gate;
    if(!io->read(io->context,FDP_MM,0x100,&s->cg0))return 6;
    return s->cg0&(3u|bit)?7:0;
}
static void fire_display_collect(const struct fire_display_io *io,struct fire_display_snapshot *s)
{
    *s=(struct fire_display_snapshot){0};s->rdma_gate=s->ovl_gate=8;
    if(!io || !io->read){s->gate=6;return;}
    s->gate=fire_display_prerequisites(io,s);if(s->gate)return;
    if(!io->read(io->context,FDP_MM,0x100,&s->cg0) || !io->read(io->context,FDP_MM,0x110,&s->cg1)){s->gate=6;return;}
    s->seen|=8;
    static const unsigned route_offsets[10]={0x30,0x34,0x38,0x3c,0x58,0x60,0x64,0x6c,0x70,0x5c};
    for(unsigned i=0;i<10;i++)if(!io->read(io->context,FDP_MM,route_offsets[i],&s->route[i])){s->gate=6;return;}
    s->seen|=64;
    s->rdma_gate=fire_display_module_gate(io,s,1u<<12);
    if(!s->rdma_gate){
        static const unsigned offsets[10]={0,4,0x10,0x14,0x18,0x24,0x2c,0xf00,0xf4,0xfc};
        for(unsigned i=0;i<10;i++)if(!io->read(io->context,FDP_RDMA,offsets[i],&s->rdma[i])){s->rdma_gate=6;break;}
        if(!s->rdma_gate)s->seen|=16;
    }
    s->ovl_gate=fire_display_module_gate(io,s,1u<<10);
    if(!s->ovl_gate){
        static const unsigned offsets[4]={0xc,0x20,0x2c,0x24};
        for(unsigned i=0;i<4;i++)if(!io->read(io->context,FDP_OVL,offsets[i],&s->ovl[i])){s->ovl_gate=6;break;}
        for(unsigned i=0;i<4 && !s->ovl_gate;i++){
            const unsigned offsets4[6]={0x30+i*0x20,0x38+i*0x20,0x44+i*0x20,0xf40+i*0x20,0x3c+i*0x20,0xc0+i*0x20};
            for(unsigned j=0;j<6;j++)if(!io->read(io->context,FDP_OVL,offsets4[j],&s->layer[i][j])){s->ovl_gate=6;break;}
        }
        if(!s->ovl_gate)s->seen|=32;
    }
    /* Late prerequisite/read failures invalidate the combined configuration;
     * module clock-gated status7 is independent, not a global power failure. */
    if(s->rdma_gate && s->rdma_gate<7)s->gate=s->rdma_gate;
    if(s->ovl_gate && s->ovl_gate<7)s->gate=s->ovl_gate;
}
#endif
