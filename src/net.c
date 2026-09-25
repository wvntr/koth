/* net.c - Pilha de rede propria: URL, DNS cache TTL, pool de conexoes keep-alive,
 * HTTP/1.1 com gzip/deflate (inflate proprio), redirects, cache HTTP em memoria.
 * TLS via OpenSSL carregado dinamicamente (dlopen) -> binario nao liga -lssl.
 */
#include "koth.h"
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <dlfcn.h>

/* ============================ URL ================================== */
int url_parse(KUrl *u, const char *raw){
    memset(u,0,sizeof *u);
    const char *p=raw;
    /* scheme */
    const char *colon=strstr(p,"://");
    if(!colon) return -1;
    uint32_t sl=colon-p;
    if(sl>=sizeof u->scheme) return -1;
    for(uint32_t i=0;i<sl;i++) u->scheme[i]=tolower((unsigned char)p[i]);
    u->scheme[sl]=0;
    p+=3;
    if(!strcmp(u->scheme,"https")) u->secure=1, u->port=443;
    else if(!strcmp(u->scheme,"http")) u->port=80;
    else return -1;
    /* host[:port] ate '/' '?' '#' */
    const char *hs=p;
    while(*p && *p!='/' && *p!='?' && *p!='#') p++;
    const char *he=p;
    /* porta? */
    const char *c=(const char*)memchr(hs,':',he-hs);
    uint32_t hl;
    if(c){
        hl=c-hs;
        int portno=atoi(c+1);
        if(portno<=0||portno>65535) return -1;
        u->port=portno;
    } else { hl=he-hs; }
    if(hl==0||hl>=sizeof u->host) return -1;
    for(uint32_t i=0;i<hl;i++) u->host[i]=tolower((unsigned char)hs[i]);
    u->host[hl]=0;
    /* path+query */
    if(*p=='#'){ strcpy(u->path,"/"); return 0; }
    if(!*p){ strcpy(u->path,"/"); return 0; }
    uint32_t pl=0;
    while(*p && *p!='#' && pl<sizeof(u->path)-1) u->path[pl++]=*p++;
    u->path[pl]=0;
    return 0;
}

void url_full(const KUrl *u, Str *out){
    str_appf(out,"%s://%s",u->scheme,u->host);
    int default_port = (u->secure&&u->port==443)||(!u->secure&&u->port==80);
    if(!default_port) str_appf(out,":%u",u->port);
    if(u->path[0]!='/') str_appc(out,'/');
    str_append(out,u->path,strlen(u->path));
}

static void url_dir(const KUrl *u, Str *out){
    const char *slash=strrchr(u->path,'/');
    str_appf(out,"%s://%s",u->scheme,u->host);
    int default_port = (u->secure&&u->port==443)||(!u->secure&&u->port==80);
    if(!default_port) str_appf(out,":%u",u->port);
    if(slash){ str_append(out,u->path,slash-u->path+1); }
    else str_appc(out,'/');
}

void url_resolve(KUrl *base, const char *rel, KUrl *out){
    if(!url_parse(out,rel)){ return; } /* absoluta ok */
    /* relatives especiais */
    while(*rel==' '||*rel=='\t'||*rel=='\n'||*rel=='\r')rel++;
    if(!strncmp(rel,"javascript:",11)||!strncmp(rel,"mailto:",7)||!strncmp(rel,"data:",5)
       ||!strncmp(rel,"tel:",4)||!strncmp(rel,"blob:",5)){ out->host[0]=0; memcpy(out,base,sizeof *base); return; }
    if(rel[0]=='#'){ memcpy(out,base,sizeof *out); return; }
    KUrl b=*base;
    memset(out,0,sizeof *out);
    memcpy(out->scheme,b.scheme,sizeof out->scheme);
    memcpy(out->host,b.host,sizeof out->host);
    out->port=b.port; out->secure=b.secure;
    if(rel[0]=='/'){
        snprintf(out->path,sizeof out->path,"%.*s",2047,rel);
        return;
    }
    /* relativo ao diretorio */
    Str dir; str_init(&dir);
    url_dir(&b,&dir);
    char *d=str_take(&dir);
    /* remove ./ e resolve ../ */
    Str res; str_init(&res);
    str_append(&res,d,strlen(d)); free(d);
    const char *p=rel;
    while(*p){
        if(!strncmp(p,"./",2)){p+=2;continue;}
        if(!strncmp(p,"../",3)){
            /* sobe um nivel no resultado */
            if(res.len){
                res.p[res.len]=0;
                char *sl=strrchr(res.p,'/');
                if(sl){ res.len=sl-res.p; res.p[res.len]=0; }
            }
            p+=3; continue;
        }
        break;
    }
    /* remove barra extra? d ja termina com / */
    str_append(&res,p,strlen(p));
    /* separa query */
    char *q=strchr(res.p,'?');
    (void)q;
    snprintf(out->path,sizeof out->path,"%.*s",2047,res.p);
    str_free(&res);
}

void search_url_for(const char *q, Str *out){
    /* se parece URL, vai direto */
    KUrl t;
    if(!url_parse(&t,q) && t.host[0]) { url_full(&t,out); return; }
    if(!strncmp(q,"http://",7)||!strncmp(q,"https://",8)){ str_append(out,q,strlen(q)); return; }
    str_append(out,"https://duckduckgo.com/html/?q=",29);
    /* percent-encode */
    for(const unsigned char *p=(const unsigned char*)q;*p;p++){
        if(isalnum(*p)||*p=='-'||*p=='_'||*p=='.'||*p=='~') str_appc(out,*p);
        else if(*p==' ') str_appc(out,'+');
        else str_appf(out,"%%%02X",*p);
    }
}

/* ======================= Inflate (RFC1951) ========================= */
typedef struct {
    const uint8_t *in; uint32_t inpos, inlen;
    uint32_t bitbuf; int nbits;
    uint8_t *out; uint32_t outlen, outcap;
    /* fixed huffman tables */
    int16_t fix_len_counts[16];
    uint16_t fix_symbols[288];
} Inf;

static uint32_t inf_bits(Inf*z,int need){
    while(z->nbits<need){
        if(z->inpos>=z->inlen) return 0xFFFFFFFF;
        z->bitbuf |= (uint32_t)z->in[z->inpos++]<<z->nbits;
        z->nbits+=8;
    }
    uint32_t v=z->bitbuf&((1u<<need)-1);
    z->bitbuf>>=need; z->nbits-=need;
    return v;
}

typedef struct { int counts[16]; uint16_t symbols[320]; } Huff;

static int build_huff(Huff*h,const uint8_t*lens,int n){
    for(int i=0;i<16;i++)h->counts[i]=0;
    for(int i=0;i<n;i++)h->counts[lens[i]]++;
    h->counts[0]=0;
    int offs[16]; offs[0]=0;
    for(int i=1;i<16;i++)offs[i]=offs[i-1]+h->counts[i-1];
    for(int i=0;i<n;i++) if(lens[i]) h->symbols[offs[lens[i]]++]=i;
    return 0;
}

/* decode canonico estilo puff.c */
static int dcode(Inf*z,Huff*h){
    int code=0,first=0,index=0;
    for(int len=1;len<=15;len++){
        code|=(int)inf_bits(z,1);
        int count=h->counts[len];
        if(code-first<count){
            return h->symbols[index+(code-first)];
        }
        index+=count;
        first=(first+count)<<1;
        code<<=1;
    }
    return -1;
}

static int inflate_raw(Inf*z){
    static const uint8_t lens_codes[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    static uint8_t dll[288],dl[32];
    Huff lc,dc;
    for(;;){
        uint32_t last=inf_bits(z,1);
        uint32_t type=inf_bits(z,2);
        if(type==0){ /* stored */
            z->bitbuf>>=(z->nbits&7); z->nbits&=~7;
            if(z->inpos+4>z->inlen) return -1;
            uint32_t len=z->in[z->inpos]|(z->in[z->inpos+1]<<8);
            z->inpos+=4;
            if(z->inpos+len>z->inlen) return -1;
            if(z->outlen+len>z->outcap){
                uint32_t nc=z->outcap?z->outcap:65536;
                while(nc<z->outlen+len)nc*=2;
                z->out=realloc(z->out,nc); z->outcap=nc;
            }
            memcpy(z->out+z->outlen,z->in+z->inpos,len);
            z->outlen+=len; z->inpos+=len;
        } else if(type==1 || type==2){
            if(type==1){
                for(int i=0;i<144;i++)dll[i]=8;
                for(int i=144;i<256;i++)dll[i]=9;
                for(int i=256;i<280;i++)dll[i]=7;
                for(int i=280;i<288;i++)dll[i]=8;
                build_huff(&lc,dll,288);
                for(int i=0;i<32;i++)dl[i]=5;
                build_huff(&dc,dl,32);
            } else {
                uint32_t hlit=inf_bits(z,5)+257, hdist=inf_bits(z,5)+1, hclen=inf_bits(z,4)+4;
                if(hlit>288||hdist>32) return -1;
                uint8_t cl[19];
                for(uint32_t i=0;i<19;i++)cl[i]=0;
                for(uint32_t i=0;i<hclen;i++) cl[lens_codes[i]]=inf_bits(z,3);
                Huff ch; build_huff(&ch,cl,19);
                uint32_t total=hlit+hdist; uint32_t index=0;
                while(index<total){
                    int sym=dcode(z,&ch);
                    if(sym<0)return -1;
                    if(sym<16){
                        if(index<288)dll[index]=sym;else dl[index-288]=sym;
                        index++;
                    }
                    else if(sym==16){
                        if(!index)return -1;
                        uint8_t prev = (index<=288)? dll[index-1] : dl[index-289];
                        uint32_t rep=3+inf_bits(z,2);
                        for(;rep&&index<total;rep--){
                            if(index<288)dll[index]=prev;else dl[index-288]=prev;
                            index++;
                        }
                    }
                    else if(sym==17){ uint32_t rep=3+inf_bits(z,3); index+=rep; }
                    else { uint32_t rep=11+inf_bits(z,7); index+=rep; }
                }
                if(dll[256]==0)return -1;
                build_huff(&lc,dll,hlit);
                build_huff(&dc,dl,hdist);
            }
            static const uint16_t lbase[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
            static const uint8_t lext[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
            static const uint16_t dbase[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
            static const uint8_t dext[30]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
            for(;;){
                int sym=dcode(z,&lc);
                if(sym<0)return -1;
                if(sym<256){
                    if(z->outlen>=z->outcap){
                        uint32_t nc=z->outcap?z->outcap*2:65536;
                        z->out=realloc(z->out,nc); z->outcap=nc;
                    }
                    z->out[z->outlen++]=sym;
                } else if(sym==256) break;
                else {
                    sym-=257;
                    if(sym>=29)return -1;
                    uint32_t length=lbase[sym]+inf_bits(z,lext[sym]);
                    int ds=dcode(z,&dc);
                    if(ds<0||ds>=30)return -1;
                    uint32_t dist=dbase[ds]+inf_bits(z,dext[ds]);
                    if(dist>z->outlen)return -1;
                    if(z->outlen+length>z->outcap){
                        uint32_t nc=z->outcap?z->outcap:65536;
                        while(nc<z->outlen+length)nc*=2;
                        z->out=realloc(z->out,nc); z->outcap=nc;
                    }
                    uint8_t *src=z->out+z->outlen-dist;
                    for(uint32_t i=0;i<length;i++) z->out[z->outlen+i]=src[i];
                    z->outlen+=length;
                }
            }
        } else return -1;
        if(last) break;
    }
    return 0;
}

int gunzip_buf(const uint8_t *in, uint32_t inlen, Str *out){
    Inf z; memset(&z,0,sizeof z);
    z.in=in; z.inlen=inlen;
    if(inlen>2 && in[0]==0x1f && in[1]==0x8b){
        /* gzip header */
        if(inlen<10) return -1;
        uint32_t p=10;
        uint8_t flg=in[3];
        if(flg&4){ if(p+2>inlen)return -1; p+=2+in[p]+(in[p+1]<<8); }
        if(flg&8){ while(p<inlen&&in[p])p++; p++; }
        if(flg&16){ while(p<inlen&&in[p])p++; p++; }
        if(flg&2) p+=2;
        z.inpos=p;
    } else if(inlen>2 && in[0]==0x78){
        z.inpos=2; /* zlib: pula header de 2 bytes */
    } else z.inpos=0;
    z.out=NULL; z.outlen=0; z.outcap=0;
    if(inflate_raw(&z)!=0){ free(z.out); return -1; }
    str_append(out,(char*)z.out,z.outlen);
    free(z.out);
    return 0;
}

/* ===================== TLS via dlopen(OpenSSL) ====================== */
typedef void*(*SSL_CTX_new_fn)(const void*);
typedef int(*SSL_CTX_use_certificate_fn)(void*,const char*);
typedef void*(*SSL_new_fn)(void*);
typedef int(*SSL_set_fd_fn)(void*,int);
typedef int(*SSL_connect_fn)(void*);
typedef int(*SSL_read_fn)(void*,void*,int);
typedef int(*SSL_write_fn)(void*,const void*,int);
typedef int(*SSL_get_error_fn)(void*,int);
typedef void(*SSL_free_fn)(void*);
typedef void(*SSL_CTX_free_fn)(void*);
typedef void(*OPENSSL_init_fn)(uint64_t,void*);
typedef const void*(*TLS_method_fn)(void);
typedef long(*SSL_ctrl_fn)(void*,int,long,void*);
typedef void(*SSL_set_tlsext_host_name_impl)(void*,const char*);

static struct {
    void *lib;
    SSL_CTX_new_fn ctx_new;
    SSL_new_fn ssl_new;
    SSL_set_fd_fn set_fd;
    SSL_connect_fn conn;
    SSL_read_fn read;
    SSL_write_fn write;
    SSL_get_error_fn get_err;
    SSL_free_fn free_ssl;
    SSL_CTX_free_fn ctx_free;
    TLS_method_fn method;
    OPENSSL_init_fn oinit;
    void *ctx;
    int ok;
} tls;

static void tls_try_load(void){
    const char *names[]={"libssl.so.3","libssl.so","libssl.so.1.1",0};
    for(int i=0;names[i]&&!tls.lib;i++) tls.lib=dlopen(names[i],RTLD_NOW|RTLD_GLOBAL);
    if(!tls.lib){ return; }
    #define GET(v,sym) do{ *(void**)&v=dlsym(tls.lib,sym); if(!v) return; }while(0)
    GET(tls.ctx_new,"SSL_CTX_new");
    GET(tls.ssl_new,"SSL_new");
    GET(tls.set_fd,"SSL_set_fd");
    GET(tls.conn,"SSL_connect");
    GET(tls.read,"SSL_read");
    GET(tls.write,"SSL_write");
    GET(tls.get_err,"SSL_get_error");
    GET(tls.free_ssl,"SSL_free");
    GET(tls.ctx_free,"SSL_CTX_free");
    GET(tls.method,"TLS_client_method");
    void *ini=dlsym(tls.lib,"OPENSSL_init_ssl");
    if(ini) tls.oinit=(OPENSSL_init_fn)ini;
    #undef GET
    if(tls.oinit) tls.oinit(0,NULL);
    tls.ctx=tls.ctx_new(tls.method());
    if(!tls.ctx) return;
    /* SNI helper: SSL_ctrl SSL_CTRL_SET_TLSEXT_HOSTNAME=55 TLSEXT_NAMETYPE_host_name=0 */
    tls.ok=1;
}

static long ssl_ctrl(void*ssl,int op,long a,void*p){
    static SSL_ctrl_fn fn=NULL; static int tried=0;
    if(!tried){ tried=1; *(void**)&fn=dlsym(tls.lib,"SSL_ctrl"); }
    return fn? fn(ssl,op,a,p) : -1;
}

/* ===================== DNS cache ==================================== */
#define DNS_BUCKETS 512
typedef struct DnsNode {
    char host[256];
    char ip[64];
    time_t expiry;
    struct DnsNode *next;
} DnsNode;
static DnsNode *g_dns[DNS_BUCKETS];
static pthread_mutex_t g_net_mtx=PTHREAD_MUTEX_INITIALIZER;

static int dns_lookup_cached(const char *host, char *ip_out, int *reused_hint){
    uint32_t h=fnv1a(host,strlen(host))&(DNS_BUCKETS-1);
    time_t now=time(NULL);
    pthread_mutex_lock(&g_net_mtx);
    for(DnsNode *d=g_dns[h];d;d=d->next){
        if(!strcmp(d->host,host)&&d->expiry>now){
            strcpy(ip_out,d->ip);
            pthread_mutex_unlock(&g_net_mtx);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_net_mtx);
    /* getaddrown: usa libc resolver (hosts file + DNS do SO) */
    struct addrinfo hints={0},*res=0;
    hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
    if(getaddrinfo(host,"80",&hints,&res)!=0||!res) return -1;
    struct sockaddr_in *sa=(struct sockaddr_in*)res->ai_addr;
    inet_ntop(AF_INET,&sa->sin_addr,ip_out,64);
    freeaddrinfo(res);
    DnsNode *d=malloc(sizeof *d);
    strcpy(d->host,host); strcpy(d->ip,ip_out);
    d->expiry=now+600; /* TTL 10 min */
    pthread_mutex_lock(&g_net_mtx);
    d->next=g_dns[h]; g_dns[h]=d;
    pthread_mutex_unlock(&g_net_mtx);
    (void)reused_hint;
    return 0;
}

/* ===================== Connection pool ============================== */
#define POOL_MAX 32
typedef struct PConn {
    int fd; int secure; void *ssl;
    char host[256]; uint16_t port;
    time_t idle_since;
    struct PConn *next;
} PConn;
static PConn *g_pool;

static void pool_put(PConn *c){
    pthread_mutex_lock(&g_net_mtx);
    c->idle_since=time(NULL);
    c->next=g_pool; g_pool=c;
    pthread_mutex_unlock(&g_net_mtx);
}
static PConn *pool_get(const char *host,uint16_t port,int secure){
    pthread_mutex_lock(&g_net_mtx);
    PConn **pp=&g_pool;
    while(*pp){
        PConn *c=*pp;
        if(c->secure==secure && c->port==port && !strcmp(c->host,host)){
            *pp=c->next;
            pthread_mutex_unlock(&g_net_mtx);
            /* descarta se ocioso ha muito tempo */
            if(time(NULL)-c->idle_since>55){ 
                if(c->ssl)tls.free_ssl(c->ssl); close(c->fd); free(c);
                pthread_mutex_lock(&g_net_mtx); pp=&g_pool; continue;
            }
            return c;
        }
        pp=&c->next;
    }
    pthread_mutex_unlock(&g_net_mtx);
    return NULL;
}
static void pool_evict_expired(void){
    pthread_mutex_lock(&g_net_mtx);
    PConn **pp=&g_pool;
    time_t now=time(NULL);
    while(*pp){
        PConn *c=*pp;
        if(now-c->idle_since>60){
            *pp=c->next;
            if(c->ssl)tls.free_ssl(c->ssl);
            close(c->fd); free(c);
        } else pp=&c->next;
    }
    pthread_mutex_unlock(&g_net_mtx);
}

static int sock_send_all(int fd,void*ssl,const char*buf,size_t n){
    size_t off=0;
    while(off<n){
        ssize_t w;
        if(ssl) w=tls.write(ssl,buf+off,n-off);
        else w=send(fd,buf+off,n-off,MSG_NOSIGNAL);
        if(w<=0){
            if(w<0&&errno==EINTR)continue;
            return -1;
        }
        off+=w;
    }
    return 0;
}

/* recv com timeout */
static ssize_t sock_recv_t(int fd,void*ssl,char*buf,size_t n,int timeout_ms){
    struct timeval tv={timeout_ms/1000,(timeout_ms%1000)*1000};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
    for(;;){
        ssize_t r;
        if(ssl) r=tls.read(ssl,buf,n);
        else r=recv(fd,buf,n,0);
        if(r<0){
            if(errno==EINTR)continue;
            if(ssl){
                int e=tls.get_err(ssl,r);
                if(e==2||e==3) continue; /* want read/write */
            }
            return -1;
        }
        return r;
    }
}

static PConn *conn_open(const char *host,uint16_t port,int secure,double *t_dns,double *t_conn,double *t_tls,int *reused){
    double t0=now_sec();
    PConn *c=pool_get(host,port,secure);
    if(c){ *reused=1; *t_dns=*t_conn=*t_tls=0; return c; }
    char ip[64];
    if(dns_lookup_cached(host,ip,NULL)!=0) return NULL;
    double t1=now_sec(); *t_dns=t1-t0;
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0)return NULL;
    int one=1; setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof one);
    struct timeval sv={5,0}; setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&sv,sizeof sv);
    struct sockaddr_in sa={0};
    sa.sin_family=AF_INET; sa.sin_port=htons(port);
    inet_pton(AF_INET,ip,&sa.sin_addr);
    double tc=now_sec();
    if(connect(fd,(struct sockaddr*)&sa,sizeof sa)!=0){ close(fd); return NULL; }
    *t_conn=now_sec()-tc;
    void *ssl=NULL;
    if(secure){
        if(!tls.ok){ close(fd); return NULL; }
        ssl=tls.ssl_new(tls.ctx);
        if(!ssl){ close(fd); return NULL; }
        tls.set_fd(ssl,fd);
        /* SNI */
        long hostname_op=55; char *hn=(char*)host;
        ssl_ctrl(ssl,hostname_op,0,hn);
        double tt=now_sec();
        if(tls.conn(ssl)!=1){ tls.free_ssl(ssl); close(fd); return NULL; }
        *t_tls=now_sec()-tt;
    } else { *t_tls=0; }
    c=malloc(sizeof *c);
    c->fd=fd;c->secure=secure;c->ssl=ssl;
    strcpy(c->host,host);c->port=port;
    return c;
}

/* ===================== Cache HTTP em memoria ======================== */
#define CACHE_BUCKETS 2048
typedef struct CEntry {
    char *url; char *data; uint32_t len;
    char *ct, *cs;
    time_t expiry;
    struct CEntry *next;
} CEntry;
static CEntry *g_cache[CACHE_BUCKETS];
static uint32_t g_cache_bytes;
#define CACHE_MAX_BYTES (64*1024*1024)

int net_cache_lookup(const char *url, char **data, uint32_t *len, char **ct, char **cs){
    uint32_t h=fnv1a(url,strlen(url))&(CACHE_BUCKETS-1);
    time_t now=time(NULL);
    pthread_mutex_lock(&g_net_mtx);
    for(CEntry *e=g_cache[h];e;e=e->next){
        if(e->expiry>now && !strcmp(e->url,url)){
            *data=e->data; *len=e->len; *ct=e->ct; *cs=e->cs;
            pthread_mutex_unlock(&g_net_mtx);
            return 1;
        }
    }
    pthread_mutex_unlock(&g_net_mtx);
    return 0;
}

static void cache_evict_some(void){
    /* heuristica: remove entradas expiradas; senao remove metade arbitraria */
    for(int b=0;b<CACHE_BUCKETS;b++){
        CEntry **pp=&g_cache[b];
        while(*pp){
            CEntry *e=*pp;
            if(e->expiry<time(NULL)){
                *pp=e->next;
                g_cache_bytes-=e->len;
                free(e->url);free(e->data);free(e->ct);free(e->cs);free(e);
            } else pp=&e->next;
        }
    }
}

void net_cache_store(const char *url, const char *data, uint32_t len, const char *ct, const char *cs, int max_age){
    if(max_age<=0||len==0||len>(8*1024*1024))return;
    pthread_mutex_lock(&g_net_mtx);
    while(g_cache_bytes+len>CACHE_MAX_BYTES){ cache_evict_some(); if(g_cache_bytes==0)break; 
        /* se nada expirou, derruba entradas LRU grosseiras: zera buckets pares */
        static int pass=0; pass++;
        int removed=0;
        for(int b=pass%CACHE_BUCKETS, cnt=0; cnt<64 && b<CACHE_BUCKETS; b=(b+1)%CACHE_BUCKETS,cnt++){
            CEntry *e=g_cache[b];
            if(e){ g_cache[b]=e->next; g_cache_bytes-=e->len;
                free(e->url);free(e->data);free(e->ct);free(e->cs);free(e); removed=1; }
        }
        if(!removed) break;
    }
    uint32_t h=fnv1a(url,strlen(url))&(CACHE_BUCKETS-1);
    /* substitui existente */
    for(CEntry *e=g_cache[h];e;e=e->next){
        if(!strcmp(e->url,url)){
            e->expiry=time(NULL)+max_age;
            pthread_mutex_unlock(&g_net_mtx);
            return; /* conteudo antigo mantido; simples */
        }
    }
    CEntry *e=calloc(1,sizeof *e);
    e->url=strdup(url);
    e->data=malloc(len); memcpy(e->data,data,len); e->len=len;
    e->ct=ct?strdup(ct):NULL; e->cs=cs?strdup(cs):NULL;
    e->expiry=time(NULL)+max_age;
    e->next=g_cache[h]; g_cache[h]=e;
    g_cache_bytes+=len;
    pthread_mutex_unlock(&g_net_mtx);
}

/* parseia max-age do Cache-Control */
static int parse_max_age(const char *cc){
    if(!cc)return 0;
    const char *p=strcasestr(cc,"max-age");
    if(!p)return 0;
    p+=7; while(*p&&*p!='='&&!isdigit(*p))p++;
    if(*p=='=')p++;
    return atoi(p);
}

/* ===================== HTTP/1.1 ===================================== */
static const char *UA="Mozilla/5.0 (X11; Linux x86_64) Koth/1.0 (KHronos Optimization & Traversal Engine)";

static int http_once(HttpResponse *r, const KUrl *u, double *tdns,double *tconn,double *ttls){
    int reused=0;
    PConn *c=conn_open(u->host,u->port,u->secure,tdns,tconn,ttls,&reused);
    r->reused_conn=reused;
    if(!c){ snprintf(r->err,sizeof r->err,"conexao falhou com %s (porta %u)",u->host,u->port); return -1; }
    /* request */
    Str req; str_init(&req);
    str_appf(&req,"GET %s HTTP/1.1\r\n",u->path[0]?u->path:"/");
    str_appf(&req,"Host: %s\r\n",u->host);
    str_appf(&req,"User-Agent: %s\r\n",UA);
    str_apps(&req,"Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/png,image/jpeg,image/gif,*/*;q=0.8\r\n");
    str_apps(&req,"Accept-Encoding: gzip, deflate\r\n");
    str_apps(&req,"Accept-Language: pt-BR,pt;q=0.9,en;q=0.8\r\n");
    str_apps(&req,"Connection: keep-alive\r\n\r\n");
    if(sock_send_all(c->fd,c->ssl,req.p,req.len)!=0){
        str_free(&req);
        close(c->fd); if(c->ssl)tls.free_ssl(c->ssl); free(c);
        snprintf(r->err,sizeof r->err,"falha ao enviar requisicao");
        return -1;
    }
    str_free(&req);
    /* resposta */
    Str raw; str_init(&raw);
    char tmp[16384];
    int header_done=0; uint32_t hdr_end=0; uint32_t hdr_start_hint=0;
    int chunked=0; long content_length=-1;
    int status=0;
    char cc_hdr[256]={0}, loc_hdr[1024]={0}, ctype[256]={0};
    Str body; str_init(&body);
    double t_first=0; double t0h=now_sec();
    for(;;){
        ssize_t n=sock_recv_t(c->fd,c->ssl,tmp,sizeof tmp,10000);
        if(n<=0){
            str_free(&raw); str_free(&body);
            close(c->fd); if(c->ssl)tls.free_ssl(c->ssl); free(c);
            snprintf(r->err,sizeof r->err,"conexao encerrada pelo servidor / timeout");
            return -1;
        }
        if(!t_first) t_first=now_sec();
        r->t_first=t_first-t0h;
        if(!header_done){
            str_append(&raw,tmp,n);
            /* procura \r\n\r\n */
            if(raw.len>=4){
                uint32_t k0=(hdr_start_hint>hdr_end)?hdr_start_hint:0;
                for(uint32_t k=k0;k+1<raw.len;k++){
                    if(raw.p[k]!='\n')continue;
                    if(raw.p[k+1]=='\n'){ hdr_end=k+2; break; }
                    if(k+2<raw.len&&raw.p[k+1]=='\r'&&raw.p[k+2]=='\n'){ hdr_end=k+3; break; }
                }
                hdr_start_hint=raw.len>2?raw.len-2:0;
            }
            if(hdr_end){
                header_done=1;
                /* status line */
                if(raw.len<12||memcmp(raw.p,"HTTP/",5)){
                    str_free(&raw);str_free(&body);close(c->fd);if(c->ssl)tls.free_ssl(c->ssl);free(c);
                    snprintf(r->err,sizeof r->err,"resposta invalida (nao-HTTP)");
                    return -1;
                }
                status=atoi(raw.p+9);
                /* headers: parseia lowercase keys */
                char *hdrs=strndup(raw.p,min_u32(hdr_end,raw.len));
                char *line=strtok(hdrs,"\r\n");
                line=strtok(NULL,"\r\n");
                while(line){
                    char *colon=strchr(line,':');
                    if(colon){
                        *colon=0;
                        for(char *q=line;*q;q++)*q=tolower(*q);
                        char *val=colon+1; while(*val==' ')val++;
                        if(!strcmp(line,"content-length")) content_length=atol(val);
                        else if(!strcmp(line,"transfer-encoding")&&strstr(val,"chunked")) chunked=1;
                        else if(!strcmp(line,"content-type")){ snprintf(ctype,sizeof ctype,"%s",val);
                            char *cs=strcasestr(val,"charset=");
                            if(cs){ cs+=8; char *e2=cs; while(*e2&&*e2!=';'&&*e2!=' '&&*e2!='"')e2++;
                                r->charset=malloc(e2-cs+1); memcpy(r->charset,cs,e2-cs); r->charset[e2-cs]=0; }
                        }
                        else if(!strcmp(line,"cache-control")) snprintf(cc_hdr,sizeof cc_hdr,"%s",val);
                        else if(!strcmp(line,"location")) snprintf(loc_hdr,sizeof loc_hdr,"%s",val);
                    }
                    line=strtok(NULL,"\r\n");
                }
                free(hdrs);
                /* corpo ja recebido apos headers */
                if(raw.len>hdr_end) str_append(&body,raw.p+hdr_end,raw.len-hdr_end);
            }
        } else {
            str_append(&body,tmp,n);
        }
        if(!header_done) continue;
        if(chunked){
            /* processa chunks: corpo em body.p; tenta completar */
            /* descompacta incrementalmente */
            Str dec; str_init(&dec);
            uint32_t pos=0; int done=0;
            char *bp=body.p; uint32_t bl=body.len;
            for(;;){
                /* linha de tamanho */
                char *nl=(char*)memmem(bp+pos,bl-pos,"\r\n",2);
                if(!nl) break;
                char lenbuf[24]={0};
                uint32_t ll=min_u32(nl-(bp+pos),20);
                memcpy(lenbuf,bp+pos,ll);
                long sz=strtol(lenbuf,NULL,16);
                pos=(nl-bp)+2;
                if(sz==0){ done=1; break; }
                if(pos+sz>bl) break;
                str_append(&dec,bp+pos,sz);
                pos+=sz;
                if(pos+2<=bl) pos+=2;
            }
            if(done){
                /* tail: possivel trailer ignorado */
                str_free(&body);
                /* decompress se gzip magic */
                if(dec.len>2&&(uint8_t)dec.p[0]==0x1f&&(uint8_t)dec.p[1]==0x8b){
                    Str dz; str_init(&dz);
                    if(gunzip_buf((uint8_t*)dec.p,dec.len,&dz)==0){ str_free(&dec); dec=dz; }
                    else str_free(&dz);
                }
                body=dec;
                break;
            }
            str_free(&dec);
        } else if(content_length>=0){
            if(body.len>=(uint32_t)content_length) break;
        } else {
            /* ate EOF */
            continue;
        }
    }
    /* se nao chunked nem CL: pode ter chegado completo so quando EOF; como quebramos
       apenas nos casos acima, para respostas sem CL assumimos ate timeout -> tratado abaixo */
    if(!chunked && content_length<0){
        /* reler ate EOF */
        for(;;){
            ssize_t n=sock_recv_t(c->fd,c->ssl,tmp,sizeof tmp,3000);
            if(n<=0)break;
            str_append(&body,tmp,n);
        }
    }
    /* desconexoes: se servidor fechou (Connection: close), nao devolve ao pool */
    int keepalive = 1;
    {
        char *low=strndup(raw.p,min_u32(hdr_end?hdr_end:raw.len,4096));
        for(char *q=low;*q;q++)*q=tolower(*q);
        if(strstr(low,"connection: close")) keepalive=0;
        free(low);
    }
    if(!keepalive){
        close(c->fd); if(c->ssl)tls.free_ssl(c->ssl); free(c);
    } else pool_put(c);

    r->status=status;
    r->content_type=strdup(ctype[0]?ctype:"application/octet-stream");

    if(status>=300&&status<400&&loc_hdr[0]){
        /* redirect */
        KUrl nu;
        url_resolve(u,loc_hdr,&nu);
        Str fs;str_init(&fs); url_full(&nu,&fs);
        snprintf(r->final_url,sizeof r->final_url,"%s",fs.p);
        str_free(&fs);
        str_free(&raw);str_free(&body);
        return 2; /* sinaliza redirect */
    }
    if(status<200||status>=400){
        str_free(&raw);str_free(&body);
        snprintf(r->err,sizeof r->err,"HTTP %d",status);
        return -1;
    }
    /* descomprime se necessario */
    Str final_body; str_init(&final_body);
    if(body.len>1&&(uint8_t)body.p[0]==0x1f&&(uint8_t)body.p[1]==0x8b){
        if(gunzip_buf((uint8_t*)body.p,body.len,&final_body)!=0){ str_free(&raw);str_free(&body);snprintf(r->err,sizeof r->err,"gzip invalido");return -1; }
    } else if(body.len>1&&(uint8_t)body.p[0]==0x78&&((uint8_t)body.p[1]==0x9c||(uint8_t)body.p[1]==0xda||(uint8_t)body.p[1]==0x01)){
        if(gunzip_buf((uint8_t*)body.p,body.len,&final_body)!=0){ str_free(&raw);str_free(&body);snprintf(r->err,sizeof r->err,"deflate invalido");return -1; }
    } else {
        str_append(&final_body,body.p,body.len);
    }
    str_free(&raw); str_free(&body);
    r->body=final_body.p; r->body_len=final_body.len;
    if(!r->charset){
        /* detecta charset meta */
        const char *m=strcasestr(r->body,"charset=");
        if(m && (uint32_t)(m-r->body)<min_u32(r->body_len,4096)){
            m+=8; const char *e2=m; while(*e2&&*e2!=';'&&*e2!='"'&&*e2!=' '&&*e2!='\''&&*e2>' ')e2++;
            r->charset=malloc(e2-m+1); memcpy(r->charset,m,e2-m); r->charset[e2-m]=0;
        }
    }
    /* cache */
    int ma=parse_max_age(cc_hdr);
    if(!ma && strstr(cc_hdr,"no-store")) ma=-1;
    if(!ma) ma=60; /* heuristic default curto */
    if(ma>0 && r->body_len<=(4*1024*1024)){
        Str fu;str_init(&fu);url_full(u,&fu);
        net_cache_store(fu.p,r->body,r->body_len,r->content_type,r->charset,ma);
        str_free(&fu);
    }
    return 0;
}

int http_get(HttpResponse *r, const char *url_str){
    memset(r,0,sizeof *r);
    double t_start=now_sec();
    KUrl u;
    if(url_parse(&u,url_str)!=0){ snprintf(r->err,sizeof r->err,"URL invalida: %s",url_str); return -1; }
    int hops=0;
    for(;;){
        Str fu;str_init(&fu);url_full(&u,&fu);
        char *furl=str_take(&fu);
        /* cache lookup */
        char *cd;uint32_t cl;char *cct,*ccs;
        if(net_cache_lookup(furl,&cd,&cl,&cct,&ccs)){
            r->body=malloc(cl);memcpy(r->body,cd,cl);r->body_len=cl;
            r->content_type=strdup(cct?cct:"text/html");
            r->charset=ccs?strdup(ccs):NULL;
            snprintf(r->final_url,sizeof r->final_url,"%s",furl);
            r->status=200;r->ok=1;r->from_cache=1;
            r->t_total=now_sec()-t_start;
            free(furl);
            return 0;
        }
        free(furl);
        double tdns,tconn,ttls;
        int rc=http_once(r,&u,&tdns,&tconn,&ttls);
        r->t_dns=tdns;r->t_conn=tconn;r->t_tls=ttls;
        if(rc==2){
            /* redirect: u <- final */
            hops++;
            if(hops>8){ snprintf(r->err,sizeof r->err,"demasiados redirects"); free(r->body); memset(r,0,sizeof *r); return -1; }
            KUrl nu;
            if(url_parse(&nu,r->final_url)!=0){ snprintf(r->err,sizeof r->err,"redirect invalido"); return -1; }
            u=nu;
            continue;
        }
        r->t_total=now_sec()-t_start;
        if(rc==0){ r->ok=1;
            Str fu2;str_init(&fu2);url_full(&u,&fu2);
            snprintf(r->final_url,sizeof r->final_url,"%s",fu2.p);
            str_free(&fu2);
            return 0;
        }
        return -1;
    }
}

void http_response_free(HttpResponse *r){
    free(r->body); free(r->content_type); free(r->charset);
    memset(r,0,sizeof *r);
}

void net_init(void){
    tls_try_load();
}

/* limpeza periodica chamada pela UI */
void net_tick(void){ pool_evict_expired(); }

/* transcode charset -> UTF-8: suporta latin-1/cp1252 basico */
void charset_to_utf8(const char *in, uint32_t len, const char *cs, Str *out){
    if(!cs){ str_append(out,in,len); return; }
    char low[32]; uint32_t i;
    for(i=0;i<strlen(cs)&&i<31;i++)low[i]=tolower(cs[i]);
    low[i]=0;
    int is_latin = !strcmp(low,"iso-8859-1")||!strcmp(low,"latin-1")||!strcmp(low,"windows-1252")||!strcmp(low,"cp1252");
    if(!is_latin){ str_append(out,in,len); return; }
    for(uint32_t k=0;k<len;k++){
        uint8_t c=in[k];
        if(c<0x80) str_appc(out,c);
        else utf8_put(out,c);
    }
}
