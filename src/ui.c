/* ui.c - Frontend X11 bare-metal: janela, barra de endereco, viewport com scroll,
 * compositor via PutImage direto no pixmap da janela (XRender/XPutImage), sem toolkit.
 */
#include "koth.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <sys/select.h>
#include <unistd.h>
#include <ctype.h>

#define CHROME_H 64      /* barra superior */
#define MARGIN   8
#define PAD      6

typedef struct {
    Display *dpy;
    Window win;
    GC gc;
    Pixmap chrome_px;     /* barra de ferramentas desenhada por nos */
    Visual *vis;
    int depth;
    int scr;
    uint32_t bg, fg, bar_bg, bar_fg, url_bg, accent, link_c;
} UI;

static UI ui;
static Page pg;
static volatile int loading=0;
static pthread_t loader_thr;

/* input state */
static char addr[1024];
static int addr_len, addr_sel_end=-1; /* cursor */
static int focused_url=0;
static int mouse_x=-1,mouse_y=-1;
static int drag_link=0;

/* ---------------- helpers de desenho na chrome -------------------- */
static void draw_chrome(void);
static void start_load(const char *url);

/* ================= loader thread =================================== */
typedef struct { char url[1024]; } LoadReq;

static void *load_thread(void *arg){
    LoadReq *lr=arg;
    page_load(&pg,lr->url);
    free(lr);
    __atomic_store_n(&loading,0,__ATOMIC_RELEASE);
    return NULL;
}

static void start_load(const char *url_str){
    Str u; str_init(&u);
    search_url_for(url_str,&u);
    LoadReq *lr=malloc(sizeof *lr);
    snprintf(lr->url,sizeof lr->url,"%s",u.p);
    str_free(&u);
    snprintf(addr,sizeof addr,"%s",lr->url);
    addr_len=strlen(addr);
    if(loading){ free(lr); return; }
    loading=1;
    pthread_create(&loader_thr,NULL,load_thread,lr);
    pthread_detach(loader_thr);
}

/* ================= paint viewport ================================== */
static void composite(void){
    int vw=pg.viewport_w, vh=pg.viewport_h;
    if(vw<=0||vh<=0)return;
    /* blit surf -> buffer linear (já é linear); envia area visivel */
    int sy=pg.scroll_y;
    if(sy<0)sy=0;
    if(sy+vh>pg.surf.h) sy=max_i(pg.surf.h-vh,0);
    Bitmap tmp=pg.surf;
    tmp.px=pg.surf.px+(size_t)sy*pg.surf.stride;
    tmp.h=min_i(vh,pg.surf.h-sy);
    /* garante fundo branco se conteudo menor que viewport */
    Image *dummy=NULL;(void)dummy;
    /* envia ao X */
    XImage *xi=XCreateImage(ui.dpy,ui.vis,ui.depth,ZPixmap,0,(char*)tmp.px,vw,tmp.h,32,0);
    if(xi){
        xi->byte_order=LSBFirst;
        xi->red_mask=0xFF0000; xi->green_mask=0xFF00; xi->blue_mask=0xFF;
        GC g=XDefaultGC(ui.dpy,ui.scr);
        XPutImage(ui.dpy,ui.win,g,xi,0,0,MARGIN,CHROME_H,vw,min_i(tmp.h,vh),0);
        /* limpa faixa abaixo se conteudo curto */
        if(tmp.h<vh){
            XSetForeground(ui.dpy,g,0xFFFFFF);
            XFillRectangle(ui.dpy,ui.win,g,MARGIN,CHROME_H+tmp.h,vw,vh-tmp.h);
        }
        XDestroyImage(xi);
    }
    /* scrollbar */
    int sbx=MARGIN+vw+2, sbw=10;
    XSetForeground(ui.dpy,ui.gc,0xE0E0E0);
    XFillRectangle(ui.dpy,ui.win,ui.gc,sbx,CHROME_H,sbw,vh);
    if(pg.content_h>vh){
        int th=max_i(vh*vh/pg.content_h,24);
        int ty=CHROME_H+(vh-th)*(long)sy/max_i(pg.content_h-vh,1);
        XSetForeground(ui.dpy,ui.gc,0x909090);
        XFillRectangle(ui.dpy,ui.win,ui.gc,sbx,ty,sbw,th);
    }
}

static void draw_chrome(void){
    /* barra de fundo */
    XWindowAttributes wa; XGetWindowAttributes(ui.dpy,ui.win,&wa);
    int W=wa.width;
    XSetForeground(ui.dpy,ui.gc,0x2B2B33);
    XFillRectangle(ui.dpy,ui.win,ui.gc,0,0,W,CHROME_H);
    /* botoes back/fwd/reload/home como caixas com glifos ASCII */
    struct { const char *lbl; int x; } btns[]={{"<",MARGIN},{"^",MARGIN+34},{">",MARGIN+68},{"o",MARGIN+102}};
    XSetForeground(ui.dpy,ui.gc,0x4A4A55);
    for(int i=0;i<4;i++) XFillRectangle(ui.dpy,ui.win,ui.gc,btns[i].x,8,26,26);
    XSetForeground(ui.dpy,ui.gc,0xFFFFFF);
    for(int i=0;i<4;i++){
        XCharStruct overall; int dirx,diry; unsigned uw,uh,ux,uy;
        XTextExtents(ui.gc,(char*)btns[i].lbl,1,&dirx,&diry,&uw,&uh,&ux,&uy,&overall);
        XDrawString(ui.dpy,ui.win,ui.gc,btns[i].x+13-overall.width/2,8+13+overall.ascent/2-1,(char*)btns[i].lbl,1);
    }
    /* campo URL */
    int ux0=MARGIN+140, uw0=W-160-120;
    if(uw0<100)uw0=100;
    XSetForeground(ui.dpy,ui.gc,focused_url?0xFFFFFF:0xF0F0F0);
    XFillRectangle(ui.dpy,ui.win,ui.gc,ux0,8,uw0,26);
    XSetForeground(ui.dpy,ui.gc,focused_url?0x4A90D9:0xC8C8C8);
    XDrawRectangle(ui.dpy,ui.win,ui.gc,ux0,8,uw0,26);
    /* texto URL com clip */
    XSetClipRect(ui.dpy,ui.gc,CW,ux0+4,4,uw0-8,34,NULL);
    XSetForeground(ui.dpy,ui.gc,0x111111);
    char shown[1024]; snprintf(shown,sizeof shown,"%s",addr);
    /* rola para manter cursor visivel: simplificado: mostra do inicio */
    XDrawString(ui.dpy,ui.win,ui.gc,ux0+4,8+18,shown,strlen(shown));
    if(focused_url){
        /* caret */
        int cw=XTextWidth(ui.gc,shown,addr_len);
        XSetForeground(ui.dpy,ui.gc,0x000000);
        XDrawLine(ui.dpy,ui.win,ui.gc,ux0+4+cw,10,ux0+4+cw,32);
    }
    XSetClipOrigin(ui.dpy,ui.gc,0,0);
    XSetClipMask(ui.dpy,ui.gc,None);
    /* status: titulo + tempo */
    XSetForeground(ui.dpy,ui.gc,0xB0B0B8);
    char st[512];
    if(loading) snprintf(st,sizeof st,"carregando... %s",pg.current_url);
    else if(pg.error) snprintf(st,sizeof st,"erro: %s",pg.errmsg);
    else snprintf(st,sizeof st,"%s   [%.0f ms%s]",pg.title,pg.load_time*1000,imgs_pending()?", imagens...":"");
    char stt[512]; int k=0;
    for(const char *p=st;*p&&k<500;p++) stt[k++]=(*p>=32&&*p<127)?*p:'.';
    stt[k]=0;
    XDrawString(ui.dpy,ui.win,ui.gc,MARGIN,CHROME_H-8,stt,strlen(stt));
}

/* ================= eventos =========================================== */
static void relayout_if_needed(int new_w){
    int vw=new_w-MARGIN*2-16;
    if(vw<200)vw=200;
    if(vw!=pg.viewport_w){
        pg.viewport_w=vw;
        page_layout(&pg,vw);
        pg.dirty=1;
    }
}

static void nav_home(void){ start_load("https://example.com"); }

static void click_button(int x,int y){
    if(y<8||y>34)return;
    if(x>=MARGIN&&x<MARGIN+26){ /* back */
        const char *u=page_back(&pg); if(u)start_load(u);
    } else if(x>=MARGIN+34&&x<MARGIN+60){ /* fwd */
        const char *u=page_forward(&pg); if(u)start_load(u);
    } else if(x>=MARGIN+68&&x<MARGIN+94){ /* reload */
        if(pg.current_url[0])start_load(pg.current_url);
    } else if(x>=MARGIN+102&&x<MARGIN+128){ /* home */
        nav_home();
    }
}

static int in_url_field(int x,int y,int W){
    int ux0=MARGIN+140, uw0=W-160-120; if(uw0<100)uw0=100;
    return x>=ux0&&x<ux0+uw0&&y>=8&&y<=34;
}

int main(int argc,char**argv){
    (void)argc;(void)argv;
    ui.dpy=XOpenDisplay(NULL);
    if(!ui.dpy){ fprintf(stderr,"Koth: sem display X11 ($DISPLAY)\n"); return 1; }
    ui.scr=DefaultScreen(ui.dpy);
    ui.vis=DefaultVisual(ui.dpy,ui.scr);
    ui.depth=DefaultDepth(ui.dpy,ui.scr);
    Window root=RootWindow(ui.dpy,ui.scr);
    int W=1280,H=800;
    ui.win=XCreateSimpleWindow(ui.dpy,root,100,60,W,H,0,0x1a1a1a,0xFFFFFF);
    XSelectInput(ui.dpy,ui.win,ExposureMask|ButtonPressMask|ButtonReleaseMask|
                 PointerMotionMask|KeyPressMask|StructureNotifyMask);
    XStoreName(ui.dpy,ui.win,"Koth — KHronos Optimization & Traversal Engine");
    Atom wmdele=XInternAtom(ui.dpy,"WM_DELETE_WINDOW",False);
    XSetWMProtocols(ui.dpy,ui.win,&wmdele,1);
    ui.gc=XCreateGC(ui.dpy,ui.win,0,NULL);
    /* fonte X core para chrome */
    XFontStruct *fs=XLoadFont(ui.dpy,"-*-fixed-medium-r-*-*-13-*-*-*-*-*-*-*");
    if(!fs) fs=XLoadFont(ui.dpy,"fixed");
    if(fs) XSetFont(ui.dpy,ui.gc,fs->fid);
    XMapWindow(ui.dpy,ui.win);

    net_init();
    page_init(&pg);
    pg.viewport_w=W-MARGIN*2-16;
    pg.viewport_h=H-CHROME_H-MARGIN;
    snprintf(addr,sizeof addr,"https://example.com");
    addr_len=strlen(addr);
    start_load(addr);

    int expose_dirty=1, content_dirty=1;
    double last_poll=0;
    while(!pg.quit_requested){
        /* processa todos eventos disponiveis sem bloquear (poll no fd do X) */
        while(XPending(ui.dpy)){
            XEvent ev; XNextEvent(ui.dpy,&ev);
            switch(ev.type){
            case Expose: expose_dirty=1; break;
            case ConfigureNotify:{
                int nw=ev.xconfigure.width,nh=ev.xconfigure.height;
                pg.viewport_h=nh-CHROME_H-MARGIN;
                if(pg.viewport_h<100)pg.viewport_h=100;
                relayout_if_needed(nw);
                expose_dirty=1;content_dirty=1;
                break;}
            case ButtonPress:
                if(ev.xbutton.button==1){
                    int x=ev.xbutton.x,y=ev.xbutton.y;
                    if(y<CHROME_H){
                        XWindowAttributes wa;XGetWindowAttributes(ui.dpy,ui.win,&wa);
                        if(in_url_field(x,y,wa.width)){focused_url=1;expose_dirty=1;}
                        else {focused_url=0;click_button(x,y);}
                    } else {
                        focused_url=0;
                        int px=x-MARGIN, py=y-CHROME_H+pg.scroll_y;
                        DomNode *lk=page_hit_link(&pg,px,py);
                        if(lk&&lk->link_url[0]){
                            lk->visited_link=1;
                            start_load(lk->link_url);
                            content_dirty=1;
                        }
                    }
                } else if(ev.xbutton.button==4){ /* scroll up */
                    pg.scroll_y=max_i(pg.scroll_y-60,0); content_dirty=1;
                } else if(ev.xbutton.button==5){
                    int maxs=max_i(pg.content_h-pg.viewport_h,0);
                    pg.scroll_y=min_i(pg.scroll_y+60,maxs); content_dirty=1;
                }
                break;
            case KeyPress:{
                KeySym ks=XLookupKeysym(&ev.xkey,0);
                if(ks==XK_BackSpace&&focused_url){
                    if(addr_len>0)addr[--addr_len]=0;
                    expose_dirty=1;
                } else if((ks&0xFF)>=32&&(ks&0xFF)<127&&!(ev.xkey.state&ControlMask)&&focused_url&&addr_len<(int)sizeof(addr)-1){
                    addr[addr_len++]=(char)(ks&0xFF); addr[addr_len]=0;
                    expose_dirty=1;
                } else
                if(focused_url){
                    if(ks==XK_Return||ks==XK_KP_Enter){
                        focused_url=0;
                        addr[addr_len]=0;
                        start_load(addr);
                    } else if(ks==XK_BackSpace){
                        if(addr_len>0)addr[--addr_len]=0;
                        expose_dirty=1;
                    } else if(ks==XK_Escape){
                        focused_url=0;expose_dirty=1;
                    } else if(ks==XK_l&&(ev.xkey.state&ControlMask)){
                        focused_url=1;expose_dirty=1;
                    } else if(ks==XK_F5){
                        if(pg.current_url[0])start_load(pg.current_url);
                    }
                } else {
                    if(ks==XK_Down||ks==XK_j){
                        int maxs=max_i(pg.content_h-pg.viewport_h,0);
                        pg.scroll_y=min_i(pg.scroll_y+40,maxs);content_dirty=1;
                    } else if(ks==XK_Up||ks==XK_k){
                        pg.scroll_y=max_i(pg.scroll_y-40,0);content_dirty=1;
                    } else if(ks==XK_Page_Down||ks==XK_space){
                        int maxs=max_i(pg.content_h-pg.viewport_h,0);
                        pg.scroll_y=min_i(pg.scroll_y+pg.viewport_h*8/10,maxs);content_dirty=1;
                    } else if(ks==XK_Page_Up){
                        pg.scroll_y=max_i(pg.scroll_y-pg.viewport_h*8/10,0);content_dirty=1;
                    } else if(ks==XK_Home||ks==XK_g){
                        pg.scroll_y=0;content_dirty=1;
                    } else if(ks==XK_End){
                        pg.scroll_y=max_i(pg.content_h-pg.viewport_h,0);content_dirty=1;
                    } else if(ks==XK_F5){
                        if(pg.current_url[0])start_load(pg.current_url);
                    } else if((ks==XK_Back&&pg.hist_pos>0)||(ks==XK_Left)){
                        const char*u=page_back(&pg);if(u)start_load(u);
                    } else if(ks==XK_Right){
                        const char*u=page_forward(&pg);if(u)start_load(u);
                    } else if(ks=='q'||ks==XK_Escape){
                        pg.quit_requested=1;
                    }
                }
                break;}
            case ClientMessage:
                if((Atom)ev.xclient.data.l[0]==wmdele) pg.quit_requested=1;
                break;
            case DestroyNotify: pg.quit_requested=1; break;
            }
        }
        /* fim de load? */
        if(loading&&!__sync_fetch_and_add(&loading,0)){
            focused_url=0;
            expose_dirty=1; content_dirty=1;
            pg.dirty=1;
        }
        /* imagens chegaram? rerender */
        if(g_page_dirty_flag){
            g_page_dirty_flag=0;
            content_dirty=1;
            expose_dirty|=1; /* status muda (imagens...) */
        }
        /* render quando necessario */
        if(pg.dirty&&!loading){
            page_render(&pg);
            pg.dirty=0;
            content_dirty=1;
        }
        if(content_dirty&&!loading){
            composite();
            content_dirty=0;
        }
        if(expose_dirty){
            draw_chrome();
            expose_dirty=0;
        }
        XFlush(ui.dpy);
        /* aguarda eventos com timeout p/ polling */
        if(!XPending(ui.dpy)){
            fd_set fds; FD_ZERO(&fds);
            int xfd=ConnectionNumber(ui.dpy);
            FD_SET(xfd,&fds);
            struct timeval tv={0,30*1000};
            select(xfd+1,&fds,NULL,NULL,&tv);
        }
        (void)last_poll;
    }
    /* teardown */
    return 0;
}
