/* Fixed little-endian, single-sector CPU probe journal. No allocation or I/O.
 * Persistence is provided by fire_emmc.c, only in an explicit SMP probe.
 */
#ifndef FIRE_CPU_JOURNAL_H
#define FIRE_CPU_JOURNAL_H
#define FIRE_CPU_JOURNAL_LBA 24832000u
#define FIRE_CPU_JOURNAL_CAP 20u
struct fire_cpu_checkpoint {uint64_t ticks,value;uint32_t stage,cpu;};
struct fire_cpu_journal {
    char magic[8];uint32_t version,run,count,sequence,checksum,reserved;
    struct fire_cpu_checkpoint entries[FIRE_CPU_JOURNAL_CAP];
};
_Static_assert(sizeof(struct fire_cpu_journal)==512,"one scratch sector");
static uint32_t fire_cpu_journal_hash(const struct fire_cpu_journal *j) {
    const uint8_t *p=(const void *)j;uint32_t h=2166136261u;
    for(unsigned i=0;i<512;i++)h=(h^((i>=24 && i<28)?0:p[i]))*16777619u;
    return h;
}
static int fire_cpu_journal_append(struct fire_cpu_journal *j,uint32_t run,
    uint32_t stage,uint32_t cpu,uint64_t value,uint64_t ticks) {
    if(!run || !stage || stage>12 || cpu>3)return -1;
    if(j->run!=run || memcmp(j->magic,"FRSMP001",8)) {
        memset(j,0,sizeof(*j));memcpy(j->magic,"FRSMP001",8);j->version=1;j->run=run;
    }
    if(j->count>FIRE_CPU_JOURNAL_CAP)return -1;
    if(j->count==FIRE_CPU_JOURNAL_CAP) {
        memmove(j->entries,j->entries+1,(FIRE_CPU_JOURNAL_CAP-1)*sizeof(*j->entries));
        j->count--;
    }
    j->entries[j->count++]=(struct fire_cpu_checkpoint){ticks,value,stage,cpu};
    j->sequence++;j->checksum=fire_cpu_journal_hash(j);return 0;
}
#endif
