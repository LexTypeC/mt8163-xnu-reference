/* Exact live DT: sysirq SPI184/level-low -> GIC216/level-high.
 * Exclusive disabled/unregistered IRQ only. Preserve neighboring polarity bits
 * and restore routing; acknowledged latched peripheral status cannot be undone. */
static vm_offset_t fire_ovl_intpol;
int fire_ovl_gic_restore(uint32_t saved[8]);
uint64_t fire_ovl_gic_status(void)
{
    const unsigned id=216,bit=1u<<24,word=24,shift=(216%16)*2;
    if(!fire_dist || !fire_ovl_intpol)return 0;
    return (uint64_t)((fr(fire_dist,FD_ENABLE+word)&bit)!=0) |
        (uint64_t)((fr(fire_dist,FD_GROUP+word)&bit)!=0)<<1 |
        (uint64_t)*(volatile uint8_t *)(fire_dist+FD_TARGET+id)<<8 |
        (uint64_t)((fr(fire_dist,FD_CONFIG+4*(id/16))>>shift)&3)<<16 |
        (uint64_t)((fr(fire_ovl_intpol,0x634)&bit)!=0)<<18 |
        (uint64_t)((fr(fire_dist,0x200+word)&bit)!=0)<<19 |
        (uint64_t)((fr(fire_dist,0x300+word)&bit)!=0)<<20 |
        (uint64_t)*(volatile uint8_t *)(fire_dist+FD_PRIORITY+id)<<24;
}
int fire_ovl_gic_attach(uint32_t saved[8],void(*handler)(void *,uint32_t),void *context)
{
    const unsigned id=216,bit=1u<<24,word=24,shift=(216%16)*2;
    if(!fire_dist || !saved || saved[0] || !handler)return KERN_INVALID_ARGUMENT;
    if(fr(fire_dist,FD_ENABLE+word)&bit)return KERN_NAME_EXISTS;
    unsigned free_slot=0;
    for(unsigned i=0;i<48;i++){if(fire_spis[i].id==id)return KERN_NAME_EXISTS;if(!fire_spis[i].id)free_slot=1;}
    if(!free_slot)return KERN_RESOURCE_SHORTAGE;
    if(!fire_ovl_intpol)fire_ovl_intpol=ml_io_map(0x10200000,4096);
    if(!fire_ovl_intpol)return KERN_RESOURCE_SHORTAGE;
    saved[1]=(fr(fire_dist,FD_GROUP+word)&bit)!=0;
    saved[2]=*(volatile uint8_t *)(fire_dist+FD_PRIORITY+id);
    saved[3]=*(volatile uint8_t *)(fire_dist+FD_TARGET+id);
    saved[4]=(fr(fire_dist,FD_CONFIG+4*(id/16))>>shift)&3;
    saved[5]=0;saved[6]=(fr(fire_ovl_intpol,0x634)&bit)!=0;saved[7]=0;saved[0]=id;
    /* Vendor sysirq_set_type: invert only SPI184 before enabling GIC216. */
    fw(fire_ovl_intpol,0x634,fr(fire_ovl_intpol,0x634)|bit);
    saved[7]=(fr(fire_ovl_intpol,0x634)&bit)!=0;
    if(!saved[7]){fire_ovl_gic_restore(saved);return KERN_FAILURE;}
    int result=gic_v3_spi_attach(id,false,handler,context);
    if(result!=KERN_SUCCESS){
        fw(fire_ovl_intpol,0x634,(fr(fire_ovl_intpol,0x634)&~bit)|(saved[6]?bit:0));
        if(((fr(fire_ovl_intpol,0x634)&bit)!=0)==saved[6])saved[0]=0;
        return result;
    }
    uint64_t live=fire_ovl_gic_status();saved[5]=(uint32_t)live;
    /* IGROUPR can read-as-zero/write-ignore from nonsecure EL1. Enable,
     * target,trigger,priority/polarity readback plus actual IRQ delivery are
     * the acceptance gates; the raw group view is advisory. */
    /* ICFGR Int_config[1] is the trigger bit. Int_config[0] is reserved/model
     * information and can read1 for a correctly level-sensitive interrupt. */
    if(!(live&1) || ((live>>8)&255)!=fire_target[master_cpu] || ((live>>17)&1) || !((live>>18)&1) || (live>>24)!=0xa0){
        gic_v3_spi_mask(id);fire_ovl_gic_restore(saved);return KERN_FAILURE;
    }
    return result;
}
int fire_ovl_gic_restore(uint32_t saved[8])
{
    const unsigned id=216,bit=1u<<24,word=24,shift=(216%16)*2;
    if(!saved || saved[0]!=id || !fire_dist || !fire_ovl_intpol)return 0;
    gic_v3_spi_mask(id);
    if(cpu_number()!=master_cpu || fire_active[master_cpu]==id)return 0;
    gic_v3_spi_detach(id);
    fw(fire_dist,FD_GROUP+word,(fr(fire_dist,FD_GROUP+word)&~bit)|(saved[1]?bit:0));
    *(volatile uint8_t *)(fire_dist+FD_PRIORITY+id)=(uint8_t)saved[2];
    *(volatile uint8_t *)(fire_dist+FD_TARGET+id)=(uint8_t)saved[3];
    unsigned off=FD_CONFIG+4*(id/16);
    fw(fire_dist,off,(fr(fire_dist,off)&~(3u<<shift))|(saved[4]<<shift));
    fw(fire_ovl_intpol,0x634,(fr(fire_ovl_intpol,0x634)&~bit)|(saved[6]?bit:0));
    fw(fire_dist,FD_PENDING_CLEAR+word,bit);
    int ok=!(fr(fire_dist,FD_ENABLE+word)&bit) &&
        ((fr(fire_dist,FD_GROUP+word)&bit)!=0)==saved[1] &&
        *(volatile uint8_t *)(fire_dist+FD_PRIORITY+id)==saved[2] &&
        *(volatile uint8_t *)(fire_dist+FD_TARGET+id)==saved[3] &&
        ((fr(fire_dist,off)>>shift)&3)==saved[4] &&
        ((fr(fire_ovl_intpol,0x634)&bit)!=0)==saved[6];
    if(ok)saved[0]=0;return ok;
}
