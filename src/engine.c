/* engine.c - Pipeline: fetch + parse + style + layout + paint. Workers de imagem. */
#include "koth.h"
#include <ctype.h>
#include <limits.h>




/* ====================== worker pool (imagens) ======================= */
typedef struct Job {
    char url[512];
    DomNode *node;
    struct Job *next;
} Job;

volatile int g_page_dirty_flag=0;

static Job *g_jobs; static pthread_mutex_t g_job_mtx=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_job_cv=PTHREAD_COND_INITIALIZER;
static volatile int g_workers_run=1;
static pthread_t g_workers[4];
static volatile int g_pending=0;

static void *worker_main(void *arg){
    (void)arg;
    for(;;){
        pthread_mutex_lock(&g_job_mtx);
        while(!g_jobs && g_workers_run)
            pthread_cond_wait(&g_job_cv,&g_job_mtx);
        if(!g_workers_run && !g_jobs){ pthread_mutex_unlock(&g_job_mtx); return NULL; }
        Job *j=g_jobs; g_jobs=j->next;
        pthread_mutex_unlock(&g_job_mtx);
        /* baixa e decodifica */
        HttpResponse r;
        if(http_get(&r,j->url)==0 && r.ok && r.body){
            Image *im=image_decode((uint8_t*)r.body,r.body_len);
            if(im){
                j->node->img=im;
                j->node->is_image=1;
                __sync_fetch_and_or(&g_page_dirty_flag,1);
            }
        }
        http_response_free(&r);
        free(j);
        __sync_sub_and_fetch(&g_pending,1);
    }
}

void img_workers_start(void){
    static int started=0;
    if(started)return; started=1;
    for(int i=0;i<4;i++) pthread_create(&g_workers[i],NULL,worker_main,NULL);
}
static void img_enqueue(const char *url, DomNode *n){
    Job *j=calloc(1,sizeof(Job));
    snprintf(j->url,sizeof j->url,"%s",url);
    j->node=n;
    __sync_add_and_fetch(&g_pending,1);
    pthread_mutex_lock(&g_job_mtx);
    j->next=g_jobs; g_jobs=j;
    pthread_cond_signal(&g_job_cv);
    pthread_mutex_unlock(&g_job_mtx);
}
int imgs_pending(void){ return g_pending; }

/* =============== title / base URL walk ============================== */
static void find_title(DomNode *n,char *out,size_t cap){
    if(out[0])return;
    if(n->type==NODE_ELEMENT&&!strcmp(n->name,"title")){
        size_t o=0;
        for(uint32_t i=0;i<n->n_children&&o<cap-1;i++){
            DomNode *t=n->children[i];
            if(t->type==NODE_TEXT){
                size_t m=min_u32(t->text_len,cap-1-o);
                memcpy(out+o,t->text,m); o+=m; out[o]=0;
            }
        }
        /* trim */
        size_t s=0,e=o;
        while(s<e&&isspace((unsigned char)out[s]))s++;
        while(e>s&&isspace((unsigned char)out[e-1]))e--;
        size_t j=0; for(size_t i2=s;i2<e;i2++)out[j++]=out[i2];
        out[j]=0;
        return;
    }
    for(uint32_t i=0;i<n->n_children;i++) find_title(n->children[i],out,cap);
}

/* marca links/imagens e resolve URLs relativas */
static void mark_links_images(DomNode *root, KUrl *base){
    DomNode **stack=malloc(sizeof(void*)*128); int sn=0,cap=128;
    stack[sn++]=root;
    while(sn){
        DomNode *n=stack[--sn];
        if(n->type==NODE_ELEMENT){
            if(!strcmp(n->name,"a")){
                const char *href=dom_attr(n,"href");
                if(href&&href[0]){
                    KUrl u; url_resolve(base,href,&u);
                    Str fs;str_init(&fs);url_full(&u,&fs);
                    snprintf(n->link_url,sizeof n->link_url,"%s",fs.p);
                    str_free(&fs);
                    n->has_link=1;
                }
            }
            if(!strcmp(n->name,"img")){
                const char *src=dom_attr(n,"src");
                if(src&&src[0]){
                    KUrl u; url_resolve(base,src,&u);
                    Str fs;str_init(&fs);url_full(&u,&fs);
                    snprintf(n->src_url,sizeof n->src_url,"%s",fs.p);
                    str_free(&fs);
                    n->is_image=1;
                }
            }
        }
        if(sn+(int)n->n_children>cap){cap=(sn+n->n_children)*2;stack=realloc(stack,cap*sizeof(void*));}
        for(uint32_t c=0;c<n->n_children;c++) stack[sn++]=n->children[c];
    }
    free(stack);
}

/* enqueue imagens sem dados */
static void queue_images(DomNode *root){
    DomNode **stack=malloc(sizeof(void*)*128); int sn=0,cap=128;
    stack[sn++]=root;
    while(sn){
        DomNode *n=stack[--sn];
        if(n->is_image&&!n->img&&n->src_url[0]&&strncmp(n->src_url,"data:",5))
            img_enqueue(n->src_url,n);
        if(sn+(int)n->n_children>cap){cap=(sn+n->n_children)*2;stack=realloc(stack,cap*sizeof(void*));}
        for(uint32_t c=0;c<n->n_children;c++) stack[sn++]=n->children[c];
    }
    free(stack);
}

/* ==================== page lifecycle ================================ */
void page_init(Page *pg){
    memset(pg,0,sizeof *pg);
    arena_init(&pg->arena,1<<20);
    css_init(&pg->css);
    pg->viewport_w=1024; pg->viewport_h=768;
    strcpy(pg->title,"Nova aba");
}

int page_load(Page *pg, const char *url_str){
    double t0=now_sec();
    HttpResponse r;
    if(http_get(&r,url_str)!=0){
        pg->error=1;
        snprintf(pg->errmsg,sizeof pg->errmsg,"%s",r.err[0]?r.err:"falha na requisicao");
        http_response_free(&r);
        return -1;
    }
    pg->error=0;
    snprintf(pg->current_url,sizeof pg->current_url,"%s",r.final_url);
    /* content-type: se nao for html, mostra como texto simples */
    int is_html = !r.content_type || strstr(r.content_type,"html") || strstr(r.content_type,"xml")
                  || strstr(r.content_type,"text/") ;
    /* transcode charset -> utf8 */
    Str utf; str_init(&utf);
    charset_to_utf8(r.body,r.body_len,r.charset,&utf);
    /* limpa estado anterior */
    arena_destroy(&pg->arena);
    arena_init(&pg->arena,1<<20);
    css_free(&pg->css); css_init(&pg->css);
    free(pg->doc.nodes); pg->doc.nodes=NULL; pg->doc.n=pg->doc.cap=0;
    KUrl base; url_parse(&base,r.final_url);
    DomNode *root;
    if(is_html){
        root=dom_parse_html(&pg->arena,&pg->doc,utf.p?utf.p:"",utf.len);
    } else {
        /* texto puro: escapa e envolve em <body><pre> */
        Str syn; str_init(&syn);
        str_apps(&syn,"<body><pre>");
        for(uint32_t k=0;k<utf.len;k++){
            char c=utf.p[k];
            if(c=='<')str_apps(&syn,"&lt;");
            else if(c=='>')str_apps(&syn,"&gt;");
            else if(c=='&')str_apps(&syn,"&amp;");
            else str_appc(&syn,c);
        }
        str_apps(&syn,"</pre></body>");
        root=dom_parse_html(&pg->arena,&pg->doc,syn.p?syn.p:"",syn.len);
        str_free(&syn);
    }
    pg->root=root;
    /* title */
    pg->title[0]=0;
    /* procura a partir do node raiz ate ancestor html: mais facil: busca no doc inteiro */
    for(uint32_t i=0;i<pg->doc.n;i++){
        DomNode *n=pg->doc.nodes[i];
        if(n->type==NODE_ELEMENT&&!strcmp(n->name,"title")){ find_title(n,pg->title,sizeof pg->title); if(pg->title[0])break; }
    }
    if(!pg->title[0]){
        /* fallback: host da URL */
        snprintf(pg->title,sizeof pg->title,"%s",base.host);
    }
    /* external stylesheets (<link rel=stylesheet href>) - fetch sincrono (rapido com keep-alive) */
    for(uint32_t i=0;i<pg->doc.n;i++){
        DomNode *n=pg->doc.nodes[i];
        if(n->type==NODE_ELEMENT&&!strcmp(n->name,"link")){
            const char *rel=dom_attr(n,"rel"), *href=dom_attr(n,"href");
            if(rel&&href&&strcasestr(rel,"stylesheet")){
                KUrl su; url_resolve(&base,href,&su);
                Str fs;str_init(&fs);url_full(&su,&fs);
                HttpResponse sr;
                if(http_get(&sr,fs.p)==0&&sr.ok&&sr.body&&sr.body_len<2*1024*1024)
                    css_parse(&pg->css,sr.body,sr.body_len);
                http_response_free(&sr);
                str_free(&fs);
            }
        }
    }
    /* inline <style> */
    for(uint32_t i=0;i<pg->doc.n;i++){
        DomNode *n=pg->doc.nodes[i];
        if(n->type==NODE_ELEMENT&&!strcmp(n->name,"style")){
            for(uint32_t c=0;c<n->n_children;c++)
                if(n->children[c]->type==NODE_TEXT)
                    css_parse(&pg->css,n->children[c]->text,n->children[c]->text_len);
        }
    }
    /* resolve estilos */
    style_resolve_tree(root,&pg->css);
    /* marca tudo que precisa renderizar: remove DISP_NONE da arvore de fluxo */
    mark_links_images(root,&base);
    img_workers_start();
    queue_images(root);
    page_layout(pg,pg->viewport_w);
    pg->load_time=now_sec()-t0;
    page_push_history(pg,pg->current_url);
    str_free(&utf);
    http_response_free(&r);
    return 0;
}

/* ===================== Layout ======================================= */
/* Modelo simplificado mas fiel ao fluxo normal:
 * - elementos block: empilham verticalmente, largura = container - margens
 * - inline runs: quebra de linha por palavras, alinhamento
 * - tabelas: cada tr vira linha com celulas proporcionais
 */
typedef struct {
    int x,y,w;          /* conteudo area */
    int cursor_y;
    int line_x, line_y, line_w_limit;
    int line_height;
    int align;
    uint8_t in_line;
} FlowCtx;

static int node_is_block(DomNode *n){
    if(n->type==NODE_TEXT) return 0;
    return n->disp==DISP_BLOCK;
}

static int scale_for(int fsize){
    int s=fsize/GLYPH_H;
    if(s<1)s=1;
    return s;
}

static Run *g_runs; static uint32_t g_nruns;

static Run *run_new(DomNode *n,const char *t,int len,int x,int y,int sc){
    Run *r=malloc(sizeof(Run)+len+1);
    r->owner=n; r->text=(char*)r+sizeof(Run);
    memcpy(r->text,t,len); r->text[len]=0;
    r->x=x;r->y=y;r->scale=sc;
    r->color=n->color;
    r->underline=n->underline;r->strikethrough=n->strikethrough;
    r->next=g_runs; g_runs=r; g_nruns++;
    return r;
}
void runs_clear(void){
    Run *r=g_runs;
    while(r){Run*nx=r->next;free(r);r=nx;}
    g_runs=NULL;g_nruns=0;
}
Run *runs_head(void){ return g_runs; }

/* avanca flow colocando word de largura ww; retorna novo ctx */
static void flow_put_word(FlowCtx *f,int ww,int h,int *lx,int *ly,int *lh){
    if(*lx!=f->line_x && *lx+ww>f->x+f->w){
        /* quebra linha */
        f->cursor_y += *lh + 2;
        *lx=f->x;
        *lh=h;
    } else if(*lh<h) *lh=h;
    *ly=f->cursor_y;
}

static void layout_inline_container(DomNode *container, FlowCtx *f);
static void layout_node(DomNode *n, FlowCtx *f);

/* coleta texto direto (descendentes inline) de um bloco */
typedef struct { DomNode *node; int is_text; } InlineItem;
static void gather_inlines(DomNode *n, InlineItem **arr, uint32_t *cnt, uint32_t *cap){
    if(n->type==NODE_TEXT){
        if(*cnt==*cap){ *cap=*cap?*cap*2:64; *arr=realloc(*arr,*cap*sizeof(InlineItem)); }
        (*arr)[(*cnt)++]=(InlineItem){n,1};
        return;
    }
    if(n->disp==DISP_NONE) return;
    if(node_is_block(n)){
        if(*cnt==*cap){ *cap=*cap?*cap*2:64; *arr=realloc(*arr,*cap*sizeof(InlineItem)); }
        (*arr)[(*cnt)++]=(InlineItem){n,0};
        return;
    }
    for(uint32_t i=0;i<n->n_children;i++) gather_inlines(n->children[i],arr,cnt,cap);
}

static void layout_node(DomNode *n, FlowCtx *f){
    if(n->type==NODE_TEXT) return;
    if(n->disp==DISP_NONE){ 
        /* ainda assim processa nada */
        return;
    }
    if(!node_is_block(n)){
        /* inline puro: tratado pelo container */
        return;
    }
    /* BLOCK */
    int ml = n->m_l!=CSS_UNSET?n->m_l:(!strcmp(n->name,"body")?8:0);
    int mr = n->m_r!=CSS_UNSET?n->m_r:(!strcmp(n->name,"body")?8:0);
    int mt = n->m_t!=CSS_UNSET?n->m_t:0;
    int mb = n->m_b!=CSS_UNSET?n->m_b:0;
    int pl = n->p_t!=CSS_UNSET&&n->p_l!=CSS_UNSET? n->p_l : (!strcmp(n->name,"body")?0:0);
    int pt = n->p_t!=CSS_UNSET? n->p_t : 0;
    /* defaults UA de margem p/ headings/p */
    if(!strcmp(n->name,"h1")){ if(mt==0&&n->m_t==CSS_UNSET)mt=21; if(mb==0&&n->m_b==CSS_UNSET)mb=21; }
    if(!strcmp(n->name,"h2")){ if(n->m_t==CSS_UNSET)mt=20; if(n->m_b==CSS_UNSET)mb=20; }
    if(!strcmp(n->name,"h3")){ if(n->m_t==CSS_UNSET)mt=19; if(n->m_b==CSS_UNSET)mb=19; }
    if(!strcmp(n->name,"p")) { if(n->m_t==CSS_UNSET)mt=14; if(n->m_b==CSS_UNSET)mb=14; }
    if(!strcmp(n->name,"ul")||!strcmp(n->name,"ol")){ if(n->m_t==CSS_UNSET)mt=16; if(n->m_b==CSS_UNSET)mb=16; if(n->m_l==CSS_UNSET)ml=40; }
    if(!strcmp(n->name,"li")){ if(n->m_t==CSS_UNSET)mt=2; if(n->m_l==CSS_UNSET)ml=0; }
    if(!strcmp(n->name,"table")){ if(n->m_t==CSS_UNSET)mt=14; }
    if(!strcmp(n->name,"hr")){ if(n->m_t==CSS_UNSET)mt=8; if(n->m_b==CSS_UNSET)mb=8; }

    int x=f->x+ml+pl;
    int w=f->w-ml-mr-pl-(n->p_r!=CSS_UNSET?n->p_r:0);
    if(w<20)w=20;
    f->cursor_y += mt;
    n->x=x-(n->p_l!=CSS_UNSET?n->p_l:0);
    n->y=f->cursor_y;
    n->tx=x; n->ty=f->cursor_y; n->tw=w;

    if(!strcmp(n->name,"hr")){
        n->h=2; f->cursor_y+=2+mb;
        n->w=w;
        return;
    }
    if(!strcmp(n->name,"img")||n->is_image){
        /* dimensoes atributos ou naturais */
        const char *aw=dom_attr(n,"width"),*ah=dom_attr(n,"height");
        int iw=aw?atoi(aw):0, ih=ah?atoi(ah):0;
        if(n->img){ if(!iw)iw=n->img->w; if(!ih)ih=n->img->h; }
        if(iw<=0)iw=240; if(ih<=0)ih=120;
        if(iw>w)iw=w;
        n->x=x; n->w=iw; n->h=ih;
        f->cursor_y+=ih+mb;
        return;
    }

    /* ===== fluxo dos filhos: sequencias inline + blocos em ordem ===== */
    FlowCtx sub={ .x=x, .w=w, .cursor_y=f->cursor_y };
    int block_top=f->cursor_y;
    uint32_t ci=0;
    while(ci<n->n_children){
        DomNode *c=n->children[ci];
        int child_is_block = (c->type==NODE_ELEMENT)&&node_is_block(c);
        if(child_is_block){
            layout_node(c,&sub);
            ci++;
            continue;
        }
        if(c->type==NODE_ELEMENT&&c->disp==DISP_NONE){ ci++; continue; }
        /* acumula sequencia inline (texto ou inline elements) */
        InlineItem *items=NULL; uint32_t cnt=0,cap=0;
        while(ci<n->n_children){
            DomNode *cc=n->children[ci];
            int cb=(cc->type==NODE_ELEMENT)&&node_is_block(cc);
            if(cb)break;
            if(cc->type==NODE_ELEMENT&&cc->disp==DISP_NONE){ci++;continue;}
            gather_inlines(cc,&items,&cnt,&cap);
            ci++;
        }
        /* layout da linha */
        int sc0=scale_for(n->fsize);
        int lx=sub.x, lh=sc0*GLYPH_H+2;
        uint32_t line_start=g_nruns;
        for(uint32_t i=0;i<cnt;i++){
            DomNode *tn=items[i].node;
            DomNode *sty=tn->parent?tn->parent:n;
            int sc=scale_for(sty->fsize);
            const char *t=tn->text; uint32_t tl=tn->text_len;
            uint32_t k=0;
            while(k<tl){
                while(k<tl&&(t[k]==' '||t[k]=='\t'||t[k]=='\n'||t[k]=='\r'))k++;
                if(k>=tl)break;
                uint32_t ws=k;
                while(k<tl&&t[k]!=' '&&t[k]!='\t'&&t[k]!='\n'&&t[k]!='\r')k++;
                uint32_t wl=min_u32(k-ws,4095);
                int wpw=text_width_utf8(t+ws,min_u32(wl,4095),sc);
                if(lx!=sub.x && lx+wpw>sub.x+sub.w){
                    sub.cursor_y+=lh; lx=sub.x; lh=sc*GLYPH_H+2;
                } else if(lh<sc*GLYPH_H+2) lh=sc*GLYPH_H+2;
                run_new(sty,t+ws,wl,lx,sub.cursor_y,sc);
                lx+=wpw;
                if(k<tl){
                    int spw=GLYPH_W*sc/2;
                    if(spw<2)spw=2;
                    if(lx+spw>sub.x+sub.w){ sub.cursor_y+=lh; lx=sub.x; }
                    else lx+=spw;
                }
            }
        }
        /* alinhamento center: centraliza cada linha formada nesta sequencia */
        if(n->align==1){
            uint32_t new_cnt=g_nruns-line_start;
            Run *np=g_runs;
            int done_y[512]; int dn=0;
            for(uint32_t i=0;i<new_cnt&&np;i++,np=np->next){
                int y0=np->y;
                int already=0;
                for(int k=0;k<dn;k++)if(done_y[k]==y0){already=1;break;}
                if(already)continue;
                if(dn<512)done_y[dn++]=y0;
                int mnx=INT32_MAX,mxx=INT32_MIN;
                for(Run *q=g_runs;q;q=q->next){
                    if(q->y==y0){
                        int wq=text_width_utf8(q->text,q->scale);
                        if(q->x<mnx)mnx=q->x;
                        if(q->x+wq>mxx)mxx=q->x+wq;
                    }
                }
                int shift=(sub.w-(mxx-mnx))/2;
                if(shift<0)shift=0;
                int delta=sub.x+shift-mnx;
                if(delta!=0)
                    for(Run *q=g_runs;q;q=q->next)
                        if(q->y==y0) q->x+=delta;
            }
        }
        sub.cursor_y+=lh;
        free(items);
    }
    n->w=w;
    int bottom=sub.cursor_y;
    int pb=(n->p_b!=CSS_UNSET&&n->p_b>0)?n->p_b:0;
    n->h=bottom-block_top+pb;
    f->cursor_y=bottom+pb+mb;
}



void page_layout(Page *pg, int width){
    runs_clear();
    pg->content_w=width;
    if(!pg->root){pg->content_h=0;return;}
    FlowCtx f={ .x=0, .w=width, .cursor_y=0 };
    /* body tem margin padrao 8px aplicada em layout_node; root deve ser html-like:
       usamos wrapper: trata root(body) diretamente */
    layout_node(pg->root,&f);
    /* se root eh inline-only (documento sem body), wrap manual */
    if(pg->root->disp!=DISP_BLOCK){
        f.cursor_y=0;
        /* trata como bloco forcado */
        pg->root->disp=DISP_BLOCK;
        layout_node(pg->root,&f);
    }
    pg->content_h=f.cursor_y+16;
    pg->dirty=1;
}

/* ===================== Paint ======================================== */
static void paint_bg_and_border(Bitmap *b, DomNode *n){
    /* fundo do bloco cobre padding box */
    if(n->bg_valid){
        int x=n->x, y=n->y;
        int w=n->w + (n->p_l!=CSS_UNSET?n->p_l:0)+(n->p_r!=CSS_UNSET?n->p_r:0);
        bmp_fill(b,x,y,w,n->h,n->bg);
    }
    if(!strcmp(n->name,"hr")){
        bmp_fill(b,n->x,n->y+n->h/2-1,n->w,2,0x808080);
    }
    if(!strcmp(n->name,"li")){
        /* marcador */
        DomNode *ul=n->parent;
        while(ul&&strcmp(ul->name,"ul")&&strcmp(ul->name,"ol"))ul=ul->parent;
        if(ul&&!strcmp(ul->name,"ul")){
            int sc=scale_for(n->fsize);
            bmp_fill(b,n->x-18,n->y+sc*3,sc*2,sc*2,0x000000);
        } else if(ul){
            /* ol: numero baseado em indice */
            int idx=1;
            for(uint32_t i=0;i<ul->n_children;i++) if(ul->children[i]==n){idx=i+1;break;}
            char num[8]; snprintf(num,sizeof num,"%d.",idx);
            int sc=scale_for(n->fsize);
            bmp_draw_text(b,n->x-30,n->y,num,0x000000,sc);
        }
    }
}

void page_render(Page *pg){
    Bitmap *b=&pg->surf;
    /* redimensiona superficie se preciso */
    int need_w=pg->viewport_w;
    int need_h=max_i(pg->content_h+4,pg->viewport_h);
    if(need_h>16384)need_h=16384;
    if(b->px && (b->w!=need_w||b->h!=need_h)){
        free(b->px); b->px=NULL;
    }
    if(!b->px){
        b->px=malloc((size_t)need_w*need_h*4);
        b->w=need_w;b->h=need_h;b->stride=need_w;
    }
    /* fundo branco */
    bmp_fill(b,0,0,b->w,b->h,0xFFFFFF);
    if(!pg->root){
        return;
    }
    /* fundos/bordas em ordem de documento (DFS pre-order sobre doc list) */
    for(uint32_t i=0;i<pg->doc.n;i++){
        DomNode *n=pg->doc.nodes[i];
        if(n->type!=NODE_ELEMENT||n->disp==DISP_NONE)continue;
        if(n->h<=0||n->w<=0)continue;
        paint_bg_and_border(b,n);
    }
    /* imagens */
    for(uint32_t i=0;i<pg->doc.n;i++){
        DomNode *n=pg->doc.nodes[i];
        if(n->is_image&&n->img&&n->w>0){
            bmp_draw_image(b,n->x,n->y,n->w,n->h,n->img);
        } else if(n->is_image&&!n->img&&n->w>0){
            /* placeholder */
            bmp_fill(b,n->x,n->y,n->w,n->h,0xE8E8E8);
            bmp_hline(b,n->x,n->x+n->w-1,n->y,0xB0B0B0);
            bmp_hline(b,n->x,n->x+n->w-1,n->y+n->h-1,0xB0B0B0);
            char ph[64]; snprintf(ph,sizeof ph,"[img %dx%d]",n->w,n->h);
            bmp_draw_text(b,n->x+4,n->y+n->h/2-4,ph,0x909090,1);
        }
    }
    /* texto (runs); g_runs esta em ordem inversa — ordem nao importa p/ texto */
    for(Run *r=runs_head();r;r=r->next){
        bmp_draw_text(b,r->x,r->y,r->text,r->color,r->scale);
        if(r->underline){
            int wpx=text_width_utf8(r->text,r->scale);
            bmp_hline(b,r->x,r->x+wpx-1,r->y+r->scale*7,r->color);
        }
        if(r->strikethrough){
            int wpx=text_width_utf8(r->text,r->scale);
            bmp_hline(b,r->x,r->x+wpx-1,r->y+r->scale*4,0xFF0000);
        }
    }
}

/* hit-test de link: ponto na pagina (coords absolutas) */
DomNode *page_hit_link(Page *pg,int px,int py){
    (void)pg;
    for(Run *r=runs_head();r;r=r->next){
        if(px>=r->x&&px<r->x+text_width_utf8(r->text,r->scale)&&py>=r->y&&py<r->y+r->scale*GLYPH_H+2){
            DomNode *p=r->owner;
            while(p){ if(p->type==NODE_ELEMENT&&p->has_link)return p; p=p->parent; }
        }
    }
    for(uint32_t i=0;i<pg->doc.n;i++){
        DomNode *n=pg->doc.nodes[i];
        if(n->type!=NODE_ELEMENT||!n->has_link)continue;
        if((n->is_image||(n->w>0&&n->h>0))&&px>=n->x&&px<n->x+n->w&&py>=n->y&&py<n->y+n->h)
            return n;
    }
    return NULL;
}

void page_push_history(Page *pg,const char *url){
    if(pg->hist_n&&pg->hist[pg->hist_pos]&&!strcmp(pg->hist[pg->hist_pos],url))return;
    /* corta forward */
    while(pg->hist_n>pg->hist_pos+1){ free(pg->hist[--pg->hist_n]); }
    if(pg->hist_n==pg->hist_cap){
        pg->hist_cap=pg->hist_cap?pg->hist_cap*2:16;
        pg->hist=realloc(pg->hist,pg->hist_cap*sizeof(char*));
    }
    pg->hist[pg->hist_n++]=strdup(url);
    pg->hist_pos=pg->hist_n-1;
}
const char *page_back(Page *pg){
    if(pg->hist_pos>0) return pg->hist[--pg->hist_pos];
    return NULL;
}
const char *page_forward(Page *pg){
    if(pg->hist_pos<pg->hist_n-1) return pg->hist[++pg->hist_pos];
    return NULL;
}

void page_free(Page *pg){
    runs_clear();
    for(uint32_t i=0;i<pg->doc.n;i++){
        if(pg->doc.nodes[i]->img) image_free(pg->doc.nodes[i]->img);
    }
    arena_destroy(&pg->arena);
    css_free(&pg->css);
    free(pg->doc.nodes);
    free(pg->surf.px);
    for(int i=0;i<pg->hist_n;i++)free(pg->hist[i]);
    free(pg->hist);
    memset(pg,0,sizeof *pg);
}
