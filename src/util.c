/* util.c - Str, arena, hash, UTF-8, tempo */
#include "koth.h"
#include <stdarg.h>

double now_sec(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec + ts.tv_nsec*1e-9;
}

uint32_t fnv1a(const char *s, uint32_t n){
    uint32_t h=2166136261u;
    for(uint32_t i=0;i<n;i++){ h^=(uint8_t)s[i]; h*=16777619u; }
    return h;
}

void str_init(Str *s){ s->p=NULL; s->len=0; s->cap=0; }
void str_free(Str *s){ free(s->p); s->p=NULL; s->len=s->cap=0; }
void str_ensure(Str *s, uint32_t n){
    if(s->len+n+1<=s->cap) return;
    uint32_t nc = s->cap? s->cap : 64;
    while(nc < s->len+n+1) nc *= 2;
    s->p = realloc(s->p, nc);
    if(!s->p){ fprintf(stderr,"OOM str\n"); exit(1); }
    s->cap = nc;
}
void str_append(Str *s, const char *p, uint32_t n){
    str_ensure(s,n); memcpy(s->p+s->len,p,n); s->len+=n; s->p[s->len]=0;
}
void str_appc(Str *s, char c){ str_ensure(s,1); s->p[s->len++]=c; s->p[s->len]=0; }
void str_apps(Str *s, const char *lit){ str_append(s,lit,(uint32_t)strlen(lit)); }
void str_appf(Str *s, const char *fmt, ...){
    char tmp[1024]; va_list ap; va_start(ap,fmt);
    int n=vsnprintf(tmp,sizeof tmp,fmt,ap); va_end(ap);
    if(n>0) str_append(s,tmp,(uint32_t)min_u32(n,1023));
}
char *str_take(Str *s){
    if(!s->p){ s->p=malloc(1); s->p[0]=0; s->cap=1; }
    char *r=s->p; str_init(s); return r;
}

/* ------------------------- Arena ---------------------------------- */
void arena_init(Arena *a, uint32_t bs){ a->head=NULL; a->block_size=bs; }
void *arena_alloc(Arena *a, uint32_t n){
    n = (n+15)&~15u; /* alinhamento 16 */
    ArenaBlock *b=a->head;
    if(!b || b->cap-b->off < n){
        uint32_t cap = a->block_size;
        if(cap < n+sizeof(ArenaBlock)) cap = n+sizeof(ArenaBlock)+4096;
        b = malloc(cap);
        if(!b){ fprintf(stderr,"OOM arena\n"); exit(1); }
        b->next=a->head; b->cap=cap-sizeof(ArenaBlock); b->off=0;
        a->head=b;
    }
    void *p = b->data + b->off;
    b->off += n;
    return p;
}
char *arena_strdup(Arena *a, const char *s, uint32_t n){
    char *p = arena_alloc(a, n+1);
    memcpy(p,s,n); p[n]=0;
    return p;
}
void arena_destroy(Arena *a){
    ArenaBlock *b=a->head;
    while(b){ ArenaBlock *nx=b->next; free(b); b=nx; }
    a->head=NULL;
}

/* ------------------------- UTF-8 ---------------------------------- */
uint32_t utf8_next(const char *s, uint32_t n, uint32_t *i){
    uint32_t p=*i;
    if(p>=n){ *i=p; return 0; }
    uint8_t c=(uint8_t)s[p];
    if(c<0x80){ *i=p+1; return c; }
    int extra; uint32_t cp;
    if((c&0xE0)==0xC0){ extra=1; cp=c&0x1F; }
    else if((c&0xF0)==0xE0){ extra=2; cp=c&0x0F; }
    else if((c&0xF8)==0xF0){ extra=3; cp=c&0x07; }
    else { *i=p+1; return 0xFFFD; }
    if(p+extra>=n){ *i=p+1; return 0xFFFD; }
    for(int k=1;k<=extra;k++){
        uint8_t cc=(uint8_t)s[p+k];
        if((cc&0xC0)!=0x80){ *i=p+1; return 0xFFFD; }
        cp=(cp<<6)|(cc&0x3F);
    }
    *i=p+extra+1;
    return cp;
}
void utf8_put(Str *out, uint32_t cp){
    if(cp<0x80){ str_appc(out,(char)cp); }
    else if(cp<0x800){
        str_ensure(out,2);
        out->p[out->len++]=(char)(0xC0|(cp>>6));
        out->p[out->len++]=(char)(0x80|(cp&0x3F));
        out->p[out->len]=0;
    } else if(cp<0x10000){
        str_ensure(out,3);
        out->p[out->len++]=(char)(0xE0|(cp>>12));
        out->p[out->len++]=(char)(0x80|((cp>>6)&0x3F));
        out->p[out->len++]=(char)(0x80|(cp&0x3F));
        out->p[out->len]=0;
    } else {
        str_ensure(out,4);
        out->p[out->len++]=(char)(0xF0|(cp>>18));
        out->p[out->len++]=(char)(0x80|((cp>>12)&0x3F));
        out->p[out->len++]=(char)(0x80|((cp>>6)&0x3F));
        out->p[out->len++]=(char)(0x80|(cp&0x3F));
        out->p[out->len]=0;
    }
}
