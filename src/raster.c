/* raster.c - Rasterizador software: primitivas com clipping e blit de imagem */
#include "koth.h"

static inline void put_px(Bitmap *b,int x,int y,uint32_t c){
    if((unsigned)x<(unsigned)b->w && (unsigned)y<(unsigned)b->h)
        b->px[(size_t)y*b->stride+x]=c;
}

void bmp_fill(Bitmap *b, int x,int y,int w,int h, uint32_t col){
    int x0=clamp_i(x,0,b->w), y0=clamp_i(y,0,b->h);
    int x1=clamp_i(x+w,0,b->w), y1=clamp_i(y+h,0,b->h);
    if(x1<=x0||y1<=y0)return;
    for(int yy=y0;yy<y1;yy++){
        uint32_t *row=b->px+(size_t)yy*b->stride;
        /* memset 4-byte pattern rapido */
        uint32_t *dst=row+x0;
        if(w>=8&&col){
            /* preenchimento vetorizado pelo compilador */
            for(int i=0;i<x1-x0;i++) dst[i]=col;
        } else {
            for(int i=0;i<x1-x0;i++) dst[i]=col;
        }
    }
}

void bmp_hline(Bitmap *b,int x0,int x1,int y,uint32_t col){
    if(y<0||y>=b->h)return;
    if(x0<0)x0=0; if(x1>=b->w)x1=b->w-1;
    uint32_t *row=b->px+(size_t)y*b->stride;
    for(int x=x0;x<=x1;x++) row[x]=col;
}

void bmp_draw_glyph(Bitmap *b,int x,int y,const uint8_t rows[8],uint32_t col,int scale){
    if(scale<1)scale=1;
    for(int gy=0;gy<8;gy++){
        uint8_t bits=rows[gy];
        if(!bits)continue;
        int py=y+gy*scale;
        if(py<0||py>=b->h)continue;
        uint32_t *row=b->px+(size_t)py*b->stride;
        for(int gx=0;gx<8;gx++){
            if(bits&(0x80>>gx)){
                int px=x+gx*scale;
                if(px<0||px>=b->w)continue;
                if(scale==1){ row[px]=col; }
                else{
                    for(int sy=0;sy<scale;sy++){
                        int yy=py+sy; if(yy>=b->h)break;
                        uint32_t *r2=b->px+(size_t)yy*b->stride;
                        for(int sx=0;sx<scale;sx++){
                            int xx=px+sx; if(xx>=b->w)break;
                            r2[xx]=col;
                        }
                    }
                }
            }
        }
    }
}

void bmp_draw_text(Bitmap *b,int x,int y,const char *s,uint32_t col,int scale){
    if(scale<1)scale=1;
    uint32_t n=strlen(s),i=0;
    int cx=x;
    while(i<n){
        uint32_t cp=utf8_next(s,n,&i);
        if(!cp)break;
        const uint8_t *g=glyph_for_cp(cp);
        bmp_draw_glyph(b,cx,y,g,col,scale);
        cx+=GLYPH_W*scale;
    }
}

void bmp_draw_image(Bitmap *b,int x,int y,int w,int h,const Image *im){
    if(!im||!im->loaded||!im->rgba||w<=0||h<=0)return;
    int x0=clamp_i(x,0,b->w),y0=clamp_i(y,0,b->h),x1=clamp_i(x+w,0,b->w),y1=clamp_i(y+h,0,b->h);
    if(x1<=x0||y1<=y0)return;
    /* nearest-neighbor com passo fixo 16.16 */
    uint32_t sx_step=((uint32_t)im->w<<16)/w;
    uint32_t sy_step=((uint32_t)im->h<<16)/h;
    for(int yy=y0;yy<y1;yy++){
        uint32_t syf=(yy-y)*sy_step; if(syf>=(uint32_t)im->h<<16)syf=((uint32_t)im->h<<16)-1;
        const uint8_t *srow=im->rgba+((size_t)(syf>>16))*im->w*4;
        uint32_t *drow=b->px+(size_t)yy*b->stride;
        uint32_t sxf=(x0-x)*sx_step;
        for(int xx=x0;xx<x1;xx++,sxf+=sx_step){
            uint32_t sxi=sxf>>16; if(sxi>=(uint32_t)im->w)sxi=im->w-1;
            const uint8_t *p=srow+sxi*4;
            uint32_t a=p[3];
            uint32_t *d=drow+xx;
            if(a==255){ *d=(p[0]<<16)|(p[1]<<8)|p[2]; }
            else if(a>0){
                uint32_t dc=*d;
                uint32_t r=((p[0]*a)+(((dc>>16)&0xFF)*(255-a)))/255;
                uint32_t g=((p[1]*a)+(((dc>>8)&0xFF)*(255-a)))/255;
                uint32_t bl=((p[2]*a)+((dc&0xFF)*(255-a)))/255;
                *d=(r<<16)|(g<<8)|bl;
            }
        }
    }
}
