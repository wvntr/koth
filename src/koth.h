/* koth.h - Koth Browser Engine (KHronos Optimization & Traversal Engine)
 * =====================================================================
 * Projeto 100% do zero: parser HTML, CSS, layout, rede HTTP/1.1 + TLS,
 * cache de conexoes, rasterizador e UI X11. Nenhum WebView/Blink/Gecko.
 */
#ifndef KOTH_H
#define KOTH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>

/* ------------------------------------------------------------------ */
/* Util: buffers de string com crescimento geometrico                 */
/* ------------------------------------------------------------------ */
typedef struct {
    char  *p;
    uint32_t len;
    uint32_t cap;
} Str;

void   str_init(Str *s);
void   str_ensure(Str *s, uint32_t n);
void   str_free(Str *s);
void   str_append(Str *s, const char *p, uint32_t n);
void   str_appc(Str *s, char c);
void   str_apps(Str *s, const char *lit);
void   str_appf(Str *s, const char *fmt, ...);
char  *str_take(Str *s);            /* desanexa buffer (chamador libera) */

static inline uint32_t min_u32(uint32_t a, uint32_t b){ return a<b?a:b; }
static inline uint32_t max_u32(uint32_t a, uint32_t b){ return a>b?a:b; }
static inline int clamp_i(int v,int lo,int hi){ return v<lo?lo:(v>hi?hi:v); }
static inline int max_i(int a,int b){ return a>b?a:b; }
static inline int min_i(int a,int b){ return a<b?a:b; }
double now_sec(void);

/* hash 32-bit estavel para chaves pequenas */
uint32_t fnv1a(const char *s, uint32_t n);

/* decode UTF-8 a partir de codepoint; avanca *i; retorna U+FFFD se invalido */
uint32_t utf8_next(const char *s, uint32_t n, uint32_t *i);
/* append codepoint como UTF-8 */
void utf8_put(Str *out, uint32_t cp);

/* ------------------------------------------------------------------ */
/* Pool de arena (alocacao bump; liberada em bloco)                    */
/* ------------------------------------------------------------------ */
typedef struct ArenaBlock {
    struct ArenaBlock *next;
    uint32_t cap, off;
    char data[];
} ArenaBlock;

typedef struct {
    ArenaBlock *head;
    uint32_t block_size;
} Arena;

void  arena_init(Arena *a, uint32_t block_size);
void *arena_alloc(Arena *a, uint32_t n);
char *arena_strdup(Arena *a, const char *s, uint32_t n);
void  arena_destroy(Arena *a);

/* ------------------------------------------------------------------ */
/* DOM                                                                  */
/* ------------------------------------------------------------------ */
typedef enum { NODE_ELEMENT, NODE_TEXT } NodeType;

typedef struct Image Image;
typedef struct DomNode {
    NodeType type;
    const char *name;          /* internada */
    const char **attr_keys;
    const char **attr_vals;
    uint16_t n_attr;
    struct DomNode **children;
    uint32_t n_children, cap_children;
    struct DomNode *parent;
    /* texto (NODE_TEXT): ponteiro no pool da pagina */
    const char *text; uint32_t text_len;
    /* estilo resolvido */
    uint16_t disp : 2;         /* 0=inline 1=block 2=none */
    uint16_t has_link : 1;     /* <a href> */
    uint16_t is_image : 1;
    char link_url[512];        /* href absoluto resolvido */
    char src_url[512];         /* src de imagem absoluto */
    Image *img;                /* imagem decodificada (worker) */
    /* caixa calculada pelo layout */
    int32_t x, y, w, h;      /* caixa do elemento */
    int32_t tx, ty, tw, th;  /* caixa de texto (content box p/ texto) */
    /* estilo computado */
    int16_t fsize;
    uint8_t bold, italic, underline;
    uint32_t color, bg;
    uint8_t bg_valid;
    int16_t m_t,m_b,m_l,m_r, p_t,p_b,p_l,p_r;
    int8_t align;              /* 0 left 1 center */
    int16_t line_h;
    uint8_t has_fsize,has_color,has_bold,has_ital,has_ul,has_align,has_lh;
    uint8_t visited_link, strikethrough, preformatted, is_hr;
} DomNode;

#define DOM_MAX_CHILDREN_CAP 4096
#define DISP_INLINE 0
#define DISP_BLOCK  1
#define DISP_NONE   2

typedef struct {
    DomNode **nodes;      /* todos os nos, ordem de criacao */
    uint32_t n, cap;
} DomDoc;

/* interning de nomes de tags/atributos */
const char *intern_name(const char *s, uint32_t n);

/* entity decode basico (&amp; &lt; &gt; &quot; &#NN; &#xHH;) */
void decode_entities(const char *s, uint32_t n, Str *out);

DomNode *dom_parse_html(Arena *ar, DomDoc *doc, const char *html, uint32_t len);
void dom_dump(DomNode *root, int depth);
const char *dom_attr(DomNode *n, const char *key);

/* ------------------------------------------------------------------ */
/* CSS                                                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *sel_tag;       /* NULL = universal */
    const char *sel_class;     /* NULL ausente */
    const char *sel_id;        /* NULL ausente */
    uint8_t specificity;
    /* declaracoes; sentinelas abaixo indicam "nao definido" */
    int16_t font_size;         /* px; 0 = nao definido */
    int8_t  font_weight;       /* 0 auto, 1 bold */
    uint8_t color_set;
    uint32_t color;            /* 0xRRGGBB */
    uint8_t bg_set;
    uint32_t bg;
    int16_t margin_t, margin_b, margin_l, margin_r; /* -9999 = nao definido */
    int16_t pad_t, pad_b, pad_l, pad_r;
    int8_t  display;           /* -1 nd, 0 inline, 1 block, 2 none */
    int8_t  text_align;        /* -1, 0 left, 1 center */
    int16_t line_height;       /* 0 = auto */
    int8_t  italic;
    int8_t  underline;
} CssRule;

#define CSS_UNSET  (-9999)

typedef struct {
    CssRule *rules;
    uint32_t n, cap;
} StyleSheet;

void css_init(StyleSheet *ss);
void css_parse(StyleSheet *ss, const char *css, uint32_t len);
void css_free(StyleSheet *ss);

/* aplica regras ao no (usa defaults herdados de `inherit`) */
void style_apply_rule(CssRule *r, DomNode *n);
int  style_node_matches(CssRule *r, DomNode *n);
int  css_attr_match(DomNode *n, const char *spec);
void style_resolve_tree(DomNode *root, StyleSheet *ss);

/* ------------------------------------------------------------------ */
/* Rede: URL, DNS cache, conexao pool, HTTP/1.1, TLS                   */
/* ------------------------------------------------------------------ */
typedef struct {
    char scheme[8];
    char host[256];
    uint16_t port;
    char path[2048];      /* path + query */
    int secure;
} KUrl;

int  url_parse(KUrl *u, const char *raw);
void url_resolve(KUrl *base, const char *rel, KUrl *out);
void url_full(const KUrl *u, Str *out); /* scheme://host[:port]/path */

typedef struct {
    char  *body;
    uint32_t body_len;
    char  *content_type;
    char  *charset;
    char  final_url[1024];
    int status;
    int ok;
    char err[256];
    double t_dns, t_conn, t_tls, t_first, t_total;
    int reused_conn;
    int from_cache;
} HttpResponse;

void net_init(void);
int  http_get(HttpResponse *r, const char *url_str);
void http_response_free(HttpResponse *r);
/* cache de conteudo em memoria */
int  net_cache_lookup(const char *url, char **data, uint32_t *len, char **ct, char **cs);
void net_cache_store(const char *url, const char *data, uint32_t len, const char *ct, const char *cs, int max_age);

/* gzip inflate minimo */
int gunzip_buf(const uint8_t *in, uint32_t inlen, Str *out);

/* ------------------------------------------------------------------ */
/* Imagens (PNG/JPEG/GIF -> RGBA)                                       */
/* ------------------------------------------------------------------ */
struct Image {
    uint8_t *rgba;
    int w, h;
    int loaded;
};

Image *image_decode(const uint8_t *data, uint32_t len);
void   image_free(Image *im);

/* ------------------------------------------------------------------ */
/* Fonte bitmap embutida 8x8                                            */
/* ------------------------------------------------------------------ */
extern const uint8_t FONT8X8[96][8];
#define GLYPH_W 8
#define GLYPH_H 8
int text_width_utf8(const char *s, int scale); /* largura em px de string utf8 */
const uint8_t *glyph_for_cp(uint32_t cp);

/* ------------------------------------------------------------------ */
/* Rasterizador                                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t *px;      /* XRGB */
    int w, h, stride;
} Bitmap;

void bmp_fill(Bitmap *b, int x,int y,int w,int h, uint32_t c);
void bmp_hline(Bitmap *b,int x0,int x1,int y,uint32_t c);
void bmp_draw_glyph(Bitmap *b,int x,int y,const uint8_t rows[8],uint32_t c,int scale);
void bmp_draw_text(Bitmap *b,int x,int y,const char *s,uint32_t c,int scale);
void bmp_draw_image(Bitmap *b,int x,int y,int w,int h,const Image *im);

/* ------------------------------------------------------------------ */
/* Engine pipeline                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    char title[512];
    DomNode *root;
    DomDoc doc;
    Arena arena;
    StyleSheet css;
    Bitmap surf;             /* superficie de renderizacao (conteudo inteiro) */
    int content_w, content_h;
    int viewport_w, viewport_h;
    int scroll_y;
    /* estado */
    char current_url[1024];
    int error;
    char errmsg[256];
    double load_time;
    /* historico */
    char **hist; int hist_n, hist_cap, hist_pos;
    /* contagem de imagens pendentes */
    volatile int pending_imgs;
    volatile int dirty;      /* precisa rerender */
    volatile int quit_requested;
} Page;

void page_init(Page *pg);
int  page_load(Page *pg, const char *url);   /* bloqueante */
void page_layout(Page *pg, int width);
void page_render(Page *pg);
void page_free(Page *pg);
void page_push_history(Page *pg, const char *url);
const char *page_back(Page *pg);
const char *page_forward(Page *pg);
DomNode *page_hit_link(Page *pg, int vx, int vy); /* node <a> sob ponto da viewport */

void net_tick(void);
void charset_to_utf8(const char *in, uint32_t len, const char *cs, Str *out);
const char *dom_attr(DomNode *n, const char *key);

/* busca */
void search_url_for(const char *q, Str *out);

/* runs de texto (layout) */
typedef struct Run {
    DomNode *owner;
    char *text;
    int x,y,scale;
    uint32_t color;
    uint8_t underline, strikethrough, centered;
    struct Run *next;
} Run;
void  runs_clear(void);
Run  *runs_head(void);
void  img_workers_start(void);
int   imgs_pending(void);
extern volatile int g_page_dirty_flag;
int   url_parse_strict(const char *raw);

#endif /* KOTH_H */
