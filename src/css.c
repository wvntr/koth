/* css.c - Parser CSS proprio: seletores simples, declaracoes, cores, unidades */
#include "koth.h"
#include <ctype.h>

void css_init(StyleSheet *ss){ ss->rules=NULL; ss->n=0; ss->cap=0; }
void css_free(StyleSheet *ss){ free(ss->rules); ss->rules=NULL; ss->n=ss->cap=0; }

static CssRule *new_rule(StyleSheet *ss){
    if(ss->n==ss->cap){
        ss->cap = ss->cap? ss->cap*2 : 64;
        ss->rules = realloc(ss->rules, ss->cap*sizeof(CssRule));
    }
    CssRule *r=&ss->rules[ss->n++];
    memset(r,0,sizeof *r);
    r->font_size=0; r->margin_t=r->margin_b=r->margin_l=r->margin_r=CSS_UNSET;
    r->pad_t=r->pad_b=r->pad_l=r->pad_r=CSS_UNSET;
    r->display=-1; r->text_align=-1; r->italic=-1; r->underline=-1;
    return r;
}

static const char *skip_ws(const char *p,const char *end){
    while(p<end && isspace((unsigned char)*p)) p++;
    /* comentarios */
    if(p+1<end && p[0]=='/'&&p[1]=='*'){
        p+=2; while(p+1<end && !(p[0]=='*'&&p[1]=='/')) p++; 
        if(p+1<end) p+=2;
        return skip_ws(p,end);
    }
    return p;
}

static int parse_px(const char *p,const char *end,int *out){
    /* aceita Npx, N, N%, Nrem, Nem -> px aproximado */
    char *ep; double v=strtod(p,&ep);
    if(ep==p) return 0;
    const char *u=ep;
    while(u<end && isalpha((unsigned char)*u)) u++;
    int ulen=u-ep;
    if(ulen>=1 && (!memcmp(ep,"px",2)||!memcmp(ep,"P",1))) ;
    else if(ulen>=3 && !memcmp(ep,"rem",3)) v*=16;
    else if(ulen>=2 && !memcmp(ep,"em",2)) v*=16;
    else if(ulen>=2 && !memcmp(ep,"pt",2)) v=v*96.0/72.0;
    else if(ulen>=1 && !memcmp(ep,"%",1)) v=v; /* % tratado no caller */
    *out=(int)(v+0.5);
    return (int)(u-p);
}

typedef struct { uint8_t r,g,b; } RGB;
static int named_color(const char *s,uint32_t n,RGB *o){
    struct { const char *nm; uint32_t c; } tbl[]={
        {"black",0x000000},{"white",0xFFFFFF},{"red",0xFF0000},{"green",0x008000},
        {"blue",0x0000FF},{"navy",0x000080},{"teal",0x008080},{"olive",0x808000},
        {"yellow",0xFFFF00},{"purple",0x800080},{"fuchsia",0xFF00FF},{"silver",0xC0C0C0},
        {"gray",0x808080},{"grey",0x808080},{"maroon",0x800000},{"orange",0xFFA500},
        {"aqua",0x00FFFF},{"lime",0x00FF00},{"pink",0xFFC0CB},{"cyan",0x00FFFF},
        {"magenta",0xFF00FF},{"brown",0xA52A2A},{"gold",0xFFD700},{"indigo",0x4B0082},
        {"violet",0xEE82EE},{"turquoise",0x40E0D0},{"beige",0xF5F5DC},{"ivory",0xFFFFF0},
        {"salmon",0xFA8072},{"khaki",0xF0E68C},{"crimson",0xDC143C},{"tomato",0xFF6347},
        {"coral",0xFF7F50},{"orchid",0xDA70D6},{"plum",0xDDA0DD},{"tan",0xD2B48C},
        {"chocolate",0xD2691E},{"sienna",0xA0522D},{"peru",0xCD853F},{"darkgray",0xA9A9A9},
        {"darkgrey",0xA9A9A9},{"lightgray",0xD3D3D3},{"lightgrey",0xD3D3D3},
        {"dimgray",0x696969},{"dimgrey",0x696969},{"slategray",0x708090},
        {"lightslategray",0x778899},{"steelblue",0x4682B4},{"royalblue",0x4169E1},
        {"dodgerblue",0x1E90FF},{"deepskyblue",0x00BFFF},{"skyblue",0x87CEEB},
        {"lightblue",0xADD8E6},{"powderblue",0xB0E0E6},{"cadetblue",0x5F9EA0},
        {"mediumblue",0x0000CD},{"darkblue",0x00008B},{"midnightblue",0x191970},
        {"forestgreen",0x228B22},{"seagreen",0x2E8B57},{"darkgreen",0x006400},
        {"olivedrab",0x6B8E23},{"lawngreen",0x7CFC00},{"limegreen",0x32CD32},
        {"darkred",0x8B0000},{"firebrick",0xB22222},{"darkorange",0xFF8C00},
        {"orangered",0xFF4500},{"goldenrod",0xDAA520},{"cornflowerblue",0x6495ED},
        {"rebeccapurple",0x663399},{"whitesmoke",0xF5F5F5},{"aliceblue",0xF0F8FF},
        {"lavender",0xE6E6FA},{"honeydew",0xF0FFF0},{"ghostwhite",0xF8F8FF},
        {"gainsboro",0xDCDCDC},{"antiquewhite",0xFAEBD7},{"wheat",0xF5DEB3},
        {"linen",0xFAF0E6},{"peachpuff",0xFFDAB9},{"mistyrose",0xFFE4E1},
        {"snow",0xFFFAFA},{"seashell",0xFFF5EE},{"bisque",0xFFE4C4},
        {"transparent",0xFFFFFF},
    };
    char low[32]; if(n>=sizeof low) return 0;
    for(uint32_t i=0;i<n;i++) low[i]=tolower((unsigned char)s[i]);
    low[n]=0;
    for(unsigned i=0;i<sizeof(tbl)/sizeof(tbl[0]);i++)
        if(!strcmp(tbl[i].nm,low)){
            o->r=tbl[i].c>>16; o->g=tbl[i].c>>8; o->b=tbl[i].c;
            return 1;
        }
    return 0;
}

static int parse_color(const char *p,const char *end,uint32_t *out){
    while(p<end&&isspace((unsigned char)*p))p++;
    if(p>=end) return 0;
    if(*p=='#'){
        p++;
        uint32_t v=0; int digits=0;
        while(p<end && digits<6 && isxdigit((unsigned char)*p)){
            char c=*p++;
            int d=(c<='9')?c-'0':(tolower(c)-'a'+10);
            v=v*16+d; digits++;
        }
        if(digits==3) v=((v&0xF00)<<16)|(((v>>4)&0xF)<<16)|((v&0xF)<<16), /* expandir */
                        v=( (v>>16)&0xF )*0x110000 | (((v>>8)&0xF))*0x1100 | ((v&0xF))*0x11;
        else if(digits==6){}
        else return 0;
        *out=v; return 1;
    }
    if(!strncmp(p,"rgb",3)){
        p+=3; if(p<end&&*p=='(')p++;
        int comps[3]={0,0,0}; int ci=0;
        while(p<end && ci<3){
            while(p<end&&(isspace((unsigned char)*p)||*p==','||*p=='/'))p++;
            char *ep; double v=strtod(p,&ep);
            if(ep==p) break;
            p=ep;
            if(p<end&&*p=='%'){ v=v*255/100; p++; }
            comps[ci++]=(int)v;
        }
        *out=(comps[0]<<16)|(comps[1]<<8)|comps[2];
        return 1;
    }
    /* nome */
    const char *s=p;
    while(p<end && isalpha((unsigned char)*p)) p++;
    RGB rgb;
    if(named_color(s,p-s,&rgb)){ *out=(rgb.r<<16)|(rgb.g<<8)|rgb.b; return 1; }
    return 0;
}

static void parse_decls(CssRule *r,const char *p,const char *end){
    while(p<end){
        while(p<end && (isspace((unsigned char)*p)||*p==';')) p++;
        if(p>=end) break;
        const char *ks=p;
        while(p<end && *p!=':' && *p!=';') p++;
        if(p>=end || *p!=':') break;
        uint32_t kl=p-ks;
        char key[40]; if(kl>=sizeof key) kl=sizeof(key)-1;
        for(uint32_t i=0;i<kl;i++){ key[i]=(ks[i]>='A'&&ks[i]<='Z')?ks[i]+32:ks[i]; }
        key[kl]=0;
        /* trim trailing space in key */
        while(kl>0 && isspace((unsigned char)key[kl-1])) key[--kl]=0;
        p++;
        const char *vs=p;
        while(p<end && *p!=';' && *p!='}') p++;
        const char *ve=p;
        while(ve>vs && isspace((unsigned char)ve[-1])) ve--;
        const char *v=vs; uint32_t vl=ve-vs;
        /* ignora !important apenas remove sufixo */
        if(!strcmp(key,"color")){ parse_color(v,ve,&r->color); r->color_set=1; }
        else if(!strcmp(key,"background-color")){ if(parse_color(v,ve,&r->bg)) r->bg_set=1; }
        else if(!strcmp(key,"background")){
            /* aceita cor simples ou 'none' */
            if(vl>=4 && !memcmp(v,"none",4)){ r->bg=0xFFFFFF; r->bg_set=0; }
            else {
                /* procura #hex ou nome apos espacos */
                const char *q=v;
                while(q<ve && (*q==' ')) q++;
                if(q<ve && *q=='#'){ if(parse_color(q,ve,&r->bg)) r->bg_set=1; }
                else { RGB rgb; if(named_color(q,ve-q,&rgb)){ r->bg=(rgb.r<<16)|(rgb.g<<8)|rgb.b; r->bg_set=1; } }
            }
        }
        else if(!strcmp(key,"font-size")){
            int px;
            if(vl==4&&!memcmp(v,"large",5-1)) px=18;
            else if(vl==6&&!memcmp(v,"medium",6)) px=16;
            else if(vl==3&&!memcmp(v,"small",5)&&vl==5) px=13;
            else if(vl==5&&!memcmp(v,"small",5)) px=13;
            else if(vl==4&&!memcmp(v,"large",5)&&vl==4) px=18;
            else if(vl==6&&!memcmp(v,"x-large",7)&&vl==6) px=24;
            else if(vl==7&&!memcmp(v,"x-large",7)) px=24;
            else if(vl==7&&!memcmp(v,"xx-large",8)&&vl==7) px=32;
            else if(vl==8&&!memcmp(v,"xx-large",8)) px=32;
            else if(vl==5&&!memcmp(v,"x-small",7)&&vl==5) px=10;
            else if(vl==4&&!memcmp(v,"larg",4)) px=18;
            else {
                int conv=parse_px(v,ve,&px);
                /* percentual relativo a 16 */
                const char *u=v; while(u<ve&&isdigit((unsigned char)*u))u++;
                if(u<ve&&*u=='%'){ char *ep; double val=strtod(v,&ep); px=(int)(val*16/100+0.5); }
                if(!conv && v!=ve && !isdigit((unsigned char)*v) && *v!='.') px=0;
            }
            if(px>0&&px<200) r->font_size=px;
        }
        else if(!strcmp(key,"font-weight")){
            if(vl>=4&&!memcmp(v,"bold",4)) r->font_weight=1;
            else if(vl>=6&&!memcmp(v,"normal",6)) r->font_weight=0;
            else { char *ep; long n=strtol(v,&ep,10); if(ep!=v && n>=600) r->font_weight=1; }
        }
        else if(!strcmp(key,"font-style")){
            r->italic = (!vl)?-1:((vl==7&&!memcmp(v,"italic",7))?1:0);
        }
        else if(!strcmp(key,"text-decoration")){
            r->underline = (vl>=9 && !memcmp(v,"underline",9))?1:0;
        }
        else if(!strcmp(key,"display")){
            if(vl==4&&!memcmp(v,"none",4)) r->display=DISP_NONE;
            else if(vl==5&&!memcmp(v,"block",5)) r->display=DISP_BLOCK;
            else if(vl==6&&!memcmp(v,"inline",6)) r->display=DISP_INLINE;
            else if(vl>=10&&(!memcmp(v,"inline-block",12)||!memcmp(v,"flex",4)||!memcmp(v,"grid",4)))
                r->display=DISP_BLOCK;
            else if(vl==4&&!memcmp(v,"flex",4)) r->display=DISP_BLOCK;
            else if(vl==4&&!memcmp(v,"grid",4)) r->display=DISP_BLOCK;
        }
        else if(!strcmp(key,"text-align")){
            r->text_align = (vl==6&&!memcmp(v,"center",6))?1:0;
        }
        else if(!strcmp(key,"line-height")){
            int px; char *ep; double dv=strtod(v,&ep);
            if(ep!=v){
                const char *u=ep; while(u<ve&&isalpha((unsigned char)*u))u++;
                if(u>ep && !memcmp(ep,"px",2)) px=(int)dv;
                else if(dv>0&&dv<4) px=(int)(dv*r->font_size+0.5); /* multiplicador */
                else px=(int)dv;
                if(px>0&&px<200) r->line_height=px;
            } else if(vl==3&&!memcmp(v,"1.5",3)) r->line_height=0;
        }
        else if(!strcmp(key,"margin")||!strcmp(key,"padding")){
            int is_m = key[0]=='m';
            int vals[4]={CSS_UNSET,CSS_UNSET,CSS_UNSET,CSS_UNSET}, nv=0;
            const char *q=v;
            while(q<ve&&nv<4){
                while(q<ve&&isspace((unsigned char)*q))q++;
                if(q>=ve)break;
                int px;
                if(q<ve && *q=='a' && ve-q>=3 && !memcmp(q,"auto",4)){ px=0; q+=4; /* auto ~ 0 */ }
                else {
                    int adv=parse_px(q,ve,&px);
                    if(!adv){ /* percent: ~ do container; guardamos negativo especial? simplifica: 0 */
                        char *ep; double dv=strtod(q,&ep);
                        if(ep==q){ break; }
                        const char *u2=ep; while(u2<ve&&isalpha((unsigned char)*u2))u2++;
                        if(u2<ve&&*u2=='%'){ px=0; q=u2+1; }
                        else break;
                    } else q+=adv;
                    /* % relativo: approximation 0 */
                    const char *chk=q; (void)chk;
                }
                if(px>-2000&&px<2000) vals[nv++]=px;
                else vals[nv++]=0;
            }
            int t,b,l,rr;
            if(nv==1){t=b=l=rr=vals[0];}
            else if(nv==2){t=b=vals[0];l=rr=vals[1];}
            else if(nv==3){t=vals[0];l=rr=vals[1];b=vals[2];}
            else {t=vals[0];r->margin_t=t;b=vals[1];l=vals[2];rr=vals[3];t=vals[0];}
            if(nv>=4){t=vals[0];rr=vals[1];b=vals[2];l=vals[3];}
            if(is_m){ r->margin_t=t;r->margin_b=b;r->margin_l=l;r->margin_r=rr; }
            else   { r->pad_t=t;r->pad_b=b;r->pad_l=l;r->pad_r=rr; }
        }
        else if(!strcmp(key,"margin-top")||!strcmp(key,"margin-bottom")||!strcmp(key,"margin-left")||!strcmp(key,"margin-right")
             || !strcmp(key,"padding-top")||!strcmp(key,"padding-bottom")||!strcmp(key,"padding-left")||!strcmp(key,"padding-right")){
            int px=0; int adv=parse_px(v,ve,&px);
            if(!adv){ char *ep; double dv=strtod(v,&ep); if(ep==v) continue; px=(int)dv; }
            if(px<-2000||px>2000) px=0;
            int is_m=key[0]=='m';
            const char *side=key+strlen(key)-4; /* top /ottom... use suffix match */
            if(!strcmp(side,"top")||!strcmp(key+(strlen(key)-3),"top")){ if(is_m)r->margin_t=px;else r->pad_t=px; }
            else if(!strcmp(key+(strlen(key)-6),"bottom")){ if(is_m)r->margin_b=px;else r->pad_b=px; }
            else if(!strcmp(key+(strlen(key)-5),"left")){ if(is_m)r->margin_l=px;else r->pad_l=px; }
            else if(!strcmp(key+(strlen(key)-5),"right")){ if(is_m)r->margin_r=px;else r->pad_r=px; }
        }
    }
}

static void parse_selector(StyleSheet *ss,const char *p,const char *end){
    /* suporta lista separada por ',' */
    while(p<end){
        const char *s=p;
        while(p<end && *p!=',') p++;
        const char *e=p;
        if(p<end) p++;
        while(s<e&&isspace((unsigned char)*s))s++;
        while(e>s&&isspace((unsigned char)e[-1]))e--;
        if(s>=e) continue;
        /* pega o ultimo segmento composto (ex: ".a .b" -> "b") */
        const char *last=s;
        for(const char *q=s;q<e;q++)
            if(*q==' '||*q=='>'||*q=='+') { if(q+1<e) last=q+1; }
        while(last<e&&isspace((unsigned char)*last))last++;
        CssRule *r=new_rule(ss);
        r->specificity=1;
        const char *q=last;
        while(q<e){
            if(*q=='#'){
                const char *t=++q;
                while(q<e && (isalnum((unsigned char)*q)||*q=='-'||*q=='_')) q++;
                r->sel_id=intern_name(t,q-t);
                r->specificity+=100;
            } else if(*q=='.'){
                const char *t=++q;
                while(q<e && (isalnum((unsigned char)*q)||*q=='-'||*q=='_')) q++;
                r->sel_class=intern_name(t,q-t);
                r->specificity+=10;
            } else if(*q=='['){
                /* atributo: pula */
                while(q<e && *q!=']') q++;
                if(q<e) q++;
            } else if(*q==':' ){
                /* pseudo: guarda :hover etc ignorar exceto ::first-line nada */
                q++;
                while(q<e && (isalnum((unsigned char)*q)||*q=='-'||*q=='(' )) q++;
                if(q<e&&*q==')')q++;
            } else if(isalpha((unsigned char)*q)||*q=='*'){
                if(*q=='*'){ q++; }
                else {
                    const char *t=q;
                    while(q<e && (isalnum((unsigned char)*q)||*q=='-'||*q=='_')) q++;
                    r->sel_tag=intern_name(t,q-t);
                    r->specificity+=1;
                }
            } else q++;
        }
    }
}


/* ------------------ parsing de blocos com multi-seletores ------------------ */
typedef struct {
    const char *tag; const char *cls; const char *id;
} SelKey;

static void selector_to_keys(const char *s,const char *e,Arena *ar,StyleSheet *ss,CssRule *proto){
    /* um seletor -> uma regra (ultimo segmento) */
    while(s<e&&isspace((unsigned char)*s))s++;
    while(e>s&&isspace((unsigned char)e[-1]))e--;
    if(s>=e) return;
    const char *last=s;
    for(const char *q=s;q<e;q++)
        if(*q==' '||*q=='>'||*q=='+'||*q=='~'){ if(q+1<e){ last=q+1; while(last<e&&isspace((unsigned char)*last))last++; } }
    CssRule *r=new_rule(ss);
    *r=*proto;
    r->specificity=1;
    const char *q=last;
    while(q<e){
        if(*q=='#'){
            const char *t=++q;
            while(q<e&&(isalnum((unsigned char)*q)||*q=='-'||*q=='_'))q++;
            r->sel_id=intern_name(t,q-t); r->specificity+=100;
        } else if(*q=='.'){
            const char *t=++q;
            while(q<e&&(isalnum((unsigned char)*q)||*q=='-'||*q=='_'))q++;
            r->sel_class=intern_name(t,q-t); r->specificity+=10;
        } else if(*q=='['){
            /* atributo: [attr], [attr=val], [attr^=val], [attr$=val], [attr*=val] */
            q++;
            const char *as=q;
            while(q<e&&*q!=']'&&*q!='='&&*q!='^'&&*q!='$'&&*q!='*'&&*q!='|')q++;
            const char *ae=q;
            char op=0; const char *vstart=NULL;
            if(q<e&&*q!=']'){ op=*q; q++; if(q<e&&*q=='=')q++; vstart=q; }
            while(q<e&&*q!=']')q++;
            uint32_t alen=ae-as;
            char key[48]; if(alen>=sizeof key)alen=sizeof(key)-1;
            for(uint32_t i2=0;i2<alen;i2++)key[i2]=(as[i2]>='A'&&as[i2]<='Z')?as[i2]+32:as[i2];
            key[alen]=0;
            const char *val=NULL;
            if(vstart){
                const char *ve2=q;
                while(ve2>vstart&&isspace((unsigned char)ve2[-1]))ve2--;
                if(vstart<ve2&&(*vstart=='"'||*vstart=='\'')){vstart++;while(ve2>vstart&&ve2[-1]!='"'&&ve2[-1]!='\'')ve2--;}
                uint32_t vlen=ve2-vstart;
                val=arena_strdup(ar,vstart,vlen);
            }
            /* armazena como pseudo-attribute selector via sel_class prefixo '@' */
            Str tmp; str_init(&tmp);
            str_appc(&tmp,'@'); str_append(&tmp,key,strlen(key));
            if(op&&val){ str_appc(&tmp,op); str_append(&tmp,val,strlen(val)); }
            r->sel_class=intern_name(tmp.p,tmp.len);
            str_free(&tmp);
            r->specificity+=10;
        } else if(*q==':'){
            q++;
            if(q<e&&*q==':')q++;
            const char *t=q;
            while(q<e&&(isalnum((unsigned char)*q)||*q=='-'||*q=='_'||*q=='('))q++;
            /* :not(.x) - ignora; ::first-line ignora */
            (void)t;
            while(q<e&&*q!=')'&&*q!='.'&&*q!='#'&&*q!='[')q++;
        } else if(isalpha((unsigned char)*q)||*q=='*'||*q=='%'||isdigit((unsigned char)*q)){
            if(*q=='*'||*q=='%'){q++;}
            else{
                const char *t=q;
                while(q<e&&(isalnum((unsigned char)*q)||*q=='-'||*q=='_'))q++;
                r->sel_tag=intern_name(t,q-t); r->specificity+=1;
            }
        } else q++;
    }
}

void css_parse(StyleSheet *ss, const char *css, uint32_t len){
    static Arena ar={0};
    if(!ar.head) arena_init(&ar,65536);
    const char *p=css,*end=css+len;
    while(p<end){
        p=skip_ws(p,end);
        if(p>=end) break;
        if(*p=='@'){
            while(p<end && *p!='{' && *p!=';') p++;
            if(p<end && *p==';'){ p++; continue; }
            if(p<end && *p=='{'){
                int depth=1; p++;
                while(p<end&&depth){ if(*p=='{')depth++; else if(*p=='}')depth--; p++; }
                continue;
            }
            break;
        }
        const char *s=p;
        while(p<end && *p!='{') p++;
        if(p>=end) break;
        const char *se=p;
        const char *bs=++p;
        int depth=1;
        while(p<end&&depth){ if(*p=='{')depth++; else if(*p=='}')depth--; if(depth)p++; }
        const char *be=p;
        /* proto rule com decls */
        CssRule proto; memset(&proto,0,sizeof proto);
        proto.margin_t=proto.margin_b=proto.margin_l=proto.margin_r=CSS_UNSET;
        proto.pad_t=proto.pad_b=proto.pad_l=proto.pad_r=CSS_UNSET;
        proto.display=-1; proto.text_align=-1; proto.italic=-1; proto.underline=-1;
        parse_decls(&proto,bs,be);
        /* lista de seletores */
        const char *q=s;
        while(q<se){
            const char *c=q;
            while(q<se&&*q!=',')q++;
            selector_to_keys(c,q,&ar,ss,&proto);
            if(q<se)q++;
        }
    }
}

/* ------------------ matching ------------------ */
int css_attr_match(DomNode *n, const char *spec){
    /* spec: "@name", "@name=value", "@name^=value", "@name$=value", "@name*=value" */
    if(spec[0]!='@')return 0;
    const char *p=spec+1;
    const char *nm=p;
    while(*p&&*p!='='&&*p!='^'&&*p!='$'&&*p!='*'&&*p!='|')p++;
    char key[48]; uint32_t kl=p-nm;
    if(kl>=sizeof key)return 0;
    memcpy(key,nm,kl);key[kl]=0;
    char op=0; const char *val=NULL;
    if(*p){
        op=*p; p++;
        if(*p=='=')p++;
        val=p;
    }
    const char *av=dom_attr(n,key);
    if(!av)return 0;
    if(!op)return 1;
    if(!strcmp(key,"href")||!strcmp(key,"src")) av=dom_attr(n,key);
    size_t anl=strlen(av), vl=strlen(val);
    switch(op){
    case '=': return anl==vl&&!memcmp(av,val,vl);
    case '^': return anl>=vl&&vl&&!!memcmp(av,val,vl)==0 && !strncmp(av,val,vl);
    case '$': return anl>=vl&&vl&&!strcmp(av+anl-vl,val);
    case '*': return vl&&strstr(av,val)!=NULL;
    case '|': return !strcmp(av,val)||(!strncmp(av,val,vl)&&av[vl]=='-');
    case '~': {
        const char *tok=av;
        while(*tok){
            while(*tok==' ')tok++;
            const char *e2=tok; while(*e2&&*e2!=' ')e2++;
            if((size_t)(e2-tok)==vl&&!memcmp(tok,val,vl))return 1;
            tok=e2;
        }
        return 0;
    }}
    return 0;
}

static int node_has_class(DomNode *n,const char *cls){
    const char *c=dom_attr(n,"class");
    if(!c) return 0;
    size_t lc=strlen(cls);
    const char *tok=c;
    while(*tok){
        while(*tok==' '||*tok=='\t'||*tok=='\n')tok++;
        const char *e=tok;
        while(*e&&*e!=' '&&*e!='\t'&&*e!='\n')e++;
        if((size_t)(e-tok)==lc&&!memcmp(tok,cls,e-tok)) return 1;
        tok=e;
    }
    return 0;
}

int style_node_matches(CssRule *r, DomNode *n){
    if(n->type!=NODE_ELEMENT) return 0;
    if(r->sel_tag && strcmp(r->sel_tag,n->name)) return 0;
    if(r->sel_id){
        const char *id=dom_attr(n,"id");
        if(!id||strcmp(id,r->sel_id)) return 0;
    }
    if(r->sel_class){
        if(r->sel_class[0]=='@'){
            if(!css_attr_match(n,r->sel_class)) return 0;
        } else if(!node_has_class(n,r->sel_class)) return 0;
    }
    return 1;
}

/* resolve estilo: duas passadas com flags para heranca correta */
static uint8_t rule_matches_and_apply(CssRule *r, DomNode *n){
    if(!style_node_matches(r,n)) return 0;
    if(r->font_size){ n->fsize=r->font_size; n->has_fsize=1; }
    if(r->font_weight){ n->bold=1; n->has_bold=1; }
    if(r->color_set){ n->color=r->color; n->has_color=1; }
    if(r->bg_set){ n->bg=r->bg; n->bg_valid=1; }
    if(r->display>=0) n->disp=r->display;
    if(r->text_align>=0){ n->align=r->text_align; n->has_align=1; }
    if(r->line_height){ n->line_h=r->line_height; n->has_lh=1; }
    if(r->italic>=0){ n->italic=r->italic; n->has_ital=1; }
    if(r->underline>=0){ n->underline=r->underline; n->has_ul=1; }
    if(r->margin_t!=CSS_UNSET)n->m_t=r->margin_t;
    if(r->margin_b!=CSS_UNSET)n->m_b=r->margin_b;
    if(r->margin_l!=CSS_UNSET)n->m_l=r->margin_l;
    if(r->margin_r!=CSS_UNSET)n->m_r=r->margin_r;
    if(r->pad_t!=CSS_UNSET)n->p_t=r->pad_t;
    if(r->pad_b!=CSS_UNSET)n->p_b=r->pad_b;
    if(r->pad_l!=CSS_UNSET)n->p_l=r->pad_l;
    if(r->pad_r!=CSS_UNSET)n->p_r=r->pad_r;
    return 1;
}

static int cmp_rules_qsort(const void *a,const void *b){
    const CssRule *ra=a, *rb=b;
    return (int)ra->specificity-(int)rb->specificity;
}

void style_resolve_tree(DomNode *root, StyleSheet *ss){
    if(ss->n>1) qsort(ss->rules,ss->n,sizeof(CssRule),cmp_rules_qsort);
    /* passada 1: aplica CSS author em ordem de especificidade crescente */
    for(uint32_t i=0;i<ss->n;i++){
        CssRule *r=&ss->rules[i];
        /* walk */
        DomNode **stack=malloc(sizeof(void*)*64); int sn=0,cap=64;
        stack[sn++]=root;
        while(sn){
            DomNode *n=stack[--sn];
            if(n->type==NODE_ELEMENT) rule_matches_and_apply(r,n);
            if(n->n_children){
                if(sn+(int)n->n_children>cap){ cap=(sn+n->n_children)*2; stack=realloc(stack,cap*sizeof(void*)); }
                for(uint32_t c=0;c<n->n_children;c++) stack[sn++]=n->children[c];
            }
        }
        free(stack);
    }
    /* passada 2: heranca + defaults UA (ordem documento via doc list nao disponivel aqui; DFS pre-order) */
    DomNode **stack=malloc(sizeof(void*)*64); int sn=0,cap=64;
    stack[sn++]=root;
    while(sn){
        DomNode *n=stack[--sn];
        if(n->type==NODE_ELEMENT){
            DomNode *p=n->parent;
            while(p&&p->type!=NODE_ELEMENT)p=p->parent;
            if(p){
                if(!n->has_fsize) n->fsize=p->fsize;
                if(!n->has_color) n->color=p->color;
                if(!n->has_bold) n->bold=p->bold;
                if(!n->has_ital) n->italic=p->italic;
                if(!n->has_align) n->align=p->align;
                if(!n->has_lh) n->line_h=p->line_h;
            }
            const char *nm=n->name;
            if(!strcmp(nm,"a")){
                if(!n->has_color&&!n->visited_link) n->color=0x0000EE;
                else if(!n->has_color) n->color=0x551A8B;
                if(!n->has_ul) n->underline=1;
            }
            if(!n->has_fsize){
                if(!strcmp(nm,"h1"))n->fsize=32;
                else if(!strcmp(nm,"h2"))n->fsize=24;
                else if(!strcmp(nm,"h3"))n->fsize=19;
                else if(!strcmp(nm,"h4"))n->fsize=16;
                else if(!strcmp(nm,"h5"))n->fsize=13;
                else if(!strcmp(nm,"h6"))n->fsize=11;
            }
            if((!strcmp(nm,"b")||!strcmp(nm,"strong"))&&!n->has_bold)n->bold=1;
            if((!strcmp(nm,"i")||!strcmp(nm,"em"))&&!n->has_ital)n->italic=1;
            if(!strcmp(nm,"u")&&!n->has_ul)n->underline=1;
            if(!strcmp(nm,"s")||!strcmp(nm,"strike")||!strcmp(nm,"del"))n->strikethrough=1;
            if(!strcmp(nm,"small")&&!n->has_fsize)n->fsize=(int)(n->fsize*0.8);
            if(!strcmp(nm,"big")&&!n->has_fsize)n->fsize=(int)(n->fsize*1.2);
            if(!strcmp(nm,"center")&&!n->has_align)n->align=1;
            if(!strcmp(nm,"mark")&&!n->bg_valid){n->bg=0xFFF176;n->bg_valid=1;}
            if(!strcmp(nm,"blockquote")){ if(!n->has_align){} n->m_l=40; n->m_r=40; }
            if(!strcmp(nm,"pre")){ n->preformatted=1; }
            if(!strcmp(nm,"hr")){ n->is_hr=1; }
            if(!strcmp(nm,"body")){ if(!n->bg_valid){n->bg=0xFFFFFF;n->bg_valid=1;} n->color=0x000000; }
        }
        if(n->n_children){
            if(sn+(int)n->n_children>cap){ cap=(sn+n->n_children)*2; stack=realloc(stack,cap*sizeof(void*)); }
            for(uint32_t c=0;c<n->n_children;c++) stack[sn++]=n->children[c];
        }
    }
    free(stack);
}
