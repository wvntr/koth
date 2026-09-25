/* dom.c - Tokenizador e parser HTML (modo de recuperacao, como navegadores reais) */
#include "koth.h"
#include <ctype.h>

/* --------------------- Interning de nomes -------------------------- */
#define INTERN_BUCKETS 1024
static struct { char *s; uint32_t len; } g_intern[INTERN_BUCKETS];
static pthread_mutex_t g_intern_mtx = PTHREAD_MUTEX_INITIALIZER;

const char *intern_name(const char *s, uint32_t n){
    /* minusculas */
    char low[64];
    if(n>=sizeof low) n = sizeof(low)-1;
    for(uint32_t i=0;i<n;i++) low[i] = (s[i]>='A'&&s[i]<='Z')? s[i]+32 : s[i];
    low[n]=0;
    uint32_t h = fnv1a(low,n) & (INTERN_BUCKETS-1);
    pthread_mutex_lock(&g_intern_mtx);
    for(int probe=0; probe<8; probe++){
        uint32_t idx = (h+probe)&(INTERN_BUCKETS-1);
        typeof(g_intern[0]) *e = &g_intern[idx];
        if(!e->s){
            char *dup = malloc(n+1); memcpy(dup,low,n); dup[n]=0;
            e->s=dup; e->len=n;
            pthread_mutex_unlock(&g_intern_mtx);
            return dup;
        }
        if(e->len==n && !memcmp(e->s,low,n)){
            pthread_mutex_unlock(&g_intern_mtx);
            return e->s;
        }
    }
    pthread_mutex_unlock(&g_intern_mtx);
    char *dup = malloc(n+1); memcpy(dup,low,n); dup[n]=0;
    return dup;
}

/* --------------------- Entidades ----------------------------------- */
void decode_entities(const char *s, uint32_t n, Str *out){
    uint32_t i=0;
    while(i<n){
        char c=s[i];
        if(c=='&' && i+1<n){
            /* procura ; */
            uint32_t j=i+1, lim=min_u32(i+10,n);
            while(j<lim && s[j]!=';') j++;
            if(j<lim && s[j]==';'){
                uint32_t elen=j-i-1;
                const char *ent=s+i+1;
                int handled=0;
                if(elen>=2 && ent[0]=='#'){
                    uint32_t cp=0;
                    if(ent[1]=='x'||ent[1]=='X'){
                        for(uint32_t k=2;k<elen;k++){
                            char ch=ent[k]; int v;
                            if(ch>='0'&&ch<='9') v=ch-'0';
                            else if(ch>='a'&&ch<='f') v=ch-'a'+10;
                            else if(ch>='A'&&ch<='F') v=ch-'A'+10;
                            else { v=-1; break; }
                            cp=cp*16+v;
                        }
                    } else {
                        for(uint32_t k=1;k<elen;k++)
                            if(ent[k]>='0'&&ent[k]<='9') cp=cp*10+(ent[k]-'0');
                    }
                    if(cp && cp<0x110000){ utf8_put(out,cp); handled=1; }
                }
                /* tabela simples */
                const char *rp=NULL; uint32_t rl=0;
                if(!handled){
                    if(elen==3&&!memcmp(ent,"amp",3)){rp="&";rl=1;}
                    else if(elen==2&&!memcmp(ent,"lt",2)){rp="<";rl=1;}
                    else if(elen==2&&!memcmp(ent,"gt",2)){rp=">";rl=1;}
                    else if(elen==4&&!memcmp(ent,"quot",4)){rp="\"";rl=1;}
                    else if(elen==6&&!memcmp(ent,"apos",6)){rp="'";rl=1;}
                    else if(elen==5&&!memcmp(ent,"nbsp",5)){rp="\xC2\xA0";rl=2;}
                    else if(elen==7&&!memcmp(ent,"copy;",7)){rp="(c)";rl=3;}
                    if(rp){ str_append(out,rp,rl); handled=1; }
                }
                #undef ENT
                if(handled){ i=j+1; continue; }
            }
        }
        str_appc(out,c); i++;
    }
}

/* --------------------- Helpers de nodes ---------------------------- */
static DomNode *new_node(Arena *ar, DomDoc *doc, NodeType t){
    DomNode *n = arena_alloc(ar,sizeof(DomNode));
    memset(n,0,sizeof *n);
    n->type=t;
    n->disp=DISP_INLINE;
    n->fsize=16; n->color=0x000000;
    n->m_t=n->m_b=n->m_l=n->m_r=n->p_t=n->p_b=n->p_l=n->p_r=CSS_UNSET;
    n->line_h=0;
    if(doc){
        if(doc->n==doc->cap){
            doc->cap = doc->cap? doc->cap*2 : 256;
            doc->nodes = realloc(doc->nodes, doc->cap*sizeof(DomNode*));
        }
        doc->nodes[doc->n++]=n;
    }
    return n;
}
static void add_child(DomNode *parent, DomNode *child){
    child->parent=parent;
    if(parent->n_children==parent->cap_children){
        parent->cap_children = parent->cap_children? parent->cap_children*2 : 4;
        if(parent->cap_children>DOM_MAX_CHILDREN_CAP) parent->cap_children=DOM_MAX_CHILDREN_CAP;
        parent->children = realloc(parent->children, parent->cap_children*sizeof(DomNode*));
    }
    if(parent->n_children < parent->cap_children)
        parent->children[parent->n_children++]=child;
}
static void set_attr(Arena *ar, DomNode *n, const char *k,uint32_t kl, const char *v,uint32_t vl){
    /* substitui se existir */
    for(uint16_t i=0;i<n->n_attr;i++)
        if(!strcmp(n->attr_keys[i],k)){ /* assume interned key */
            char *buf=arena_alloc(ar,vl+1); memcpy(buf,v,vl); buf[vl]=0;
            n->attr_vals[i]=buf; return;
        }
    if(n->n_attr>=64) return;
    if(!n->attr_keys){
        n->attr_keys=arena_alloc(ar,64*sizeof(void*));
        n->attr_vals=arena_alloc(ar,64*sizeof(void*));
    }
    n->attr_keys[n->n_attr]=intern_name(k,kl);
    char *buf=arena_alloc(ar,vl+1); memcpy(buf,v,vl); buf[vl]=0;
    n->attr_vals[n->n_attr]=buf;
    n->n_attr++;
}
const char *dom_attr(DomNode *n, const char *key){
    for(uint16_t i=0;i<n->n_attr;i++)
        if(n->attr_keys[i] && !strcmp(n->attr_keys[i],key)) return n->attr_vals[i];
    return NULL;
}

/* --------------------- Tags ---------------------------------------- */
static int is_void_tag(const char *t){
    static const char *v[]={"area","base","br","col","embed","hr","img","input",
        "link","meta","param","source","track","wbr",0};
    for(int i=0;v[i];i++) if(!strcmp(t,v[i])) return 1;
    return 0;
}
static int is_rawtext_tag(const char *t){
    return !strcmp(t,"script")||!strcmp(t,"style")||!strcmp(t,"textarea");
}
static int auto_closes(const char *open, const char *closing){
    /* <p> fechado por blocos; li fecha li; etc. */
    static const char *blockish[]={"address","article","aside","blockquote","details",
        "div","dl","fieldset","figcaption","figure","footer","form","h1","h2","h3",
        "h4","h5","h6","header","hr","main","nav","ol","p","section","table","ul",0};
    if(!strcmp(open,"p")){
        for(int i=0;blockish[i];i++) if(!strcmp(blockish[i],closing)) return 1;
    }
    if(!strcmp(open,"li") && !strcmp(closing,"li")) return 1;
    if(!strcmp(open,"option") && !strcmp(closing,"option")) return 1;
    if(!strcmp(open,"dt") && (!strcmp(closing,"dt")||!strcmp(closing,"dd"))) return 1;
    if(!strcmp(open,"dd") && (!strcmp(closing,"dt")||!strcmp(closing,"dd"))) return 1;
    if(!strcmp(open,"tr") && (!strcmp(closing,"tr"))) return 1;
    if(!strcmp(open,"td") && (!strcmp(closing,"td")||!strcmp(closing,"th")||!strcmp(closing,"tr"))) return 1;
    if(!strcmp(open,"th") && (!strcmp(closing,"td")||!strcmp(closing,"th")||!strcmp(closing,"tr"))) return 1;
    if((!strcmp(open,"thead")||!strcmp(open,"tbody")) && !strcmp(closing,"tbody")) return 1;
    return 0;
}

/* display default por tag */
static int default_display(const char *t){
    static const char *blocks[]={"address","article","aside","blockquote","body","center",
        "dd","details","div","dl","dt","fieldset","figcaption","figure","footer","form",
        "h1","h2","h3","h4","h5","h6","header","hr","html","li","main","nav","ol","p",
        "pre","section","summary","table","tbody","td","tfoot","th","thead","tr","ul",0};
    for(int i=0;blocks[i];i++) if(!strcmp(blocks[i],t)) return DISP_BLOCK;
    static const char *nones[]={"head","script","style","title","meta","link","noscript",
        "template","svg","canvas","iframe","video","audio","object","map","option",0};
    for(int i=0;nones[i];i++) if(!strcmp(nones[i],t)) return DISP_NONE;
    return DISP_INLINE;
}

/* --------------------- Parser -------------------------------------- */
/* stack para open elements */
typedef struct { DomNode **st; int n, cap; } NodeStack;
static void st_push(NodeStack*s,DomNode*d){
    if(s->n==s->cap){ s->cap=s->cap?s->cap*2:64; s->st=realloc(s->st,s->cap*sizeof(void*)); }
    s->st[s->n++]=d;
}
static DomNode* st_peek(NodeStack*s){ return s->n? s->st[s->n-1]:NULL; }
static void st_pop(NodeStack*s){ if(s->n) s->n--; }
static int st_find(NodeStack*s,const char*name,int upto){
    for(int i=s->n-1;i>=upto;i--)
        if(s->st[i]->type==NODE_ELEMENT && !strcmp(s->st[i]->name,name)) return i;
    return -1;
}

DomNode *dom_parse_html(Arena *ar, DomDoc *doc, const char *html, uint32_t len){
    DomNode *html_el = new_node(ar,doc,NODE_ELEMENT);
    html_el->name=intern_name("html",4);
    html_el->disp=DISP_BLOCK;
    NodeStack stack={0};
    st_push(&stack, html_el);
    DomNode *body = NULL;

    uint32_t i=0;
    Str txtacc; str_init(&txtacc);

    while(i<len){
        char c=html[i];
        if(c=='<'){
            /* comentario / doctype / CDATA */
            if(i+1<len && html[i+1]=='!'){
                if(i+3<len && !memcmp(html+i+2,"--",2)){
                    uint32_t e=i+4;
                    while(e+2<len && memcmp(html+e,"-->",3)) e++;
                    i = (e+2<len)? e+3 : len;
                    continue;
                } else {
                    uint32_t e=i+2;
                    while(e<len && html[e]!='>') e++;
                    i=(e<len)? e+1 : len;
                    continue;
                }
            }
            if(i+1<len && (html[i+1]=='?' )){ /* PI */
                uint32_t e=i; while(e<len && html[e]!='>') e++;
                i=(e<len)?e+1:len; continue;
            }
            if(i+1<len && html[i+1]=='/'){
                /* close tag */
                uint32_t j=i+2; uint32_t ns=j;
                while(j<len && (isalnum((unsigned char)html[j])||html[j]=='-'||html[j]=='_'||html[j]==':')) j++;
                const char *nm=intern_name(html+ns,j-ns);
                uint32_t e=j; while(e<len && html[e]!='>') e++;
                i=(e<len)?e+1:len;
                /* encontra na stack */
                int idx=st_find(&stack,nm,1);
                if(idx>=1){
                    /* implicitamente fecha acima */
                    while(stack.n-1 > idx) st_pop(&stack);
                    st_pop(&stack);
                }
                continue;
            }
            if(i+1<len && (isalpha((unsigned char)html[i+1])||html[i+1]=='$')){
                /* open tag */
                uint32_t j=i+1, ns=j;
                while(j<len && (isalnum((unsigned char)html[j])||html[j]=='-'||html[j]=='_'||html[j]==':')) j++;
                const char *nm=intern_name(html+ns,j-ns);
                /* atributos */
                DomNode *el=new_node(ar,doc,NODE_ELEMENT);
                el->name=nm;
                el->disp=default_display(nm);
                int selfclose=0;
                while(j<len){
                    while(j<len && (html[j]==' '||html[j]=='\t'||html[j]=='\n'||html[j]=='\r'||html[j]=='/')){
                        if(html[j]=='/' && j+1<len && html[j+1]=='>'){ selfclose=1; }
                        j++;
                    }
                    if(j>=len) break;
                    if(html[j]=='>'){ j++; break; }
                    /* nome attr */
                    uint32_t as=j;
                    while(j<len && !(html[j]=='='||html[j]=='>'||html[j]==' '||html[j]=='\t'||html[j]=='\n'||html[j]=='\r'||html[j]=='/')) j++;
                    if(j>as){
                        uint32_t vs=j, ve=j;
                        /* skip spaces */
                        uint32_t k=j;
                        while(k<len && (html[k]==' '||html[k]=='\t')) k++;
                        if(k<len && html[k]=='='){
                            k++;
                            while(k<len && (html[k]==' '||html[k]=='\t')) k++;
                            if(k<len && (html[k]=='"'||html[k]=='\'')){
                                char q=html[k]; uint32_t s0=++k;
                                while(k<len && html[k]!=q) k++;
                                vs=s0; ve=k;
                                if(k<len) k++;
                            } else {
                                uint32_t s0=k;
                                while(k<len && !(html[k]=='>'||html[k]==' '||html[k]=='\t'||html[k]=='\n'||html[k]=='\r')) k++;
                                vs=s0; ve=k;
                            }
                            j=k;
                        } else { vs=ve=j; }
                        /* lowercased key via intern */
                        char keybuf[64]; uint32_t kl=j-as;
                        if(kl>=sizeof keybuf) kl=sizeof(keybuf)-1;
                        for(uint32_t x=0;x<kl;x++) keybuf[x]=(html[as+x]>='A'&&html[as+x]<='Z')?html[as+x]+32:html[as+x];
                        /* valor com entities decoded in-place lower? keep raw */
                        set_attr(ar, el, keybuf, kl, html+vs, ve-vs);
                    }
                }
                i=j;
                /* auto-close regras */
                while(stack.n>1 && auto_closes(st_peek(&stack)->name, nm)) st_pop(&stack);
                /* <html>/<head>/<body> aninhamento */
                if(!strcmp(nm,"body")||!strcmp(nm,"html")||!strcmp(nm,"head")){
                    int idx=st_find(&stack,nm,0);
                    if(idx>=0){
                        while(stack.n-1>idx) st_pop(&stack);
                        if(!strcmp(nm,"html")){
                            /* reabre html: nao empilha de novo nem adiciona */
                            continue;
                        }
                        st_pop(&stack);
                    }
                }
                add_child(st_peek(&stack), el);
                if(!body && !strcmp(nm,"body")) body=el;
                if(!selfclose && !is_void_tag(nm)){
                    st_push(&stack, el);
                    if(is_rawtext_tag(nm)){
                        /* consome ate </tag */
                        uint32_t s0=i;
                        char endtag[64];
                        snprintf(endtag,sizeof endtag,"</%s",nm);
                        uint32_t ell=(uint32_t)strlen(endtag);
                        while(i+ell<len){
                            if(!strncasecmp(html+i,endtag,ell)){ break; }
                            i++;
                        }
                        uint32_t rawlen=i-s0;
                        if(!strcmp(nm,"style")){
                            /* style text sera coletado depois pelo walker */
                            DomNode *t=new_node(ar,doc,NODE_TEXT);
                            t->text=arena_strdup(ar,html+s0,rawlen); t->text_len=rawlen;
                            add_child(el,t);
                        } else if(!strcmp(nm,"script")){
                            /* scripts: ignora conteudo mas mantem no como marker */
                            el->disp=DISP_NONE;
                        } else { /* textarea */
                            Str d; str_init(&d);
                            decode_entities(html+s0,rawlen,&d);
                            DomNode *t=new_node(ar,doc,NODE_TEXT);
                            t->text=arena_strdup(ar,d.p?d.p:"",d.len); t->text_len=d.len;
                            str_free(&d);
                            add_child(el,t);
                        }
                        /* pula ate '>' de </tag> */
                        while(i<len && html[i]!='>') i++;
                        if(i<len) i++;
                        st_pop(&stack);
                        continue;
                    }
                }
                continue;
            }
            /* '<' solto -> texto */
            DomNode *t=new_node(ar,doc,NODE_TEXT);
            t->text=arena_strdup(ar,"<",1); t->text_len=1;
            add_child(st_peek(&stack),t);
            i++;
            continue;
        }
        /* texto */
        uint32_t s0=i;
        while(i<len && html[i]!='<') i++;
        uint32_t tl=i-s0;
        /* colapsa whitespace-only? mantemos; layout cuida */
        int allspace=1;
        for(uint32_t x=s0;x<s0+tl;x++)
            if(html[x]!=' '&&html[x]!='\t'&&html[x]!='\n'&&html[x]!='\r'){ allspace=0; break; }
        if(allspace && tl>0){
            /* representa como espaco unico */
            DomNode *t=new_node(ar,doc,NODE_TEXT);
            t->text=arena_strdup(ar," ",1); t->text_len=1;
            add_child(st_peek(&stack),t);
        } else if(tl>0){
            Str d; str_init(&d);
            decode_entities(html+s0,tl,&d);
            DomNode *t=new_node(ar,doc,NODE_TEXT);
            t->text=arena_strdup(ar,d.p?d.p:"",d.len); t->text_len=d.len;
            str_free(&d);
            add_child(st_peek(&stack),t);
        }
    }
    free(stack.st);
    (void)txtacc;
    if(!body){
        /* sintese: trata html_el como body */
        body=html_el;
    }
    /* retorna o node de renderizacao: body se existir senao html */
    /* garante que children de head nao aparecem: body ja separado */
    return body;
}

void dom_dump(DomNode *n, int depth){
    if(!n) return;
    for(int i=0;i<depth;i++) fprintf(stderr,"  ");
    if(n->type==NODE_ELEMENT)
        fprintf(stderr,"<%s> attrs=%u children=%u\n", n->name, n->n_attr, n->n_children);
    else {
        char tmp[60]; uint32_t m=min_u32(n->text_len,50);
        memcpy(tmp,n->text,m); tmp[m]=0;
        fprintf(stderr,"#text \"%s\"\n",tmp);
    }
    for(uint32_t i=0;i<n->n_children;i++) dom_dump(n->children[i],depth+1);
}
