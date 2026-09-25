/* img.c - Decodificadores de imagem proprios: PNG (via libz p/ inflate) e GIF89a LZW.
 * JPEG fica pendente de DCT; retorna NULL (placeholder desenhado pelo layout).
 */
#include "koth.h"
#include <zlib.h>

static uint32_t rd_be32(const uint8_t*p){ return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }
static uint16_t rd_le16(const uint8_t*p){ return p[0]|(p[1]<<8); }

/* ------------------------- PNG ------------------------------------- */
static void paeth_pred(uint8_t *row,const uint8_t *prev,uint8_t a0,int bpp,int w){
    for(int i=0;i<w;i++){
        int a = i>=bpp? row[i-bpp] : 0;
        int b = prev[i];
        int c = i>=bpp? prev[i-bpp] : 0;
        int p=a+b-c;
        int pa=abs(p-a),pb=abs(p-b),pc=abs(p-c);
        int pr=(pa<=pb&&pa<=pc)?a:(pb<=pc?b:c);
        row[i]=(uint8_t)(row[i]+pr);
        (void)a0;(void)w;
    }
}

static Image *decode_png(const uint8_t *d, uint32_t n){
    if(n<8||memcmp(d,"\x89PNG\r\n\x1a\n",8))return NULL;
    uint32_t pos=8;
    int w=0,h=0,bd=8,ct=6,interlace=0;
    uint8_t *idat=NULL; uint32_t idat_len=0,idat_cap=0;
    while(pos+8<=n){
        uint32_t clen=rd_be32(d+pos);
        const uint8_t*type=d+pos+4;
        uint32_t data=pos+8;
        if(pos+12+(long)clen>n)break;
        if(!memcmp(type,"IHDR",4)&&clen>=13){
            w=rd_be32(d+data); h=rd_be32(d+data+4);
            bd=d[data+8]; ct=d[data+9]; interlace=d[data+12];
        } else if(!memcmp(type,"IDAT",4)){
            if(idat_len+clen>idat_cap){
                idat_cap=(idat_len+clen)*2+4096;
                idat=realloc(idat,idat_cap);
            }
            memcpy(idat+idat_len,d+data,clen); idat_len+=clen;
        } else if(!memcmp(type,"IEND",4)){
            break;
        }
        pos=data+clen+4;
    }
    if(!w||!h||!idat||bd!=8||interlace){free(idat);return NULL;}
    int channels;
    switch(ct){ case 0:channels=1;break; case 2:channels=3;break; case 3:channels=1;break;
               case 4:channels=2;break; case 6:channels=4;break; default:free(idat);return NULL; }
    /* inflate */
    uLongf raw_len=(uLongf)((size_t)w*h*channels+h+16);
    uint8_t *raw=malloc(raw_len);
    z_stream zs; memset(&zs,0,sizeof zs);
    if(inflateInit(&zs)!=Z_OK){ free(idat);free(raw);return NULL; }
    zs.next_in=idat; zs.avail_in=idat_len;
    zs.next_out=raw; zs.avail_out=raw_len;
    int zr=inflate(&zs,Z_FINISH);
    inflateEnd(&zs);
    free(idat);
    if(zr!=Z_STREAM_END){ free(raw);return NULL; }
    Image *im=calloc(1,sizeof(Image));
    im->w=w; im->h=h; im->rgba=malloc((size_t)w*h*4);
    if(!im->rgba){free(raw);free(im);return NULL;}
    uint8_t *cur=calloc(w*channels,1), *prev=calloc(w*channels,1);
    uint8_t *pal=NULL; int pal_n=0; uint8_t *trns=NULL; int trns_n=0;
    /* reparse para palette/transparency */
    {
        uint32_t p2=8;
        while(p2+8<=n){
            uint32_t cl=rd_be32(d+p2); const uint8_t*ty=d+p2+4; uint32_t dt=p2+8;
            if(dt+cl>n)break;
            if(!memcmp(ty,"PLTE",4)){ pal=malloc(cl); memcpy(pal,d+dt,cl); pal_n=cl/3; }
            else if(!memcmp(ty,"tRNS",4)){ trns=malloc(cl); memcpy(trns,d+dt,cl); trns_n=cl; }
            p2=dt+cl+4;
        }
    }
    size_t src=0;
    int bpp=channels;
    for(int y=0;y<h;y++){
        if(src>=raw_len)break;
        uint8_t filter=raw[src++];
        int lin_w=w*channels;
        if(src+lin_w>raw_len)break;
        memcpy(cur,raw+src,lin_w); src+=lin_w;
        switch(filter){
        case 0:break;
        case 1: for(int i=bpp;i<lin_w;i++) cur[i]+=cur[i-bpp]; break;
        case 2: for(int i=0;i<lin_w;i++) cur[i]+=prev[i]; break;
        case 3: for(int i=0;i<lin_w;i++){int a=i>=bpp?cur[i-bpp]:0; cur[i]+=(a+prev[i])/2;} break;
        case 4: paeth_pred(cur,prev,0,bpp,lin_w); break;
        default: goto fail;
        }
        uint32_t *dst=im->rgba+(size_t)y*w*4;
        for(int x=0;x<w;x++){
            uint32_t r,g,b,a=255;
            switch(ct){
            case 0:{uint8_t v=cur[x];r=g=b=v;if(trns_n==2){uint8_t tv0=trns[0],tv1=trns[1];if(v>=tv0&&v<=tv1)a=0;}break;}
            case 2:r=cur[x*3];g=cur[x*3+1];b=cur[x*3+2];
                if(trns_n>=6){uint32_t tr=(trns[0]<<16)|(trns[2]<<8)|trns[4];if(((r<<16)|(g<<8)|b)==tr)a=0;}
                break;
            case 3:{uint8_t idx=cur[x];if(pal&&idx<pal_n){r=pal[idx*3];g=pal[idx*3+1];b=pal[idx*3+2];}
                if(trns&&idx<trns_n)a=trns[idx];break;}
            case 4:r=g=b=cur[x*2];a=cur[x*2+1];break;
            case 6:r=cur[x*4];g=cur[x*4+1];b=cur[x*4+2];a=cur[x*4+3];break;
            }
            dst[x]=(r<<16)|(g<<8)|b|(a<<24);
        }
        memcpy(prev,cur,lin_w);
    }
fail:
    free(raw);free(cur);free(prev);free(pal);free(trns);
    im->loaded=1;
    return im;
}

/* ------------------------- GIF -------------------------------------- */
static Image *decode_gif(const uint8_t *d, uint32_t n){
    if(n<13||memcmp(d,"GIF8",4))return NULL;
    uint8_t gct_flags=d[10];
    uint8_t *gct=NULL; int gct_n=0;
    if(gct_flags&0x80){
        gct_n=2<<(gct_flags&7);
        if(13+(size_t)gct_n*3>n)return NULL;
        gct=malloc(gct_n*3); memcpy(gct,d+13,gct_n*3);
    }
    uint32_t pos=13+((gct_flags&0x80)?(uint32_t)gct_n*3:0);
    int transparent=-1;
    while(pos+1<n){
        uint8_t b=d[pos++];
        if(b=='!'){
            if(pos>=n)break;
            uint8_t label=d[pos++];
            if(label==0xF9 && pos+4<=n){
                uint8_t bl=d[pos+3];
                if(bl&1)transparent=d[pos+4];
            }
            while(pos<n){ uint8_t bs=d[pos++]; if(!bs)break; pos+=bs; }
            continue;
        }
        if(b==0x2C){
            if(pos+9>n)break;
            uint16_t iw=d[pos+3]|(d[pos+4]<<8), ih=d[pos+5]|(d[pos+6]<<8);
            uint8_t lf=d[pos+8];
            pos+=9;
            uint8_t *lct=NULL; int lct_n=0;
            if(lf&0x80){
                lct_n=2<<(lf&7);
                if(pos+(uint32_t)lct_n*3>n){free(gct);return NULL;}
                lct=malloc(lct_n*3); memcpy(lct,d+pos,lct_n*3); pos+=lct_n*3;
            }
            if(pos>=n){free(lct);free(gct);return NULL;}
            int lzw_min=d[pos++];
            if(lzw_min<2||lzw_min>11){free(lct);free(gct);return NULL;}
            /* coleta sub-blocos */
            uint32_t bo=0,total=0;
            {
                uint32_t q=pos;
                while(q<n){ uint8_t bs=d[q++]; if(!bs)break; total+=bs; q+=bs; }
            }
            uint8_t *buf=malloc(total?total:1);
            {
                uint32_t q=pos;
                bo=0;
                while(q<n){
                    uint8_t bs=d[q++];
                    if(!bs)break;
                    if(q+bs>n)bs=n-q;
                    memcpy(buf+bo,d+q,bs); bo+=bs; q+=bs;
                }
            }
            pos+= 0; /* avanca apos dados consumidos abaixo */
            { uint32_t q=pos; while(q<n){ uint8_t bs=d[q++]; if(!bs)break; q+=bs; } pos=q; }
            /* LZW decode correto */
            int npix=(int)iw*(int)ih;
            if(npix<=0||npix>40*1000*1000){free(buf);free(lct);free(gct);return NULL;}
            uint8_t *idx=calloc(npix,1);
            uint16_t prefix[4096]; uint8_t suffix_[4096]; uint8_t firsts[4096];
            int clear=1<<lzw_min, end_code=clear+1;
            for(int i=0;i<clear;i++){prefix[i]=0xFFFF;suffix_[i]=i;firsts[i]=i;}
            int code_size=lzw_min+1, next=end_code+1;
            uint32_t bitpos=0;
            int ip=0;
            int prev_code=-1;
            #define GETB(k) ({ uint32_t v=0; for(int ii=0;ii<(k);ii++){ uint32_t byte=bitpos>>3; if(byte>=bo)break; v|=((uint32_t)((buf[byte]>>(bitpos&7))&1))<<ii; bitpos++; } v; })
            for(;;){
                if(bitpos>=bo*8)break;
                int code=GETB(code_size);
                if(code==clear){
                    code_size=lzw_min+1; next=end_code+1; prev_code=-1; continue;
                }
                if(code==end_code)break;
                uint8_t stk[4096]; int sl=0;
                int c=code;
                if(c>=next){ /* codigo ainda nao definido: usa sufixo do anterior */
                    if(prev_code<0){break;}
                    stk[sl++]=firsts[prev_code];
                    c=prev_code;
                }
                int out_start_ip=ip;
                while(c>=0&&c<4096&&prefix[c]!=0xFFFF){ stk[sl++]=suffix_[c]; c=prefix[c]; }
                if(c<0||c>=4096){break;}
                uint8_t fc=(uint8_t)c;
                stk[sl++]=fc;
                if(ip+sl>npix)sl=npix-ip;
                for(int i=sl-1;i>=0;i--){ if(ip<npix) idx[ip++]=stk[i]; }
                if(prev_code>=0&&next<4096){
                    prefix[next]=(uint16_t)prev_code;
                    suffix_[next]=fc;
                    firsts[next]=firsts[prev_code];
                    next++;
                    if(next==(1<<code_size)&&code_size<12) code_size++;
                }
                prev_code=code;
                if(ip>=npix)break;
            }
            #undef GETB
            free(buf);
            Image *im=calloc(1,sizeof(Image));
            im->w=iw; im->h=ih; im->rgba=malloc((size_t)iw*ih*4);
            const uint8_t *pal_use=lct?lct:gct;
            int pal_n=lct?lct_n:gct_n;
            for(int i=0;i<iw*ih;i++){
                uint8_t pi=idx[i];
                uint32_t r=0,g=0,bl=0;
                if(pal_use&&pi<pal_n){ r=pal_use[pi*3];g=pal_use[pi*3+1];bl=pal_use[pi*3+2]; }
                uint32_t a=(pi==transparent)?0:255;
                im->rgba[i]=r|(g<<8)|(bl<<16)|(a<<24);
            }
            free(idx);free(lct);free(gct);
            im->loaded=1;
            return im;
        }
        if(b==0x3B)break;
        break;
    }
    free(gct);
    return NULL;
}

Image *image_decode(const uint8_t *data, uint32_t len){
    if(len>8&&!memcmp(data,"\x89PNG",4)) return decode_png(data,len);
    if(len>3&&!memcmp(data,"GIF8",4))   return decode_gif(data,len);
    return NULL; /* JPEG/webp: nao suportados ainda */
}

void image_free(Image *im){
    if(!im)return;
    free(im->rgba); free(im);
}
