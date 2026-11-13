/* Fire-only, selected classic phone viewport. Panel and guest coordinates stay distinct.
 * No GPU/Surface ownership changes. Call input only on acknowledged valid frames.
 * A contact keeps its original destination until lift, including outside drags. */
#ifndef FIRE_NATIVE_HOME_H
#define FIRE_NATIVE_HOME_H
#define FNH_PANEL_W 800u
#define FNH_PANEL_H 1280u
#define FNH_X 0u
#define FNH_SCALE 2u
#define FNH_W 640u
#define FNH_H 960u
#define FNH_VW 800u
#define FNH_VH 1200u
#define FNH_HOME_X 400
#define FNH_HOME_Y 1240
#define FNH_HOME_R 30
#define FNH_ICON_R 9

static inline int fnh_home_hit(unsigned x,unsigned y){
    if(x>=FNH_PANEL_W || y>=FNH_PANEL_H)return 0;
    int dx=(int)x-FNH_HOME_X,dy=(int)y-FNH_HOME_Y;
    return dx*dx+dy*dy<=FNH_HOME_R*FNH_HOME_R;
}
/* All colors are grayscale: no panel channel-order assumption. */
static inline uint32_t fnh_chrome_pixel(unsigned x,unsigned y){
    int dx=(int)x-FNH_HOME_X,dy=(int)y-FNH_HOME_Y;
    int d=dx*dx+dy*dy;
    if(d<=FNH_HOME_R*FNH_HOME_R && d>=(FNH_HOME_R-3)*(FNH_HOME_R-3))return 0xff707070u;
    if(d<(FNH_HOME_R-3)*(FNH_HOME_R-3)){
        unsigned ax=dx<0?(unsigned)-dx:(unsigned)dx;
        unsigned ay=dy<0?(unsigned)-dy:(unsigned)dy;
        if(ax<=FNH_ICON_R && ay<=FNH_ICON_R && (ax>=FNH_ICON_R-2 || ay>=FNH_ICON_R-2))return 0xffa0a0a0u;
        return 0xff101010u;
    }
    return 0xff000000u;
}
/* Writes only reserved margins/bar; never alters a guest pixel or row padding. */
static inline void fnh_draw_chrome(uint8_t *base,unsigned stride){
    for(unsigned y=0;y<FNH_PANEL_H;y++){
        uint32_t *row=(uint32_t *)(base+(uint64_t)y*stride);
        for(unsigned x=0;x<FNH_PANEL_W;x++)
            if(y>=FNH_VH || x-FNH_X>=FNH_VW)
                row[x]=fnh_chrome_pixel(x,y);
    }
}
#ifdef FIRE_NATIVE_HOME_INPUT
struct fnh_input {unsigned route[10],x[10],y[10],home_contacts;};
typedef void (*fnh_post_fn)(uint32_t,uint32_t,uint32_t,uint32_t);
static inline unsigned fnh_axis(unsigned value,unsigned start,unsigned extent){
    if(value<=start)return 0;
    if(value>=start+extent-1)return 65535;
    return (value-start)*65535/(extent-1);
}
static inline void fnh_input_frame(struct fnh_input *s,unsigned seen,
    const unsigned *px,const unsigned *py,fnh_post_fn post){
    for(unsigned id=0;id<10;id++){
        if(seen&(1u<<id)){
            unsigned x=px[id],y=py[id];
            int fresh=!s->route[id];
            if(fresh){
                if(x-FNH_X<FNH_VW && y<FNH_VH)s->route[id]=1;
                else if(fnh_home_hit(x,y)){
                    s->route[id]=2;
                    if(!s->home_contacts++)post(HAKIOS_BUTTON_DOWN,HAKIOS_BUTTON_HOME,0,0);
                }else s->route[id]=3;
            }
            if(s->route[id]==1){
                x=fnh_axis(x,FNH_X,FNH_VW);y=fnh_axis(y,0,FNH_VH);
                if(fresh || x!=s->x[id] || y!=s->y[id])
                    post(fresh?HAKIOS_TOUCH_DOWN:HAKIOS_TOUCH_MOVE,id,x,y);
                s->x[id]=x;s->y[id]=y;
            }
        }else if(s->route[id]){
            if(s->route[id]==1)post(HAKIOS_TOUCH_UP,id,s->x[id],s->y[id]);
            else if(s->route[id]==2 && !--s->home_contacts)
                post(HAKIOS_BUTTON_UP,HAKIOS_BUTTON_HOME,0,0);
            s->route[id]=0;
        }
    }
}
#endif /* FIRE_NATIVE_HOME_INPUT */
#endif
