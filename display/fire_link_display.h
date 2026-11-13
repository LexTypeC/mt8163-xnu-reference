/* Read-only MT8163 endpoint prerequisites. LARB0 identity from the inspected
 * device DT (14016000, larbid0); MMU_EN f00 bit0 is DISP_OVL0, vendor m4u/smi.
 * Never reconfigure a plane or M4U to make a measurement pass. */
#define FL_LARB0 0x14016000u
#if defined(FIRE_BUFFERED_PRESENTATION)
#include "fire_present.h"
#endif
static vm_offset_t fl_ovl_bank,fl_larb_bank;
static uint64_t fl_alpha_nonzero;
static unsigned fl_display_ready,fl_display_reason;
static uint32_t fl_layer_offsets[2][6];
static uint32_t fl_layer_expected[2][6];
static unsigned fl_display_check(void){
    if(!fl_display_ready){fl_display_reason=1;return 0;}
    struct fire_display_io io={NULL,fso_read};struct fire_display_snapshot gate={0};
    if(fire_display_module_gate(&io,&gate,1u<<10)){fl_display_reason=2;return 0;}
    if(fu_r32(fl_larb_bank,0xf00)&1u){fl_display_reason=3;return 0;} /* translated address remains unknown */
    if(fu_r32(fl_ovl_bank,0xc)!=1 || fu_r32(fl_ovl_bank,0x20)!=0x05000320 || fu_r32(fl_ovl_bank,0x2c)!=9){fl_display_reason=4;return 0;}
    const unsigned ro[10]={0x30,0x34,0x38,0x3c,0x58,0x60,0x64,0x6c,0x70,0x5c};
    const unsigned rv[10]={1,0,1,1,1,0,0,0,0,0};
    for(unsigned i=0;i<10;i++)if(fu_r32(fso_banks[3],ro[i])!=rv[i]){fl_display_reason=10+i;return 0;}
    /* Vendor ovl_get_address tests RDMA_CTRL bit0; upper changing state is
     * not layer enable. Geometry/format/addresses retain exact stable checks. */
    for(unsigned i=0;i<2;i++)for(unsigned j=0;j<6;j++){
        uint32_t observed=fu_r32(fl_ovl_bank,fl_layer_offsets[i][j]);
#if defined(FIRE_BUFFERED_PRESENTATION)
        /* Only checked front/pending ownership can authorize dynamic L0_ADDR.
         * Pool membership alone never admits FREE/DRAWING/READY addresses. */
        if(!i && j==3){if(!fire_present_address_allowed(observed)){fl_display_reason=33;return 0;}continue;}
#endif
        if((observed&(j==5?1u:~0u))!=(fl_layer_expected[i][j]&(j==5?1u:~0u))){fl_display_reason=30+i*6+j;return 0;}
    }
    fl_display_reason=fl_alpha_nonzero?5:0;return !fl_alpha_nonzero;
}
static void fl_display_start(void){
    /* Existing collect has already checked power, clocks, geometry and route.
     * No clock enabling, writes or motion-time buffer scanning. */
    fl_ovl_bank=ml_io_map(FDP_OVL,4096);fl_larb_bank=ml_io_map(FL_LARB0,4096);
    if(!fl_ovl_bank || !fl_larb_bank)return;
    for(unsigned i=0;i<2;i++){
        unsigned l=i?3:0;const unsigned off[6]={0x30+l*0x20,0x38+l*0x20,0x44+l*0x20,0xf40+l*0x20,0x3c+l*0x20,0xc0+l*0x20};
        for(unsigned j=0;j<6;j++){fl_layer_offsets[i][j]=off[j];fl_layer_expected[i][j]=fu_r32(fl_ovl_bank,off[j]);}
        if(fl_layer_expected[i][0]!=0x010021ff || fl_layer_expected[i][1]!=0x05000320 ||
           (fl_layer_expected[i][2]&0xffff)!=3200 || fl_layer_expected[i][3]!=(i?0x98be8000:0x98800000) || fl_layer_expected[i][4])return;
    }
    unsigned mmu=fu_r32(fl_larb_bank,0xf00);
    char initial[256];snprintf(initial,sizeof(initial),"fire-link-display: preflight mmu=%u source=%u con0=%u con3=%u address0=%u address3=%u\n",mmu,fu_r32(fl_ovl_bank,0x2c),fl_layer_expected[0][0],fl_layer_expected[1][0],fl_layer_expected[0][3],fl_layer_expected[1][3]);loader_log_line(initial);
    if(mmu&1)return; /* Do not read a presumed physical alias of an IOVA. */
    volatile uint32_t *top=(volatile uint32_t *)ml_io_map(0x98be8000,800*1280*4);
    if(!top)return;
    for(unsigned i=0;i<800*1280;i++)fl_alpha_nonzero+=(top[i]>>24)!=0;
    fl_display_ready=1;
    unsigned ready=fl_display_check();
    char detail[256];snprintf(detail,sizeof(detail),"fire-link-display: rdma0_initial=%#x live=%#x rdma3_initial=%#x live=%#x\n",fl_layer_expected[0][5],fu_r32(fl_ovl_bank,0xc0),fl_layer_expected[1][5],fu_r32(fl_ovl_bank,0x120));loader_log_line(detail);
    char line[512];snprintf(line,sizeof(line),"fire-link-display: version=1 mmu=%u alpha_nonzero=%llu ready=%u reason=%u en=%#x roi=%#x source=%#x\n",mmu,fl_alpha_nonzero,ready,fl_display_reason,fu_r32(fl_ovl_bank,0xc),fu_r32(fl_ovl_bank,0x20),fu_r32(fl_ovl_bank,0x2c));loader_log_line(line);
}
