"""Selected Fire phone viewport; reversible exact-anchor source hooks."""
FB='iokit/Kernel/MiniIOMobileFramebuffer.cpp'
HEADER='fire_native_home.h'
MARK='/* FIRE_NATIVE_HOME: '

def changes():
    return [
      ('#include "fire_present_deadline.h"', '#include "fire_present_deadline.h"\n/* FIRE_NATIVE_HOME: shared board geometry */\n#include "fire_native_home.h"'),
      ('\tfColumn = IONewZero(uint32_t, fPanelWidth);', '''/* FIRE_NATIVE_HOME: exact panel and coherent stock guest only */
    if(fPanelWidth!=FNH_PANEL_W || fPanelHeight!=FNH_PANEL_H || fRowBytes!=3200 ||
       fWidth!=FNH_W || fHeight!=FNH_H || ios6_guest_get()->scale!=FNH_SCALE)
        panic("fire: invalid native Home presentation geometry");
\tfColumn = IONewZero(uint32_t, fPanelWidth);'''),
      ('\tint64_t qx0 = (int64_t) dx * fPanelWidth / fWidth;\n\tint64_t qy0 = (int64_t) dy * fPanelHeight / fHeight;\n\tint64_t qx1 = ((int64_t) dx + dw) * fPanelWidth / fWidth;\n\tint64_t qy1 = ((int64_t) dy + dh) * fPanelHeight / fHeight;', '''/* FIRE_NATIVE_HOME: profile viewport transform; layer transforms remain */
    int64_t qx0=(int64_t)dx*FNH_VW/FNH_W+FNH_X,qy0=(int64_t)dy*FNH_VH/FNH_H;
    int64_t qx1=((int64_t)dx+dw)*FNH_VW/FNH_W+FNH_X,qy1=((int64_t)dy+dh)*FNH_VH/FNH_H;'''),
      ('\tcx0 = px0 < 0 ? 0 : px0;\n\tcx1 = (uint32_t) px1 > fPanelWidth ? (int32_t) fPanelWidth : px1;', '''/* FIRE_NATIVE_HOME: layers cannot write reserved panel chrome */
    cx0=px0<(int32_t)FNH_X?(int32_t)FNH_X:px0;
    cx1=px1>(int32_t)(FNH_X+FNH_VW)?(int32_t)(FNH_X+FNH_VW):px1;
    if(cx1<=cx0 || py1<=0 || py0>=(int32_t)FNH_VH)return;'''),
      ('\tjob.ry1 = py1 > (int32_t) fPanelHeight ? (int32_t) fPanelHeight : py1;', '\tjob.ry1 = py1 > (int32_t) FNH_VH ? (int32_t) FNH_VH : py1; /* FIRE_NATIVE_HOME: clip bottom */'),
      ('\tjob.unscaled = px1 - px0 == sw && py1 - py0 == sh && px0 >= 0 && (uint32_t) px1 <= fPanelWidth;', '\tjob.unscaled = px1 - px0 == sw && py1 - py0 == sh && px0 >= (int32_t)FNH_X && px1 <= (int32_t)(FNH_X+FNH_VW); /* FIRE_NATIVE_HOME: clipped rows use checked general path */'),
      ('                PE_initialize_console(NULL, kPEReleaseScreen);', '                PE_initialize_console(NULL, kPEReleaseScreen);\n                /* FIRE_NATIVE_HOME: persistent chrome, outside every app layer */\n                fnh_draw_chrome(fBase,fRowBytes);'),
    ]

def framebuffer(body,enabled):
    pairs=[p for p in changes() if p[0]!=p[1]]
    if MARK in body:
        for old,new in reversed(pairs):
            if body.count(new)!=1:raise ValueError('native Home source drift during restoration')
            body=body.replace(new,old,1)
        # Chrome must also survive layer-zero clear while display remains lit.
        old='\t\t\t\t}\n\t\t\t}\n\t\t\tcontinue;\n\t\t}\n\t\tmemcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));'
        new=old.replace('\t\t\t}\n\t\t\tcontinue;', '                fnh_draw_chrome(fBase,fRowBytes); /* native-home clear */\n\t\t\t}\n\t\t\tcontinue;')
        if body.count(new)!=1:raise ValueError('native Home clear hook drift')
        body=body.replace(new,old,1)
    if enabled:
        for old,new in pairs:
            if body.count(old)!=1:raise ValueError('native Home framebuffer anchor missing: '+old[:60])
            body=body.replace(old,new,1)
        old='\t\t\t\t}\n\t\t\t}\n\t\t\tcontinue;\n\t\t}\n\t\tmemcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));'
        new=old.replace('\t\t\t}\n\t\t\tcontinue;', '                fnh_draw_chrome(fBase,fRowBytes); /* native-home clear */\n\t\t\t}\n\t\t\tcontinue;')
        if body.count(old)!=1:raise ValueError('native Home clear anchor missing')
        body=body.replace(old,new,1)
    return body

def touch(body):
    pairs=[
      ('#include <sys/systm.h>', '#include <sys/systm.h>\n#define FIRE_NATIVE_HOME_INPUT 1\n#include "fire_native_home.h"'),
      ('    unsigned active=0,x[10]={0},y[10]={0},failures=0,frames=0,contacts=0;', '    unsigned active=0,x[10]={0},y[10]={0},failures=0,frames=0,contacts=0;\n    struct fnh_input native={0};'),
      ('seen|=1u<<id;nx[id]=px*65535/799;ny[id]=py*65535/1279;', 'seen|=1u<<id;nx[id]=px;ny[id]=py;'),
      ('                for(unsigned id=0;id<10;id++){', '                fnh_input_frame(&native,seen,nx,ny,hakios_touch_post);\n                for(unsigned id=0;id<10;id++){'),
      ('                        if(!(active&bit) || nx[id]!=x[id] || ny[id]!=y[id])\n                            hakios_touch_post(active&bit?HAKIOS_TOUCH_MOVE:HAKIOS_TOUCH_DOWN,id,nx[id],ny[id]);', ''),
      ('                    }else if(active&bit)hakios_touch_post(HAKIOS_TOUCH_UP,id,x[id],y[id]);', '                    }'),
      ('    for(unsigned id=0;id<10;id++)if(active&(1u<<id))hakios_touch_post(HAKIOS_TOUCH_UP,id,x[id],y[id]);', '    fnh_input_frame(&native,0,x,y,hakios_touch_post);'),
    ]
    for old,new in pairs:
        if body.count(old)!=1:raise ValueError('native Home touch anchor missing: '+old[:50])
        body=body.replace(old,new,1)
    return body

# panel is retained for low-level probes and reproducible pre-selection checks.
# New interactive Fire work uses the sole selected UI profile.
PREFERRED='phone-classic-home'
PROFILES=('panel',PREFERRED)
def header(body,mode):
    if mode!=PREFERRED:raise ValueError('invalid Home presentation')
    expected={'FNH_PANEL_W':'800u','FNH_PANEL_H':'1280u','FNH_X':'0u',
              'FNH_SCALE':'2u','FNH_W':'640u','FNH_H':'960u','FNH_VW':'800u',
              'FNH_VH':'1200u','FNH_HOME_X':'400','FNH_HOME_Y':'1240',
              'FNH_HOME_R':'30','FNH_ICON_R':'9'}
    for key,value in expected.items():
        if body.splitlines().count('#define '+key+' '+value)!=1:
            raise ValueError('selected Home geometry drift: '+key)
    return body

def display(mode):
    if mode not in PROFILES:raise ValueError('invalid Fire presentation')
    if mode=='panel':return dict(width=800,height=1280,scale=1,dpi=189)
    if mode=='phone-classic-home':return dict(width=640,height=960,scale=2,dpi=326)
    raise ValueError('invalid Fire presentation')

HANDHELDS=('iPod5,1','iPhone5,1','iPod4,1')
def proof(guest,cpu_count=1):
    result={'iPod4,1':'phone640x960 scale2 uniformly800x1200 plus80px Home bar;one-core coherent N81',
            'iPhone5,1':'phone640x960 scale2 uniformly800x1200 plus80px Home bar;one-core coherent N41',
            'iPod5,1':'phone640x960 scale2 uniformly800x1200 plus80px Home bar;one-core coherent N78'}[guest]
    if cpu_count!=1:
        if guest not in ('iPod5,1','iPod4,1') or cpu_count not in (2,4):raise ValueError('unsupported handheld CPU proof')
        result=result.replace('one-core',{2:'two-core',4:'four-core'}[cpu_count])
    return result
