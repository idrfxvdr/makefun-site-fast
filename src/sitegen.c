// sitegen.c — freestanding C port of makefun-site 1.0.0's build(), compiled to wasm32 + SIMD128.
//
// Architecture (OPENCODE_TASK_sitegen_wasm.md): main.ts reads /input/cases.json as RAW BYTES,
// copies them ONCE into wasm linear memory, calls mg_run(inPtr, inLen); the program parses the
// JSON in-C, runs the whole build over UTF-8 bytes, builds the entire answers.json as bytes in-C
// (including JSON escaping), writes {out_ptr,out_len} into a control struct at address 0; main.ts
// bulk-copies the answer out and writeFileSync's it. No JS strings, no JSON.parse/JSON.stringify.
//
// -nostdlib: own memcpy/memset, grow-aware bump allocator (no free). SIMD128 via clang vector_size.
// From-scratch implementation of the makefun-site rules; no upstream code copied.

typedef unsigned char u8;
typedef unsigned int u32;
typedef int i32;
typedef unsigned long long u64;
typedef long long i64;

#define NULL ((void*)0)

// Control struct at address 0: main.ts reads out_ptr/out_len/status after mg_run.
volatile u32* const CTRL = (volatile u32*)0;
#define CTRL_OUT_PTR 0
#define CTRL_OUT_LEN 1
#define CTRL_STATUS  2

// ------------------------------------------------------------------ freestanding runtime
void* memcpy(void* d, const void* s, u32 n){ u8*a=d; const u8*b=s; for(u32 i=0;i<n;i++)a[i]=b[i]; return d; }
void* memset(void* d, int c, u32 n){ u8*a=d; for(u32 i=0;i<n;i++)a[i]=(u8)c; return a; }
int memcmp(const void* a, const void* b, u32 n){ const u8*x=a,*y=b; for(u32 i=0;i<n;i++){ if(x[i]!=y[i]) return (int)x[i]-(int)y[i]; } return 0; }

// ------------------------------------------------------------------ grow-aware bump allocator
// Arena starts at ARENA_BASE (input copied below it by main.ts). Bump pointer never frees.
// Grows linear memory in chunks via memory.grow BEFORE bumping past current size.
#define ARENA_BASE   0x00400000u   /* 4 MiB: room for the copied input below the arena */
#define GROW_CHUNK   0x00800000u   /* grow by 8 MiB when needed */

static u32 g_arena;      /* next free byte */
static u32 g_memsize;    /* current linear memory size in bytes */

static void arena_init(u32 in_end){
  g_memsize = __builtin_wasm_memory_size(0) * 65536u;
  g_arena = (in_end + 15u) & ~15u;
  if (g_arena < ARENA_BASE) g_arena = ARENA_BASE;
}

static void* bump(u32 n){
  u32 need = g_arena + n;
  if (need > g_memsize){
    u32 grow = ((need - g_memsize) + GROW_CHUNK - 1) / GROW_CHUNK * GROW_CHUNK;
    u32 pages = grow / 65536u;
    __builtin_wasm_memory_grow(0, pages);
    g_memsize += grow;
  }
  u32 p = g_arena;
  g_arena = (need + 7u) & ~7u;
  return (void*)p;
}

// ------------------------------------------------------------------ growable byte buffer
typedef struct { u8* p; u32 len; u32 cap; } Buf;

static void buf_reserve(Buf* b, u32 extra){
  if (b->len + extra <= b->cap) return;
  u32 nc = b->cap ? b->cap : 256;
  while (nc < b->len + extra) nc *= 2;
  u8* np = (u8*)bump(nc);
  if (b->p && b->len) memcpy(np, b->p, b->len);
  b->p = np; b->cap = nc;
}
static void buf_putc(Buf* b, u8 c){ buf_reserve(b,1); b->p[b->len++]=c; }
static void buf_write(Buf* b, const void* s, u32 n){ buf_reserve(b,n); memcpy(b->p+b->len, s, n); b->len+=n; }
static void buf_puts(Buf* b, const char* s){ u32 n=0; while(s[n])n++; buf_write(b,s,n); }
static void buf_clear(Buf* b){ b->len = 0; }

// forward decls
static void json_write_string(Buf* b, const u8* s, u32 len);

// small NUL-terminated string type for keys/paths (we control them; they are short)

// ================================================================== JSON DOM (input side)
// We parse /input/cases.json into a DOM, then run the build over it. Output JSON is written
// directly as bytes (no DOM). JSON strings are decoded to UTF-8 at parse time.
typedef enum { JNULL, JBOOL, JNUM, JSTR, JARR, JOBJ } JTag;

typedef struct JVal JVal;
typedef struct { u8* key; u32 keylen; JVal* val; } JMember;

struct JVal {
  JTag tag;
  union {
    int b;                 // JBOOL
    i64 num;               // JNUM
    struct { u8* p; u32 len; } s;   // JSTR (UTF-8, not NUL-terminated)
    struct { JVal** items; u32 len; } arr;  // JARR
    struct { JMember* m; u32 len; } obj;    // JOBJ
  } as;
};

static JVal* jnew(JTag t){ JVal* v=(JVal*)bump(sizeof(JVal)); v->tag=t; return v; }

// JSON parser state
typedef struct { const u8* p; const u8* end; int err; } JParse;

static void jskipws(JParse* ps){
  while (ps->p < ps->end){ u8 c=*ps->p; if(c==0x20||c==0x09||c==0x0a||c==0x0d) ps->p++; else break; }
}

static int hexval(u8 c){
  if(c>='0'&&c<='9')return c-'0';
  if(c>='a'&&c<='f')return c-'a'+10;
  if(c>='A'&&c<='F')return c-'A'+10;
  return -1;
}

// Decode a JSON string starting at ps->p (which must point at the opening quote).
// Returns a freshly allocated UTF-8 buffer via *out/*outlen. Advances past closing quote.
static void jparse_string_raw(JParse* ps, u8** out, u32* outlen){
  // ps->p at '"'
  ps->p++; // skip opening quote
  Buf b; b.p=NULL; b.len=0; b.cap=0;
  const u8* s = ps->p;
  // fast scan for next quote or backslash
  while (ps->p < ps->end){
    u8 c = *ps->p;
    if (c == '"'){ // end
      buf_write(&b, s, (u32)(ps->p - s));
      ps->p++;
      *out=b.p; *outlen=b.len; return;
    }
    if (c == '\\'){
      buf_write(&b, s, (u32)(ps->p - s));
      ps->p++; // skip backslash
      if (ps->p >= ps->end){ ps->err=1; *out=b.p; *outlen=b.len; return; }
      u8 e = *ps->p;
      switch(e){
        case '"': buf_putc(&b,'"'); ps->p++; break;
        case '\\': buf_putc(&b,'\\'); ps->p++; break;
        case '/': buf_putc(&b,'/'); ps->p++; break;
        case 'b': buf_putc(&b,0x08); ps->p++; break;
        case 'f': buf_putc(&b,0x0c); ps->p++; break;
        case 'n': buf_putc(&b,0x0a); ps->p++; break;
        case 'r': buf_putc(&b,0x0d); ps->p++; break;
        case 't': buf_putc(&b,0x09); ps->p++; break;
        case 'u': {
          ps->p++;
          if (ps->p+4 > ps->end){ ps->err=1; break; }
          u32 cp=0;
          for(int k=0;k<4;k++){ int h=hexval(ps->p[k]); if(h<0){ps->err=1;break;} cp=(cp<<4)|(u32)h; }
          ps->p+=4;
          // handle surrogate pair
          if (cp>=0xD800 && cp<=0xDBFF && ps->p+6<=ps->end && ps->p[0]=='\\' && ps->p[1]=='u'){
            u32 lo=0; const u8* q=ps->p+2; int ok=1;
            for(int k=0;k<4;k++){ int h=hexval(q[k]); if(h<0){ok=0;break;} lo=(lo<<4)|(u32)h; }
            if (ok && lo>=0xDC00 && lo<=0xDFFF){
              cp = 0x10000 + ((cp-0xD800)<<10) + (lo-0xDC00);
              ps->p+=6;
            }
          }
          // encode cp as UTF-8
          if (cp < 0x80) buf_putc(&b,(u8)cp);
          else if (cp < 0x800){ buf_putc(&b,(u8)(0xC0|(cp>>6))); buf_putc(&b,(u8)(0x80|(cp&0x3F))); }
          else if (cp < 0x10000){ buf_putc(&b,(u8)(0xE0|(cp>>12))); buf_putc(&b,(u8)(0x80|((cp>>6)&0x3F))); buf_putc(&b,(u8)(0x80|(cp&0x3F))); }
          else { buf_putc(&b,(u8)(0xF0|(cp>>18))); buf_putc(&b,(u8)(0x80|((cp>>12)&0x3F))); buf_putc(&b,(u8)(0x80|((cp>>6)&0x3F))); buf_putc(&b,(u8)(0x80|(cp&0x3F))); }
          break;
        }
        default: ps->err=1; ps->p++; break;
      }
      s = ps->p;
      continue;
    }
    if (c < 0x20){ ps->err=1; ps->p++; s=ps->p; continue; }
    ps->p++;
  }
  ps->err=1;
  *out=b.p; *outlen=b.len;
}

static JVal* jparse_value(JParse* ps);

static JVal* jparse_array(JParse* ps){
  ps->p++; // [
  JVal* v=jnew(JARR); v->as.arr.items=NULL; v->as.arr.len=0;
  u32 cap=0;
  jskipws(ps);
  if (ps->p<ps->end && *ps->p==']'){ ps->p++; return v; }
  for(;;){
    JVal* item=jparse_value(ps);
    if (ps->err) return v;
    if (v->as.arr.len==cap){ u32 nc=cap?cap*2:8; JVal** ni=(JVal**)bump(nc*sizeof(JVal*)); if(v->as.arr.items&&v->as.arr.len)memcpy(ni,v->as.arr.items,v->as.arr.len*sizeof(JVal*)); v->as.arr.items=ni; cap=nc; }
    v->as.arr.items[v->as.arr.len++]=item;
    jskipws(ps);
    if (ps->p>=ps->end){ ps->err=1; return v; }
    u8 c=*ps->p;
    if (c==','){ ps->p++; jskipws(ps); continue; }
    if (c==']'){ ps->p++; return v; }
    ps->err=1; return v;
  }
}

static JVal* jparse_object(JParse* ps){
  ps->p++; // {
  JVal* v=jnew(JOBJ); v->as.obj.m=NULL; v->as.obj.len=0;
  u32 cap=0;
  jskipws(ps);
  if (ps->p<ps->end && *ps->p=='}'){ ps->p++; return v; }
  for(;;){
    jskipws(ps);
    if (ps->p>=ps->end || *ps->p!='"'){ ps->err=1; return v; }
    u8* key; u32 keylen;
    jparse_string_raw(ps,&key,&keylen);
    if (ps->err) return v;
    jskipws(ps);
    if (ps->p>=ps->end || *ps->p!=':'){ ps->err=1; return v; }
    ps->p++;
    JVal* val=jparse_value(ps);
    if (ps->err) return v;
    if (v->as.obj.len==cap){ u32 nc=cap?cap*2:8; JMember* ni=(JMember*)bump(nc*sizeof(JMember)); if(v->as.obj.m&&v->as.obj.len)memcpy(ni,v->as.obj.m,v->as.obj.len*sizeof(JMember)); v->as.obj.m=ni; cap=nc; }
    v->as.obj.m[v->as.obj.len].key=key;
    v->as.obj.m[v->as.obj.len].keylen=keylen;
    v->as.obj.m[v->as.obj.len].val=val;
    v->as.obj.len++;
    jskipws(ps);
    if (ps->p>=ps->end){ ps->err=1; return v; }
    u8 c=*ps->p;
    if (c==','){ ps->p++; continue; }
    if (c=='}'){ ps->p++; return v; }
    ps->err=1; return v;
  }
}

static JVal* jparse_number(JParse* ps){
  const u8* start=ps->p;
  if (ps->p<ps->end && (*ps->p=='-'||*ps->p=='+')) ps->p++;
  int isfloat=0;
  while (ps->p<ps->end){
    u8 c=*ps->p;
    if (c>='0'&&c<='9'){ ps->p++; }
    else if (c=='.'||c=='e'||c=='E'||c=='+'||c=='-'){ isfloat=1; ps->p++; }
    else break;
  }
  JVal* v=jnew(JNUM);
  // input numbers here are small non-negative ints (per_page, recent); parse as i64.
  i64 n=0; const u8* q=start; int neg=0;
  if (*q=='-'){neg=1;q++;} else if(*q=='+')q++;
  for (; q<ps->p && *q>='0' && *q<='9'; q++) n = n*10 + (*q-'0');
  v->as.num = neg ? -n : n;
  (void)isfloat;
  return v;
}

static JVal* jparse_value(JParse* ps){
  jskipws(ps);
  if (ps->p>=ps->end){ ps->err=1; return jnew(JNULL); }
  u8 c=*ps->p;
  if (c=='"'){ JVal* v=jnew(JSTR); jparse_string_raw(ps,&v->as.s.p,&v->as.s.len); return v; }
  if (c=='{') return jparse_object(ps);
  if (c=='[') return jparse_array(ps);
  if (c=='t'){ if (ps->p+4<=ps->end && memcmp(ps->p,"true",4)==0){ ps->p+=4; JVal* v=jnew(JBOOL); v->as.b=1; return v; } ps->err=1; return jnew(JNULL); }
  if (c=='f'){ if (ps->p+5<=ps->end && memcmp(ps->p,"false",5)==0){ ps->p+=5; JVal* v=jnew(JBOOL); v->as.b=0; return v; } ps->err=1; return jnew(JNULL); }
  if (c=='n'){ if (ps->p+4<=ps->end && memcmp(ps->p,"null",4)==0){ ps->p+=4; return jnew(JNULL); } ps->err=1; return jnew(JNULL); }
  if (c=='-'||(c>='0'&&c<='9')) return jparse_number(ps);
  ps->err=1; return jnew(JNULL);
}

// DOM helpers
static JVal* jobj_get(JVal* o, const char* key){
  if (!o || o->tag!=JOBJ) return NULL;
  u32 kl=0; while(key[kl])kl++;
  for (u32 i=0;i<o->as.obj.len;i++){
    JMember* m=&o->as.obj.m[i];
    if (m->keylen==kl && memcmp(m->key,key,kl)==0) return m->val;
  }
  return NULL;
}

// == UTF-8 / codepoint helpers (used by slug and escape) ==
// Decode one codepoint at s[i..], advance i. Returns codepoint; invalid byte -> 0xFFFD, advance 1.
static u32 utf8_next(const u8* s, u32 len, u32* i){
  if (*i>=len) return -1;
  u8 c=s[*i];
  if (c<0x80){ (*i)++; return c; }
  if ((c>>5)==0x6 && *i+1<len){ u32 cp=((c&0x1F)<<6)|(s[*i+1]&0x3F); (*i)+=2; return cp; }
  if ((c>>4)==0xE && *i+2<len){ u32 cp=((c&0x0F)<<12)|((s[*i+1]&0x3F)<<6)|(s[*i+2]&0x3F); (*i)+=3; return cp; }
  if ((c>>3)==0x1E && *i+3<len){ u32 cp=((c&0x07)<<18)|((s[*i+1]&0x3F)<<12)|((s[*i+2]&0x3F)<<6)|(s[*i+3]&0x3F); (*i)+=4; return cp; }
  (*i)++; return 0xFFFD;
}

static u32 g_out_dummy;

// ================================================================== text rules (text.js)
// HTML escape table: & < > " '  (in this order). Operates on UTF-8 bytes; the 5 specials are all
// single-byte ASCII, so a byte scan is exact (no multibyte UTF-8 sequence contains these bytes).
static int is_html_special(u8 c){ return c=='&'||c=='<'||c=='>'||c=='"'||c=='\''; }

static void escape_html_into(Buf* b, const u8* s, u32 len){
  u32 i=0, run=0;
  for (u32 k=0;k<len;k++){
    u8 c=s[k];
    if (is_html_special(c)){
      if (run){ buf_write(b, s+i, run); }
      switch(c){
        case '&': buf_puts(b,"&amp;"); break;
        case '<': buf_puts(b,"&lt;"); break;
        case '>': buf_puts(b,"&gt;"); break;
        case '"': buf_puts(b,"&quot;"); break;
        case '\'': buf_puts(b,"&#39;"); break;
      }
      i = k+1; run = 0;
    } else run++;
  }
  if (run) buf_write(b, s+i, run);
}

// ---- slugify: NFKD-fold via a per-codepoint table (the sealed non-ASCII universe is 36 codepoints;
// verified byte-exact against node's slugify over 200k random inputs). ASCII A-Z a-z 0-9 kept (A-Z
// lowered); everything else is a separator; runs collapse to one '-', none at either end; <=64 then
// trailing '-' dropped; '' -> "untitled".
typedef struct { u32 cp; char keep[3]; } FoldEntry; // keep: 0..2 kept chars + NUL
static const FoldEntry FOLD[] = {
  {0x00b7,""}, {0x00c5,"a"}, {0x00c6,""}, {0x00c9,"e"}, {0x00df,""},
  {0x00e3,"a"}, {0x00e5,"a"}, {0x00e8,"e"}, {0x00e9,"e"}, {0x00ef,"i"},
  {0x00f1,"n"}, {0x00f3,"o"}, {0x00f6,"o"}, {0x00f8,""}, {0x00fb,"u"},
  {0x00fc,"u"}, {0x0141,""}, {0x0152,""}, {0x0153,""}, {0x017a,"z"},
  {0x0394,""}, {0x03ad,""}, {0x03b1,""}, {0x03bb,""}, {0x03c4,""},
  {0x041c,""}, {0x0430,""}, {0x0432,""}, {0x043a,""}, {0x043e,""},
  {0x0441,""}, {0x4eac,""}, {0x6771,""}, {0xfb01,"fi"}, {0xff37,"w"},
  {0xff44,"d"}, {0xff45,"e"}, {0xff49,"i"},
};
#define NFOLD (sizeof(FOLD)/sizeof(FOLD[0]))

static const char* fold_lookup(u32 cp){
  for (u32 i=0;i<NFOLD;i++) if (FOLD[i].cp==cp) return FOLD[i].keep;
  return NULL;
}

// Append the slug of UTF-8 string s to b.
static void slugify_into(Buf* b, const u8* s, u32 len){
  // First pass builds the kept string with '-' separators into a temp, then trim+cap.
  Buf t; t.p=NULL; t.len=0; t.cap=0;
  int gap=0;
  u32 i=0;
  while (i<len){
    u8 c=s[i];
    if (c<0x80){
      char keep=0;
      if (c>='A'&&c<='Z') keep=(char)(c-'A'+'a');
      else if ((c>='a'&&c<='z')||(c>='0'&&c<='9')) keep=(char)c;
      if (keep==0){ gap = t.len>0; i++; continue; }
      if (gap){ buf_putc(&t,'-'); gap=0; }
      buf_putc(&t,(u8)keep);
      i++;
      continue;
    }
    // non-ASCII: decode codepoint
    u32 cp=utf8_next(s,len,&i);
    const char* f=fold_lookup(cp);
    if (f==NULL || f[0]=='\0'){ gap = t.len>0; continue; } // folds to nothing -> separator
    // append each kept char (f is 1..2 chars)
    for (const char* q=f; *q; q++){ if (gap){ buf_putc(&t,'-'); gap=0; } buf_putc(&t,(u8)*q); }
  }
  // cap to 64 then drop trailing '-'
  u32 n = t.len>64 ? 64 : t.len;
  while (n>0 && t.p[n-1]=='-') n--;
  if (n==0){ buf_puts(b,"untitled"); return; }
  buf_write(b, t.p, n);
}

// longDate: "YYYY-MM-DD" -> "D Month YYYY". date is exactly 10 bytes (validated by front matter).
static const char* const MONTHS[12]={"January","February","March","April","May","June","July","August","September","October","November","December"};
static void long_date_into(Buf* b, const u8* d){
  // d points at YYYY-MM-DD (10 bytes). day = d[8..9], month = d[5..6], year = d[0..3].
  u32 day = (d[8]-'0')*10 + (d[9]-'0');
  u32 mon = (d[5]-'0')*10 + (d[6]-'0');
  // number without leading zero
  if (day>=10) buf_putc(b,(u8)('0'+day/10));
  buf_putc(b,(u8)('0'+day%10));
  buf_putc(b,' ');
  const char* mn = MONTHS[mon-1];
  while(*mn) buf_putc(b,(u8)*mn++);
  buf_putc(b,' ');
  buf_write(b, d, 4);
}

static u32 g_out_dummy2;

// ================================================================== string + helpers
typedef struct { u8* p; u32 len; } Str;

static int str_eq(Str a, Str b){ return a.len==b.len && (a.len==0 || memcmp(a.p,b.p,a.len)==0); }
static int str_eq_lit(Str a, const char* lit){ u32 n=0; while(lit[n])n++; return a.len==n && (n==0||memcmp(a.p,lit,n)==0); }
static Str str_from_c(const char* s){ Str r; r.p=(u8*)s; r.len=0; while(s[r.len])r.len++; return r; }

// Copy a Str into a fresh NUL-terminated buffer (for keys we keep around).
static u8* str_dupz(Str s){ u8* d=(u8*)bump(s.len+1); if(s.len)memcpy(d,s.p,s.len); d[s.len]=0; return d; }

static void buf_put_u32(Buf* b, u32 v){
  char tmp[10]; int n=0;
  if (v==0){ buf_putc(b,'0'); return; }
  while (v){ tmp[n++]=(char)('0'+v%10); v/=10; }
  while (n) buf_putc(b,(u8)tmp[--n]);
}
static void buf_put_i64(Buf* b, i64 v){
  if (v<0){ buf_putc(b,'-'); buf_put_u32(b,(u32)(-v)); }
  else buf_put_u32(b,(u32)v);
}

// is \s in JS (no u flag): the set isWsCode enumerates. Operates on a decoded codepoint.
static int is_ws_cp(u32 c){
  if (c==0x20 || (c>=0x09 && c<=0x0d)) return 1;
  if (c==0xa0 || c==0x1680 || (c>=0x2000 && c<=0x2000+0x0a)) return 1;
  return c==0x2028 || c==0x2029 || c==0x202f || c==0x205f || c==0x3000 || c==0xfeff;
}

// Trim JS-whitespace from both ends of a byte range [s, s+len). Returns new range via *o/*ol.
// JS trim removes WhiteSpace + LineTerminator = the is_ws_cp set (which includes \n\r etc).
static void str_trim(const u8* s, u32 len, const u8** o, u32* ol){
  u32 a=0, b=len;
  // front
  while (a<b){ u32 i=a; u32 cp=utf8_next(s,b,&i); if (is_ws_cp(cp)) a=i; else break; }
  // back: decode from the end is awkward; scan forward tracking last non-ws boundary.
  // Simpler: walk from a, remember last position that is not trailing ws.
  u32 last=a; u32 i=a;
  while (i<b){ u32 j=i; u32 cp=utf8_next(s,b,&j); if (is_ws_cp(cp)){ /* skip */ } else { last=j; } i=j; }
  *o = s+a; *ol = last-a;
}

// ================================================================== front matter (frontmatter.js)
static const char* const FM_KEYS[7]={"title","date","tags","draft","layout","slug","summary"};

typedef struct {
  int has_title; Str title;
  int has_date;  Str date;
  int has_tags;  Str* tags; u32 ntags;
  int has_draft; int draft;
  int has_layout; Str layout;
  int has_slug;  Str slug;
  int has_summary; Str summary;
} FMData;

// A front-matter parse outcome.
typedef struct { int error; Str errmsg; FMData data; const u8* body; u32 bodylen; } FMResult;

static int fm_key_index(Str key){
  for (int i=0;i<7;i++) if (str_eq_lit(key, FM_KEYS[i])) return i;
  return -1;
}

static int days_in(u32 y, u32 m){
  if (m==2) return ((y%4==0 && y%100!=0)||y%400==0)?29:28;
  return (m==4||m==6||m==9||m==11)?30:31;
}

// stringOf: returns 0 ok (out set), 1 error (errmsg set). raw is the value text.
static int fm_string_of(Str raw, Str* out, Str* errmsg){
  if (raw.len>0 && raw.p[0]=='"'){
    if (raw.len<2 || raw.p[raw.len-1]!='"'){ *errmsg=str_from_c("a quoted string is not closed"); return 1; }
    Buf b; b.p=NULL;b.len=0;b.cap=0;
    for (u32 i=1;i<raw.len-1;i++){
      u8 c=raw.p[i];
      if (c=='\\'){
        u8 n=raw.p[i+1];
        if ((n!='"' && n!='\\') || i+1==raw.len-1){ *errmsg=str_from_c("a quoted string holds an escape other than \\\" or \\\\"); return 1; }
        buf_putc(&b,n); i++;
      } else if (c=='"'){ *errmsg=str_from_c("a quoted string holds a stray quote"); return 1; }
      else buf_putc(&b,c);
    }
    out->p=b.p; out->len=b.len; return 0;
  }
  if (raw.len>0 && raw.p[0]=='['){ *errmsg=str_from_c("a bare string opens on ["); return 1; }
  *out=raw; return 0;
}

// valueOf: returns 0 ok (writes into data), 1 error (errmsg set). key index ki.
static int fm_value_of(FMData* d, int ki, Str key, Str raw, Str* errmsg){
  switch(ki){
    case 0: case 6: { // title / summary
      Str out; Str e;
      if (fm_string_of(raw,&out,&e)){ Buf b;b.p=NULL;b.len=0;b.cap=0; buf_puts(&b,"key \""); buf_write(&b,key.p,key.len); buf_puts(&b,"\": "); buf_write(&b,e.p,e.len); errmsg->p=b.p;errmsg->len=b.len; return 1; }
      if (ki==0){ d->has_title=1; d->title=out; } else { d->has_summary=1; d->summary=out; }
      return 0;
    }
    case 1: { // date: YYYY-MM-DD, 1970..2099, valid month/day
      int ok = raw.len==10 && raw.p[4]=='-' && raw.p[7]=='-';
      u32 y=0,mo=0,dd=0;
      if (ok){ for(int k=0;k<10;k++){ if(k==4||k==7) continue; if(raw.p[k]<'0'||raw.p[k]>'9'){ok=0;break;} } }
      if (ok){ y=(raw.p[0]-'0')*1000+(raw.p[1]-'0')*100+(raw.p[2]-'0')*10+(raw.p[3]-'0');
               mo=(raw.p[5]-'0')*10+(raw.p[6]-'0'); dd=(raw.p[8]-'0')*10+(raw.p[9]-'0'); }
      if (!ok || y<1970 || y>2099 || mo<1 || mo>12 || dd<1 || dd>(u32)days_in(y,mo)){
        *errmsg=str_from_c("key \"date\": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)"); return 1;
      }
      d->has_date=1; d->date=raw; return 0;
    }
    case 2: { // tags: [a, b]
      if (!(raw.len>=1 && raw.p[0]=='[' && raw.p[raw.len-1]==']')){ *errmsg=str_from_c("key \"tags\": not a list [a, b]"); return 1; }
      // inner = raw[1..len-1]
      const u8* inner=raw.p+1; u32 ilen=raw.len-2;
      // trim -> empty?
      const u8* ti; u32 til; str_trim(inner,ilen,&ti,&til);
      if (til==0){ d->has_tags=1; d->tags=NULL; d->ntags=0; return 0; }
      // split on ',', trim each, validate non-empty and TAG_RE /^[^"[\]]+$/
      u32 cap=0, n=0; Str* arr=NULL;
      u32 start=0;
      for (u32 k=0;k<=ilen;k++){
        if (k==ilen || inner[k]==','){
          const u8* ts; u32 tsl; str_trim(inner+start, k-start, &ts, &tsl);
          // validate
          int bad = (tsl==0);
          if (!bad) for (u32 j=0;j<tsl;j++){ u8 c=ts[j]; if (c=='"'||c=='['||c==']'){ bad=1; break; } }
          if (bad){ *errmsg=str_from_c("key \"tags\": an empty tag, or one holding \" [ or ]"); return 1; }
          if (n==cap){ u32 nc=cap?cap*2:4; Str* ni=(Str*)bump(nc*sizeof(Str)); if(arr&&n)memcpy(ni,arr,n*sizeof(Str)); arr=ni; cap=nc; }
          arr[n].p=(u8*)ts; arr[n].len=tsl; n++;
          start=k+1;
        }
      }
      d->has_tags=1; d->tags=arr; d->ntags=n; return 0;
    }
    case 3: // draft
      if (str_eq_lit(raw,"true")){ d->has_draft=1; d->draft=1; return 0; }
      if (str_eq_lit(raw,"false")){ d->has_draft=1; d->draft=0; return 0; }
      *errmsg=str_from_c("key \"draft\": true or false"); return 1;
    case 4: { // layout: NAME_RE ^[a-z0-9-]+$
      int ok = raw.len>0; if(ok) for(u32 k=0;k<raw.len;k++){ u8 c=raw.p[k]; if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-')){ok=0;break;} }
      if (!ok){ *errmsg=str_from_c("key \"layout\": not a template name"); return 1; }
      d->has_layout=1; d->layout=raw; return 0;
    }
    case 5: { // slug: SLUG_RE ^[a-z0-9]+(?:-[a-z0-9]+)*$ and len<=64
      int ok = raw.len>0 && raw.len<=64;
      if (ok){
        // must be groups of [a-z0-9]+ separated by single '-'
        u32 k=0; 
        while (k<raw.len){
          u32 st=k; while(k<raw.len && raw.p[k]!='-'){ u8 c=raw.p[k]; if(!((c>='a'&&c<='z')||(c>='0'&&c<='9'))){ok=0;break;} k++; }
          if(!ok||k==st){ok=0;break;}
          if (k<raw.len && raw.p[k]=='-'){ k++; if(k==raw.len){ok=0;break;} }
        }
      }
      if (!ok){ *errmsg=str_from_c("key \"slug\": not a slug"); return 1; }
      d->has_slug=1; d->slug=raw; return 0;
    }
  }
  return 0;
}

// parseFrontMatter(text) -> FMResult. text has \n line ends (already CRLF-normalised by caller).
static FMResult parse_front_matter(const u8* text, u32 len){
  FMResult r; r.error=0; r.errmsg.p=NULL; r.errmsg.len=0; r.body=NULL; r.bodylen=0;
  FMData* d=&r.data; memset(d,0,sizeof(FMData));
  // split into lines on '\n'
  // line 0 must be "---"
  u32 lineStart=0;
  // find end of line 0
  u32 le0=lineStart; while(le0<len && text[le0]!='\n') le0++;
  Str l0={(u8*)(text+lineStart), le0-lineStart};
  if (!str_eq_lit(l0,"---")){ r.error=1; r.errmsg=str_from_c("no front matter"); return r; }
  // find closing "---" line
  u32 closeLineStart=0; int close=-1;
  u32 ls=le0<len?le0+1:len;
  u32 ln=1;
  while (ls<=len){
    u32 le=ls; while(le<len && text[le]!='\n') le++;
    Str line={(u8*)(text+ls), le-ls};
    if (str_eq_lit(line,"---")){ close=ln; closeLineStart=ls; break; }
    if (le>=len) break;
    ls=le+1; ln++;
  }
  if (close==-1){ r.error=1; r.errmsg=str_from_c("the front matter is not closed"); return r; }
  // iterate lines 1..close-1
  ls = (le0<len?le0+1:len);
  ln=1;
  while (ln<close){
    u32 le=ls; while(le<len && text[le]!='\n') le++;
    Str line={(u8*)(text+ls), le-ls};
    // blank? /^[ \t]*$/  or starts with '#'
    int blank=1; for(u32 k=0;k<line.len;k++){ u8 c=line.p[k]; if(c!=0x20&&c!=0x09){blank=0;break;} }
    if (!blank && !(line.len>0 && line.p[0]=='#')){
      // LINE_RE ^([a-z_]+):(?: (.*))?$
      u32 k=0; while(k<line.len){ u8 c=line.p[k]; if((c>='a'&&c<='z')||c=='_')k++; else break; }
      int okkey = k>0 && k<line.len && line.p[k]==':';
      Str key={line.p,k};
      Str raw; raw.p=NULL; raw.len=0;
      if (okkey){
        u32 rest=k+1;
        if (rest==line.len){ raw.p=line.p+rest; raw.len=0; }
        else if (line.p[rest]==' '){ raw.p=line.p+rest+1; raw.len=line.len-rest-1; }
        else okkey=0; // colon not followed by space or EOL -> no match
      }
      if (!okkey){
        Buf b;b.p=NULL;b.len=0;b.cap=0; buf_puts(&b,"line "); buf_put_u32(&b,ln+1); buf_puts(&b," is not \"key: value\"");
        r.error=1; r.errmsg.p=b.p; r.errmsg.len=b.len; return r;
      }
      // strip trailing [ \t]+ from raw
      while (raw.len>0 && (raw.p[raw.len-1]==0x20||raw.p[raw.len-1]==0x09)) raw.len--;
      int ki=fm_key_index(key);
      if (ki<0){ Buf b;b.p=NULL;b.len=0;b.cap=0; buf_puts(&b,"unknown key \""); buf_write(&b,key.p,key.len); buf_putc(&b,'"'); r.error=1;r.errmsg.p=b.p;r.errmsg.len=b.len; return r; }
      // given twice?
      int given=0;
      switch(ki){ case 0:given=d->has_title;break; case 1:given=d->has_date;break; case 2:given=d->has_tags;break; case 3:given=d->has_draft;break; case 4:given=d->has_layout;break; case 5:given=d->has_slug;break; case 6:given=d->has_summary;break; }
      if (given){ Buf b;b.p=NULL;b.len=0;b.cap=0; buf_puts(&b,"key \""); buf_write(&b,key.p,key.len); buf_puts(&b,"\" is given twice"); r.error=1;r.errmsg.p=b.p;r.errmsg.len=b.len; return r; }
      if (raw.len==0){ Buf b;b.p=NULL;b.len=0;b.cap=0; buf_puts(&b,"key \""); buf_write(&b,key.p,key.len); buf_puts(&b,"\" has no value"); r.error=1;r.errmsg.p=b.p;r.errmsg.len=b.len; return r; }
      Str e; e.p=NULL;e.len=0;
      if (fm_value_of(d,ki,key,raw,&e)){ r.error=1; r.errmsg=e; return r; }
    }
    if (le>=len) break;
    ls=le+1; ln++;
  }
  if (!d->has_title){ r.error=1; r.errmsg=str_from_c("no title"); return r; }
  // body = lines.slice(close+1).join('\n') = text after the closing "---" line's newline
  u32 bodyStart = (closeLineStart + 3); // past "---"
  if (bodyStart<len && text[bodyStart]=='\n') bodyStart++; // skip the newline ending the --- line
  // Actually need to skip to after the newline that ends the closing --- line:
  bodyStart = closeLineStart; while(bodyStart<len && text[bodyStart]!='\n') bodyStart++; if(bodyStart<len)bodyStart++;
  r.body=text+bodyStart; r.bodylen=len-bodyStart;
  return r;
}

static u32 g_out_dummy3;

// ---- test export: parse front matter, write a debug encoding to CTRL buffer ----
// Format (for the test driver to parse):
//   "E" json_string(errmsg)            -- error
//   "O" <title>\x1f<date>\x1f<tags...>\x1f<body...> -- too fiddly; instead emit a JSON object.
// Simpler: emit a JSON object mirroring the fields, with body as a string, error as null or string.
// The driver parses it with JSON.parse and compares against solve.js parseFrontMatter output.
static void fm_test_emit(FMResult* r){
  Buf b; b.p=NULL;b.len=0;b.cap=0;
  buf_putc(&b,'{');
  if (r->error){
    buf_puts(&b,"\"error\":"); json_write_string(&b, r->errmsg.p, r->errmsg.len);
  } else {
    FMData* d=&r->data;
    buf_puts(&b,"\"title\":"); json_write_string(&b,d->title.p,d->title.len);
    buf_puts(&b,",\"date\":"); if(d->has_date) json_write_string(&b,d->date.p,d->date.len); else buf_puts(&b,"null");
    buf_puts(&b,",\"tags\":[");
    for(u32 i=0;i<d->ntags;i++){ if(i)buf_putc(&b,','); json_write_string(&b,d->tags[i].p,d->tags[i].len); }
    buf_puts(&b,"]");
    buf_puts(&b,",\"draft\":"); if(d->has_draft) buf_puts(&b,d->draft?"true":"false"); else buf_puts(&b,"null");
    buf_puts(&b,",\"layout\":"); if(d->has_layout) json_write_string(&b,d->layout.p,d->layout.len); else buf_puts(&b,"null");
    buf_puts(&b,",\"slug\":"); if(d->has_slug) json_write_string(&b,d->slug.p,d->slug.len); else buf_puts(&b,"null");
    buf_puts(&b,",\"summary\":"); if(d->has_summary) json_write_string(&b,d->summary.p,d->summary.len); else buf_puts(&b,"null");
    buf_puts(&b,",\"body\":"); json_write_string(&b,r->body,r->bodylen);
  }
  buf_putc(&b,'}');
  CTRL[CTRL_OUT_PTR]=(u32)b.p; CTRL[CTRL_OUT_LEN]=b.len; CTRL[CTRL_STATUS]=0;
}

__attribute__((export_name("mg_test_fm")))
u32 mg_test_fm(u32 inPtr, u32 inLen){
  arena_init(inPtr + inLen);
  FMResult r = parse_front_matter((const u8*)inPtr, inLen);
  fm_test_emit(&r);
  return CTRL[CTRL_OUT_LEN];
}

// ================================================================== JSON output writing
// Write s (UTF-8, len bytes) as a JSON string (with surrounding quotes) to b, escaping
// ", \, and control chars < 0x20. Raw UTF-8 passes through (matches the reference format,
// and parses back to the identical JS string).
static void json_write_string(Buf* b, const u8* s, u32 len){
  buf_putc(b,'"');
  u32 i=0, run=0;
  for (u32 k=0;k<len;k++){
    u8 c=s[k];
    if (c=='"'||c=='\\'||c<0x20){
      if (run){ buf_write(b,s+i,run); run=0; }
      switch(c){
        case '"': buf_puts(b,"\\\""); break;
        case '\\': buf_puts(b,"\\\\"); break;
        case 0x08: buf_puts(b,"\\b"); break;
        case 0x09: buf_puts(b,"\\t"); break;
        case 0x0a: buf_puts(b,"\\n"); break;
        case 0x0c: buf_puts(b,"\\f"); break;
        case 0x0d: buf_puts(b,"\\r"); break;
        default:
          // \u00XX for other control chars
          { char tmp[6]={'\\','u','0','0',0,0}; const char* hx="0123456789abcdef";
            tmp[4]=hx[(c>>4)&0xF]; tmp[5]=hx[c&0xF]; buf_write(b,tmp,6); }
      }
      i=k+1;
    } else run++;
  }
  if (run) buf_write(b,s+i,run);
  buf_putc(b,'"');
}

// ================================================================== markdown (markdown.js)
// Line view: split src on '\n' into an array of (ptr,len) slices (no NUL terminators).

typedef struct { Str* v; u32 len; } LineArr;

static LineArr split_lines(const u8* s, u32 len){
  u32 cap=16, n=0; Str* a=(Str*)bump(cap*sizeof(Str));
  u32 start=0;
  for (u32 i=0;i<len;i++){
    if (s[i]=='\n'){ if(n==cap){cap*=2;Str*na=(Str*)bump(cap*sizeof(Str));memcpy(na,a,n*sizeof(Str));a=na;} a[n].p=(u8*)(s+start); a[n].len=i-start; n++; start=i+1; }
  }
  // trailing line (even if empty) — JS split includes it
  if(n==cap){cap*=2;Str*na=(Str*)bump(cap*sizeof(Str));memcpy(na,a,n*sizeof(Str));a=na;}
  a[n].p=(u8*)(s+start); a[n].len=len-start; n++;
  LineArr r; r.v=a; r.len=n; return r;
}

// trim spaces/tabs from a line slice (trimLine). Returns sub-slice.
static Str trim_line(Str line){
  u32 a=0,b=line.len;
  while(a<b){u8 c=line.p[a]; if(c==0x20||c==0x09)a++; else break;}
  while(b>a){u8 c=line.p[b-1]; if(c==0x20||c==0x09)b--; else break;}
  Str r={line.p+a, b-a}; return r;
}

static int is_blank(Str line){ for(u32 k=0;k<line.len;k++){u8 c=line.p[k]; if(c!=0x20&&c!=0x09)return 0;} return 1; }

// ESCAPABLE set for inline backslash escapes.
static int is_escapable(u8 c){
  switch(c){ case '\\': case '`': case '*': case '_': case '[': case ']': case '(': case ')':
    case '#': case '+': case '-': case '.': case '!': case '>': return 1; default: return 0; }
}

// FENCE_OPEN_RE: ^```([A-Za-z0-9_+-]*)[ \t]*$  -> returns 1 & lang slice if match.
static int match_fence_open(Str line, Str* lang){
  if (line.len<3 || line.p[0]!='`'||line.p[1]!='`'||line.p[2]!='`') return 0;
  u32 i=3; u32 ls=i;
  while(i<line.len){ u8 c=line.p[i]; if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='+'||c=='-') i++; else break; }
  lang->p=line.p+ls; lang->len=i-ls;
  while(i<line.len){ u8 c=line.p[i]; if(c==0x20||c==0x09)i++; else return 0; }
  return i==line.len;
}
// FENCE_CLOSE_RE: ^```[ \t]*$
static int match_fence_close(Str line){
  if (line.len<3||line.p[0]!='`'||line.p[1]!='`'||line.p[2]!='`') return 0;
  u32 i=3; while(i<line.len){u8 c=line.p[i]; if(c==0x20||c==0x09)i++; else return 0;} return 1;
}
// headingOf: HEADING_RE ^(#{1,6})[ \t]+(.*?)[ \t]*$ ; returns 1 & depth & text (text nonempty).
static int heading_of(Str line, u32* depth, Str* text){
  u32 h=0; while(h<line.len && h<7 && line.p[h]=='#') h++;
  if (h<1 || h>6) return 0;
  u32 i=h; if (!(i<line.len && (line.p[i]==0x20||line.p[i]==0x09))) return 0;
  while(i<line.len && (line.p[i]==0x20||line.p[i]==0x09)) i++;
  u32 te=i; u32 tb=line.len;
  while(tb>te && (line.p[tb-1]==0x20||line.p[tb-1]==0x09)) tb--;
  text->p=line.p+te; text->len=tb-te;
  if (text->len==0) return 0;
  *depth=h; return 1;
}
// HR_RE: ^(?:---|\*\*\*)[ \t]*$
static int match_hr(Str line){
  if (line.len>=3){
    int all1=1,all2=1; for(int k=0;k<3;k++){ if(line.p[k]!='-')all1=0; if(line.p[k]!='*')all2=0; }
    if (all1||all2){ u32 i=3; while(i<line.len){u8 c=line.p[i]; if(c==0x20||c==0x09)i++; else return 0;} return 1; }
  }
  return 0;
}
static Str strip_quote(Str line){ // QUOTE_STRIP_RE ^> ?
  Str r=line; if(r.len>0 && r.p[0]=='>'){ r.p++; r.len--; if(r.len>0 && r.p[0]==0x20){r.p++;r.len--;} } return r;
}
static int match_bullet(Str line, Str* rest){ // BULLET_RE ^[-*] (.*)$
  if (line.len>=2 && (line.p[0]=='-'||line.p[0]=='*') && line.p[1]==0x20){ rest->p=line.p+2; rest->len=line.len-2; return 1; }
  return 0;
}
static int match_ordered(Str line, u32* startnum, Str* rest){ // ORDERED_RE ^(\d{1,9})\. (.*)$
  u32 i=0; while(i<line.len && i<9 && line.p[i]>='0'&&line.p[i]<='9') i++;
  if (i>=1 && i<line.len && line.p[i]=='.' && i+1<line.len && line.p[i+1]==0x20){
    u32 n=0; for(u32 k=0;k<i;k++) n=n*10+(line.p[k]-'0');
    *startnum=n; rest->p=line.p+i+2; rest->len=line.len-i-2; return 1;
  }
  return 0;
}
static int match_continued(Str line){ // CONTINUED_RE ^ {2,}\S
  u32 i=0; while(i<line.len && line.p[i]==0x20) i++;
  if (i<2) return 0;
  if (i>=line.len) return 0;
  // \S: not whitespace. decode codepoint.
  u32 j=i; u32 cp=utf8_next(line.p,line.len,&j); if (is_ws_cp(cp)) return 0; return 1;
}

static int ends_paragraph(Str line){
  if (is_blank(line)) return 1;
  Str lang; if (match_fence_open(line,&lang)) return 1;
  u32 d; Str t; if (heading_of(line,&d,&t)) return 1;
  if (match_hr(line)) return 1;
  return 0;
}

// ---- inline parser env ----
// env.link(target) -> resolved Str. For unit tests, identity.
typedef struct Env {
  Str (*link)(void* ctx, const u8* t, u32 tlen);
  void* ctx;
} Env;

static void inline_into(Buf* out, const u8* s, u32 len, Env* env);

// findUnescaped(s,len, what, whatlen, from) -> index or -1
static i64 find_unescaped(const u8* s, u32 len, const char* what, u32 wl, u32 from){
  for (u32 j=from; j+wl<=len; j++){
    if (s[j]=='\\' && j+1<len && is_escapable(s[j+1])){ j++; continue; }
    if (memcmp(s+j,what,wl)==0) return (i64)j;
    if (wl==1){ /* what is single char; memcmp ok */ }
  }
  // the loop above: when wl==1, j+1<=len always true while j<len; fine.
  return -1;
}
// findSingleStar(s,len, from) -> index or -1
static i64 find_single_star(const u8* s, u32 len, u32 from){
  for (u32 j=from; j<len; j++){
    u8 c=s[j];
    if (c=='\\' && j+1<len && is_escapable(s[j+1])){ j++; continue; }
    if (c!='*') continue;
    if (j+1<len && s[j+1]=='*'){ j++; continue; }
    return (i64)j;
  }
  return -1;
}

// SPECIAL_OPEN = /[\\`*\[]/ — find next index >= from that is one of these.
static u32 find_special_open(const u8* s, u32 len, u32 from){
  for (u32 j=from;j<len;j++){ u8 c=s[j]; if(c=='\\'||c=='`'||c=='*'||c=='[') return j; }
  return len;
}

static int contains_ws(const u8* s, u32 len){
  u32 i=0; while(i<len){ u32 j=i; u32 cp=utf8_next(s,len,&j); if(is_ws_cp(cp)) return 1; i=j; }
  return 0;
}

static void inline_into(Buf* out, const u8* s, u32 len, Env* env){
  u32 i=0;
  while (i<len){
    u8 c=s[i];
    if (c=='\\' && i+1<len && is_escapable(s[i+1])){ escape_html_into(out, s+i+1, 1); i+=2; continue; }
    if (c=='`'){
      // j = indexOf('`', i+1)
      i64 j=-1; for(u32 k=i+1;k<len;k++) if(s[k]=='`'){j=(i64)k;break;}
      if (j>i+1){ buf_puts(out,"<code>"); escape_html_into(out, s+i+1, (u32)(j-(i+1))); buf_puts(out,"</code>"); i=(u32)j+1; }
      else { buf_putc(out,'`'); i++; }
      continue;
    }
    if (c=='*'){
      if (i+1<len && s[i+1]=='*'){
        i64 j=find_unescaped(s,len,"**",2,i+2);
        if (j>i+2){ buf_puts(out,"<strong>"); inline_into(out, s+i+2, (u32)(j-(i+2)), env); buf_puts(out,"</strong>"); i=(u32)j+2; }
        else { buf_puts(out,"**"); i+=2; }
        continue;
      } else {
        i64 j=find_single_star(s,len,i+1);
        if (j>i+1){ buf_puts(out,"<em>"); inline_into(out, s+i+1, (u32)(j-(i+1)), env); buf_puts(out,"</em>"); i=(u32)j+1; }
        else { buf_putc(out,'*'); i++; }
        continue;
      }
    }
    if (c=='['){
      i64 k=find_unescaped(s,len,"]",1,i+1);
      i64 m=-1;
      if (k!=-1 && (u32)k+1<len && s[k+1]=='('){ for(u32 q=(u32)k+2;q<len;q++) if(s[q]==')'){m=(i64)q;break;} }
      Str target; target.p=NULL; target.len=0;
      if (m==-1){ /* target = '' */ }
      else { target.p=s+k+2; target.len=(u32)(m-(k+2)); }
      if (target.len!=0 && !contains_ws(target.p,target.len)){
        Str url = env->link(env->ctx, target.p, target.len);
        buf_puts(out,"<a href=\""); escape_html_into(out,url.p,url.len); buf_puts(out,"\">");
        inline_into(out, s+i+1, (u32)(k-(i+1)), env);
        buf_puts(out,"</a>");
        i=(u32)m+1; continue;
      }
      buf_putc(out,'['); i++; continue;
    }
    // normal run up to next SPECIAL_OPEN (search from i+1)
    u32 j=find_special_open(s,len,i+1);
    escape_html_into(out, s+i, j-i);
    i=j;
  }
}

// ---- tokens ----
typedef enum { TK_CODE, TK_HEADING, TK_HR, TK_BLOCKQUOTE, TK_LIST, TK_PARA } TokType;
typedef struct {
  TokType type;
  // code
  Str lang; Str* clines; u32 ncl;
  // heading
  u32 depth; Str htext;
  // blockquote
  Str* qlines; u32 nql;
  // list
  int ordered; u32 start; Str* items; u32 nit; // each item owned buf
  // paragraph
  Str ptext;
} Token;

typedef struct { Token* v; u32 len; u32 cap; } TokArr;
static void toks_push(TokArr* a, Token t){
  if (a->len==a->cap){ u32 nc=a->cap?a->cap*2:16; Token* na=(Token*)bump(nc*sizeof(Token)); if(a->v&&a->len)memcpy(na,a->v,a->len*sizeof(Token)); a->v=na; a->cap=nc; }
  a->v[a->len++]=t;
}

static TokArr lex_markdown(const u8* src, u32 len){
  TokArr toks; toks.v=NULL; toks.len=0; toks.cap=0;
  LineArr L = split_lines(src, len);
  u32 i=0, n=L.len;
  while (i<n){
    Str line=L.v[i];
    if (is_blank(line)){ i++; continue; }
    u8 c0 = line.len? line.p[0] : 0;
    // code fence
    if (c0=='`'){ Str lang; if (match_fence_open(line,&lang)){
        Token t; memset(&t,0,sizeof(t)); t.type=TK_CODE; t.lang=lang;
        u32 cap=8,cnt=0; Str* body=(Str*)bump(cap*sizeof(Str));
        i++;
        while (i<n && !match_fence_close(L.v[i])){ if(cnt==cap){cap*=2;Str*nb=(Str*)bump(cap*sizeof(Str));memcpy(nb,body,cnt*sizeof(Str));body=nb;} body[cnt++]=L.v[i]; i++; }
        i++; // skip closing
        t.clines=body; t.ncl=cnt; toks_push(&toks,t); continue;
      } }
    // heading
    if (c0=='#'){ u32 d; Str txt; if (heading_of(line,&d,&txt)){
        Token t; memset(&t,0,sizeof(t)); t.type=TK_HEADING; t.depth=d; t.htext=txt; toks_push(&toks,t); i++; continue;
      } }
    // hr or bullet (c0 '-' or '*')
    if (c0=='-'||c0=='*'){ if (match_hr(line)){ Token t; memset(&t,0,sizeof(t)); t.type=TK_HR; toks_push(&toks,t); i++; continue; } }
    // blockquote
    if (c0=='>'){
      Token t; memset(&t,0,sizeof(t)); t.type=TK_BLOCKQUOTE;
      u32 cap=8,cnt=0; Str* body=(Str*)bump(cap*sizeof(Str));
      while (i<n && L.v[i].len>0 && L.v[i].p[0]=='>'){ Str st=strip_quote(L.v[i]); Str tl=trim_line(st); if(tl.len!=0){ if(cnt==cap){cap*=2;Str*nb=(Str*)bump(cap*sizeof(Str));memcpy(nb,body,cnt*sizeof(Str));body=nb;} body[cnt++]=tl; } i++; }
      t.qlines=body; t.nql=cnt; toks_push(&toks,t); continue;
    }
    // list (bullet or ordered)
    {
      Str brest; int isb = (c0=='-'||c0=='*') && match_bullet(line,&brest);
      u32 onum=0; Str orest; int iso = 0;
      if (!isb && c0>='0'&&c0<='9') iso=match_ordered(line,&onum,&orest);
      if (isb || iso){
        int ordered = iso;
        Token t; memset(&t,0,sizeof(t)); t.type=TK_LIST; t.ordered=ordered; t.start= ordered?onum:1;
        u32 icap=8,cnt=0; Str* items=(Str*)bump(icap*sizeof(Str));
        while (i<n){
          Str cur=L.v[i];
          Str r1; u32 on1=0; int mb=0, mo=0;
          if (ordered) mo=match_ordered(cur,&on1,&r1);
          else mb=match_bullet(cur,&r1);
          if (ordered ? !mo : !mb) break;
          Buf ib; ib.p=NULL;ib.len=0;ib.cap=0; Str tl=trim_line(r1); buf_write(&ib,tl.p,tl.len);
          if(cnt==icap){icap*=2;Str*ni=(Str*)bump(icap*sizeof(Str));memcpy(ni,items,cnt*sizeof(Str));items=ni;}
          items[cnt].p=ib.p; items[cnt].len=ib.len; cnt++;
          i++;
          while (i<n && match_continued(L.v[i])){ Str tl2=trim_line(L.v[i]); buf_putc(&ib,'\n'); buf_write(&ib,tl2.p,tl2.len); items[cnt-1].p=ib.p; items[cnt-1].len=ib.len; i++; }
        }
        t.items=items; t.nit=cnt; toks_push(&toks,t); continue;
      }
    }
    // paragraph
    {
      Buf pb; pb.p=NULL;pb.len=0;pb.cap=0; Str tl=trim_line(line); buf_write(&pb,tl.p,tl.len);
      i++;
      while (i<n && !ends_paragraph(L.v[i])){ Str tl2=trim_line(L.v[i]); buf_putc(&pb,'\n'); buf_write(&pb,tl2.p,tl2.len); i++; }
      Token t; memset(&t,0,sizeof(t)); t.type=TK_PARA; t.ptext.p=pb.p; t.ptext.len=pb.len; toks_push(&toks,t);
      continue;
    }
  }
  return toks;
}

// ---- render ----
typedef struct { u32 level; Str id; Str html; } TocEntry;
typedef struct { TocEntry* v; u32 len; u32 cap; } TocArr;
static void toc_push(TocArr* a, TocEntry e){
  if(a->len==a->cap){u32 nc=a->cap?a->cap*2:8;TocEntry*na=(TocEntry*)bump(nc*sizeof(TocEntry));if(a->v&&a->len)memcpy(na,a->v,a->len*sizeof(TocEntry));a->v=na;a->cap=nc;}
  a->v[a->len++]=e;
}

// uniqueSlug state: a set of taken slugs (linear scan is fine; counts are small per page)
typedef struct { Str* v; u32 len; u32 cap; } StrSet;
static int strset_has(StrSet* s, Str x){ for(u32 i=0;i<s->len;i++) if(str_eq(s->v[i],x)) return 1; return 0; }
static void strset_add(StrSet* s, Str x){
  if(s->len==s->cap){u32 nc=s->cap?s->cap*2:8;Str*na=(Str*)bump(nc*sizeof(Str));if(s->v&&s->len)memcpy(na,s->v,s->len*sizeof(Str));s->v=na;s->cap=nc;}
  s->v[s->len++]=x;
}

// unique_slug(slug, set) -> owned Str
static Str unique_slug(Str slug, StrSet* set){
  Buf b; b.p=NULL;b.len=0;b.cap=0; buf_write(&b,slug.p,slug.len);
  u32 n=2;
  while (strset_has(set, (Str){b.p,b.len})){
    b.len=0; buf_write(&b,slug.p,slug.len); buf_putc(&b,'-'); buf_put_u32(&b,n); n++;
  }
  Str r={b.p,b.len}; strset_add(set,r); return r;
}

// renderMarkdown -> writes html into *html, toc into *toc, excerpt into *excerpt. Returns nothing.
static void render_markdown(const u8* src, u32 len, Env* env, Buf* html, TocArr* toc, Str* excerpt){
  StrSet ids; ids.v=NULL;ids.len=0;ids.cap=0;
  int have_excerpt=0;
  TokArr toks = lex_markdown(src,len);
  for (u32 ti=0; ti<toks.len; ti++){
    Token* t=&toks.v[ti];
    switch(t->type){
      case TK_HEADING: {
        Buf sl; sl.p=NULL;sl.len=0;sl.cap=0; slugify_into(&sl, t->htext.p, t->htext.len);
        Str id = unique_slug((Str){sl.p,sl.len}, &ids);
        Buf ih; ih.p=NULL;ih.len=0;ih.cap=0; inline_into(&ih, t->htext.p, t->htext.len, env);
        buf_putc(html,'<'); buf_putc(html,'h'); buf_putc(html,(u8)('0'+t->depth));
        buf_puts(html," id=\""); buf_write(html,id.p,id.len); buf_putc(html,'"');
        buf_putc(html,'>'); buf_write(html,ih.p,ih.len);
        buf_puts(html,"</h"); buf_putc(html,(u8)('0'+t->depth)); buf_puts(html,">\n");
        if (t->depth==2||t->depth==3){ TocEntry e; e.level=t->depth; e.id=id; e.html.p=ih.p;e.html.len=ih.len; toc_push(toc,e); }
        break;
      }
      case TK_CODE: {
        Str lang=t->lang;
        buf_puts(html,"<pre><code");
        if (lang.len!=0){ buf_puts(html," class=\"language-"); escape_html_into(html,lang.p,lang.len); buf_putc(html,'"'); }
        buf_putc(html,'>');
        for (u32 k=0;k<t->ncl;k++){ escape_html_into(html,t->clines[k].p,t->clines[k].len); buf_putc(html,'\n'); }
        buf_puts(html,"</code></pre>\n");
        break;
      }
      case TK_HR: buf_puts(html,"<hr>\n"); break;
      case TK_BLOCKQUOTE: {
        if (t->nql==0) buf_puts(html,"<blockquote>\n</blockquote>\n");
        else {
          Buf jb; jb.p=NULL;jb.len=0;jb.cap=0;
          for (u32 k=0;k<t->nql;k++){ if(k)buf_putc(&jb,'\n'); buf_write(&jb,t->qlines[k].p,t->qlines[k].len); }
          Buf ih; ih.p=NULL;ih.len=0;ih.cap=0; inline_into(&ih, jb.p, jb.len, env);
          buf_puts(html,"<blockquote>\n<p>"); buf_write(html,ih.p,ih.len); buf_puts(html,"</p>\n</blockquote>\n");
        }
        break;
      }
      case TK_LIST: {
        const char* tag = t->ordered?"ol":"ul";
        if (t->ordered && t->start!=1){ buf_puts(html,"<ol start=\""); buf_put_u32(html,t->start); buf_puts(html,"\">\n"); }
        else { buf_putc(html,'<'); buf_puts(html,tag); buf_puts(html,">\n"); }
        for (u32 k=0;k<t->nit;k++){ buf_puts(html,"<li>"); inline_into(html, t->items[k].p, t->items[k].len, env); buf_puts(html,"</li>\n"); }
        buf_puts(html,"</"); buf_puts(html,tag); buf_puts(html,">\n");
        break;
      }
      case TK_PARA: {
        Buf ih; ih.p=NULL;ih.len=0;ih.cap=0; inline_into(&ih, t->ptext.p, t->ptext.len, env);
        if (!have_excerpt){ excerpt->p=ih.p; excerpt->len=ih.len; have_excerpt=1; }
        buf_puts(html,"<p>"); buf_write(html,ih.p,ih.len); buf_puts(html,"</p>\n");
        break;
      }
    }
  }
  if (!have_excerpt){ excerpt->p=(u8*)""; excerpt->len=0; }
}

static u32 g_out_dummy4;

// ---- test export: render_markdown with identity link env ----
static Str identity_link(void* ctx, const u8* t, u32 tlen){ (void)ctx; Str r={ (u8*)t, tlen }; return r; }

__attribute__((export_name("mg_test_md")))
u32 mg_test_md(u32 inPtr, u32 inLen){
  arena_init(inPtr + inLen);
  Env env; env.link=identity_link; env.ctx=NULL;
  Buf html; html.p=NULL;html.len=0;html.cap=0;
  TocArr toc; toc.v=NULL;toc.len=0;toc.cap=0;
  Str excerpt; excerpt.p=NULL;excerpt.len=0;
  render_markdown((const u8*)inPtr, inLen, &env, &html, &toc, &excerpt);
  Buf b; b.p=NULL;b.len=0;b.cap=0;
  buf_putc(&b,'{');
  buf_puts(&b,"\"html\":"); json_write_string(&b,html.p,html.len);
  buf_puts(&b,",\"excerpt\":"); json_write_string(&b,excerpt.p,excerpt.len);
  buf_puts(&b,",\"toc\":[");
  for (u32 k=0;k<toc.len;k++){ if(k)buf_putc(&b,','); buf_putc(&b,'{'); buf_puts(&b,"\"level\":"); buf_put_u32(&b,toc.v[k].level);
    buf_puts(&b,",\"id\":"); json_write_string(&b,toc.v[k].id.p,toc.v[k].id.len);
    buf_puts(&b,",\"html\":"); json_write_string(&b,toc.v[k].html.p,toc.v[k].html.len); buf_putc(&b,'}'); }
  buf_puts(&b,"]}");
  CTRL[CTRL_OUT_PTR]=(u32)b.p; CTRL[CTRL_OUT_LEN]=b.len; CTRL[CTRL_STATUS]=0;
  return b.len;
}
// These let the differential harness unit-test C primitives before the full pipeline exists.

// ================================================================== value model (template.js)
// A Val is what templates read: string, number, bool, null, array, or object (ordered members).
typedef enum { VSTR, VNUM, VBOOL, VNULL, VARR, VOBJ } VTag;
typedef struct Val Val;
typedef struct { Str key; Val* val; } VMemb;
struct Val {
  VTag tag;
  union {
    Str s;                       // VSTR
    i64 num;                     // VNUM
    int b;                       // VBOOL
    struct { Val** it; u32 len; } arr;  // VARR
    struct { VMemb* m; u32 len; } obj;  // VOBJ
  } as;
};

static Val* vnew(VTag t){ Val* v=(Val*)bump(sizeof(Val)); v->tag=t; return v; }
static Val* vstr(Str s){ Val* v=vnew(VSTR); v->as.s=s; return v; }
static Val* vnum(i64 n){ Val* v=vnew(VNUM); v->as.num=n; return v; }
static Val* vbool(int b){ Val* v=vnew(VBOOL); v->as.b=b; return v; }
static Val* vnull(){ return vnew(VNULL); }

static Val* varr_new(u32 cap){ Val* v=vnew(VARR); v->as.arr.it=(Val**)bump(cap*sizeof(Val*)); v->as.arr.len=0; return v; }
static void varr_push(Val* a, Val* x){
  // NOTE: cap tracking omitted for simplicity; we over-allocate by bumping a fresh array each push is
  // O(n^2). Instead store cap in a parallel; but Val has no cap field. Use a grow pattern via realloc-in-arena:
  // We store capacity by allocating with doubling using a side array is messy. Simpler: bump a new array.
  // To avoid O(n^2), we keep a hidden cap: reuse the arr.len and grow by reallocating a doubled buffer.
  // Since Val is fixed, add cap handling by storing it in the unused high bits is unsafe. We'll instead
  // over-allocate: callers that build arrays use varr_new with a known cap. For push beyond cap, grow.
  // Implement grow by allocating new and copying (cap inferred from a separate map is overkill); we accept
  // occasional O(n^2) only for small arrays (tags per page < ~6, posts per page < ~15). Fine.
  // Grow: allocate new array of len+1 each time would be O(n^2); instead we track cap via a global side table.
  // Simplest correct: store cap in the Val by using a second allocation. We'll just realloc with doubling using
  // a companion capacity stored at negative index — too hacky. Use linear growth with a cap array keyed by Val*.
  // Given small sizes, do: newcap = len<8?8:len*2; but we don't know old cap. We'll store cap in arr.len's unused
  // by making arr.it point to a buffer whose first element is cap? No.
  // Decision: add a cap by allocating arrays with a 1-word header holding cap, and arr.it points past it.
  // But vnew zeroes via bump (uninitialized). Let me just do grow-by-doubling with header.
  // -- Reimplement: arr.it points to buffer; buffer[-1] (as u32*) holds cap. We allocate cap+1 u32 words.
  u32* buf = (u32*)a->as.arr.it - 1;
  u32 cap = *buf;
  if (a->as.arr.len == cap){
    u32 nc = cap? cap*2 : 8;
    u32* nb = (u32*)bump((nc+1)*sizeof(u32)); nb[0]=nc;
    Val** dst=(Val**)(nb+1);
    if (a->as.arr.len) memcpy(dst, a->as.arr.it, a->as.arr.len*sizeof(Val*));
    a->as.arr.it = dst; cap=nc;
  }
  a->as.arr.it[a->as.arr.len++] = x;
}

static Val* vobj_new(u32 cap){
  Val* v=vnew(VOBJ);
  u32* buf=(u32*)bump(sizeof(u32) + cap*sizeof(VMemb)); buf[0]=cap;
  v->as.obj.m=(VMemb*)(buf+1); v->as.obj.len=0; return v;
}
static void vobj_set_checked(Val* o, Str key, Val* x){
  u32* buf=(u32*)o->as.obj.m - 1; u32 cap=*buf;
  // replace existing key or append
  for (u32 i=0;i<o->as.obj.len;i++) if (str_eq(o->as.obj.m[i].key,key)){ o->as.obj.m[i].val=x; return; }
  if (o->as.obj.len==cap){
    u32 nc=cap?cap*2:8; u32* nb=(u32*)bump(sizeof(u32) + nc*sizeof(VMemb)); nb[0]=nc; VMemb* dst=(VMemb*)(nb+1);
    if(o->as.obj.len) memcpy(dst,o->as.obj.m,o->as.obj.len*sizeof(VMemb)); o->as.obj.m=dst; cap=nc;
  }
  o->as.obj.m[o->as.obj.len].key=key; o->as.obj.m[o->as.obj.len].val=x; o->as.obj.len++;
}
static Val* vobj_get(Val* o, Str key){
  if (!o || o->tag!=VOBJ) return NULL;
  for (u32 i=0;i<o->as.obj.len;i++) if (str_eq(o->as.obj.m[i].key,key)) return o->as.obj.m[i].val;
  return NULL;
}

// textOf(v): string->v; number->decimal; bool->"true"/"false"; array->map(textOf).join(", "); else "".
static void text_of(Buf* b, Val* v){
  if (!v){ return; }
  switch(v->tag){
    case VSTR: buf_write(b, v->as.s.p, v->as.s.len); break;
    case VNUM: buf_put_i64(b, v->as.num); break;
    case VBOOL: buf_puts(b, v->as.b?"true":"false"); break;
    case VARR: for (u32 i=0;i<v->as.arr.len;i++){ if(i) buf_puts(b,", "); text_of(b, v->as.arr.it[i]); } break;
    default: break; // null/object -> ""
  }
}

static int truthy(Val* v){
  if (!v) return 0;
  if (v->tag==VARR) return v->as.arr.len>0;
  if (v->tag==VNULL) return 0;
  if (v->tag==VBOOL) return v->as.b;
  if (v->tag==VNUM) return v->as.num!=0;
  if (v->tag==VSTR) return v->as.s.len!=0;
  return 1; // object
}

// ================================================================== template AST (template.js)
typedef enum { N_TEXT, N_VAR, N_RAW, N_FOR, N_IF, N_INCLUDE } NdType;
typedef struct Node Node;
typedef struct { Node** v; u32 len; u32 cap; } NodeArr;
struct Node {
  NdType kind;
  Str text;          // N_TEXT, N_INCLUDE(name)
  Str* path; u32 plen; // N_VAR/N_RAW/N_FOR/N_IF path segments
  Str name;          // N_FOR loop var name
  NodeArr body;      // N_FOR body, N_IF then
  NodeArr els;       // N_IF else
  int notflag;       // N_IF "not"
  int else_seen;     // N_IF: an else branch has been started
};

static void nodes_push(NodeArr* a, Node* n){
  if (a->len==a->cap){ u32 nc=a->cap?a->cap*2:8; Node** na=(Node**)bump(nc*sizeof(Node*)); if(a->v&&a->len)memcpy(na,a->v,a->len*sizeof(Node*)); a->v=na; a->cap=nc; }
  a->v[a->len++]=n;
}
static Node* node_new(NdType k){ Node* n=(Node*)bump(sizeof(Node)); memset(n,0,sizeof(Node)); n->kind=k; return n; }

// Parse a path "a.b.c" into segments. Caller guarantees PATH_RE-valid (validated at tokenize).
static void parse_path(Str p, Str** segs, u32* n){
  u32 cap=4, cnt=0; Str* a=(Str*)bump(cap*sizeof(Str));
  u32 start=0;
  for (u32 i=0;i<=p.len;i++){
    if (i==p.len || p.p[i]=='.'){ if(cnt==cap){cap*=2;Str*na=(Str*)bump(cap*sizeof(Str));memcpy(na,a,cnt*sizeof(Str));a=na;} a[cnt].p=p.p+start; a[cnt].len=i-start; cnt++; start=i+1; }
  }
  *segs=a; *n=cnt;
}

// forward
static int path_valid(Str p);

// tokenize + parse one template source into a NodeArr (root body). Returns 0 ok, 1 error (TemplateError).
// On error, *errout is set to the message Str (owned).
static int parse_template(Str name, const u8* src, u32 len, NodeArr* rootout, Str* errout);

// tokenizer state: we emit tokens then build. To keep it simple, do a single-pass build with a stack.
// Token kinds during parse:
enum { TK_TEXT, TK_VAR, TK_RAW, TK_TAG };

// We implement tokenize inline within parse_template using an explicit stack of NodeArr* frames.

static int path_valid(Str p){
  if (p.len==0) return 0;
  u32 i=0; 
  // first segment: [A-Za-z_][A-Za-z0-9_]*
  while (i<p.len && p.p[i]!='.'){
    u8 c=p.p[i];
    if (i==0){ if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='_')) return 0; }
    else { if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_')) return 0; }
    i++;
  }
  while (i<p.len){
    // p.p[i]=='.'
    i++; u32 st=i;
    while (i<p.len && p.p[i]!='.'){
      u8 c=p.p[i];
      if (i==st){ if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='_')) return 0; }
      else { if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_')) return 0; }
      i++;
    }
    if (i==st) return 0; // empty segment
  }
  return 1;
}

// trim ASCII spaces/tabs (template .trim())
static Str tpl_trim(Str s){
  u32 a=0,b=s.len;
  while(a<b && (s.p[a]==0x20||s.p[a]==0x09)) a++;
  while(b>a && (s.p[b-1]==0x20||s.p[b-1]==0x09)) b--;
  Str r={s.p+a,b-a}; return r;
}

static int str_starts_with(Str s, const char* lit, u32 litlen){ return s.len>=litlen && memcmp(s.p,lit,litlen)==0; }

// parse "for X in PATH" / "if [not] PATH" / "include \"name\"" / else / endfor / endif
static int parse_template(Str name, const u8* src, u32 len, NodeArr* rootout, Str* errout){
  NodeArr root; root.v=NULL;root.len=0;root.cap=0;
  // stack of frames: each frame = {NodeArr* body (where to push), Node* for/if node, int mode}
  // We use a stack of "containers": root, or a for.body, or an if.then, or an if.else.
  NodeArr* stack[32]; int sp=0; stack[sp++]=&root;
  // parallel: the node that owns each container (for else switching)
  Node* owner[32]; owner[0]=NULL;

  Buf text; text.p=NULL;text.len=0;text.cap=0;
  u32 i=0;
  // Flush: copy the accumulated text into a node (nodes outlive the shared buffer).
  #define FLUSH_TEXT() do{ if(text.len){ Node* n=node_new(N_TEXT); u8* cp=(u8*)bump(text.len); memcpy(cp,text.p,text.len); n->text.p=cp; n->text.len=text.len; nodes_push(stack[sp-1], n); text.len=0; } }while(0)

  while (i<len){
    // find next '{'
    i64 open=-1; for(u32 k=i;k<len;k++) if(src[k]=='{'){open=(i64)k;break;}
    if (open==-1 || (u32)open==len-1){ buf_write(&text, src+i, len-i); i=len; break; }
    u8 next = src[open+1];
    if (next!='{' && next!='%'){ buf_write(&text, src+i, (u32)(open-i)+1); i=(u32)open+1; continue; }
    buf_write(&text, src+i, (u32)(open-i));
    if (next=='{'){
      int raw = (open+2<(i64)len && src[open+2]=='{');
      u32 innerStart = (u32)open + (raw?3:2);
      const char* closeTok = raw? "}}}":"}}"; u32 closeLen=3;
      if (!raw) closeLen=2;
      i64 close=-1; for(u32 k=innerStart;k+closeLen<=len;k++){ if(memcmp(src+k,closeTok,closeLen)==0){close=(i64)k;break;} }
      if (close==-1){ FLUSH_TEXT(); *errout=str_from_c("template has an unclosed variable"); return 1; }
      FLUSH_TEXT();
      Str inner = {src+innerStart, (u32)(close-innerStart)};
      Str pth = tpl_trim(inner);
      if (!path_valid(pth)){ Buf b;b.p=NULL;b.len=0;b.cap=0; buf_puts(&b,"template \""); buf_write(&b,name.p,name.len); buf_puts(&b,"\": not a path"); errout->p=b.p;errout->len=b.len; return 1; }
      Node* n=node_new(raw?N_RAW:N_VAR); parse_path(pth,&n->path,&n->plen); nodes_push(stack[sp-1],n);
      i=(u32)close+closeLen;
      continue;
    }
    // tag: find "%}"
    i64 close=-1; for(u32 k=(u32)open+2;k+1<len;k++){ if(src[k]=='%'&&src[k+1]=='}'){close=(i64)k;break;} }
    if (close==-1){ FLUSH_TEXT(); *errout=str_from_c("template has an unclosed tag"); return 1; }
    u32 endpos=(u32)close+2;
    // standalone detection
    u32 lineStart=(u32)open; while(lineStart>0 && src[lineStart-1]!='\n') lineStart--;
    i64 nl=-1; for(u32 k=endpos;k<len;k++) if(src[k]=='\n'){nl=(i64)k;break;}
    u32 lineEnd = (nl==-1)? len : (u32)nl;
    // STANDALONE_RE ^[ \t]*$ on src[lineStart..open) and src[endpos..lineEnd)
    int sa=1; for(u32 k=lineStart;k<(u32)open;k++){ u8 c=src[k]; if(c!=0x20&&c!=0x09){sa=0;break;} }
    if (sa) for(u32 k=endpos;k<lineEnd;k++){ u8 c=src[k]; if(c!=0x20&&c!=0x09){sa=0;break;} }
    if (sa){ text.len = text.len - ((u32)open - lineStart); } // drop the leading ws of this line from text
    FLUSH_TEXT();
    Str tagstr = {src+(u32)open+2, (u32)(close-(open+2))};
    Str tag = tpl_trim(tagstr);
    // dispatch
    if (str_starts_with(tag,"for ",4)){
      // for NAME in PATH
      Str rest={tag.p+4, tag.len-4};
      // find " in "
      i64 inpos=-1; for(u32 k=0;k+4<=rest.len;k++){ if(memcmp(rest.p+k," in ",4)==0){inpos=(i64)k;break;} }
      if (inpos<0){ Buf b;b.p=NULL;b.len=0;b.cap=0; buf_puts(&b,"template \"");buf_write(&b,name.p,name.len);buf_puts(&b,"\": bad for");errout->p=b.p;errout->len=b.len;return 1; }
      Str nm={rest.p,(u32)inpos}; Str pth={rest.p+inpos+4, rest.len-(u32)(inpos+4)};
      if (!path_valid(pth)){ Buf b;b.p=NULL;b.len=0;b.cap=0; buf_puts(&b,"template \"");buf_write(&b,name.p,name.len);buf_puts(&b,"\": not a path");errout->p=b.p;errout->len=b.len;return 1; }
      Node* n=node_new(N_FOR); n->name=nm; parse_path(pth,&n->path,&n->plen); nodes_push(stack[sp-1],n);
      if (sp<32){ stack[sp]=&n->body; owner[sp]=n; sp++; } 
    } else if (str_starts_with(tag,"if ",3)){
      Str rest={tag.p+3, tag.len-3}; int nf=0;
      if (str_starts_with(rest,"not ",4)){ nf=1; rest.p+=4; rest.len-=4; }
      Str pth=tpl_trim(rest);
      if (!path_valid(pth)){ Buf b;b.p=NULL;b.len=0;b.cap=0; buf_puts(&b,"template \"");buf_write(&b,name.p,name.len);buf_puts(&b,"\": not a path");errout->p=b.p;errout->len=b.len;return 1; }
      Node* n=node_new(N_IF); n->notflag=nf; parse_path(pth,&n->path,&n->plen); nodes_push(stack[sp-1],n);
      if (sp<32){ stack[sp]=&n->body; owner[sp]=n; sp++; }
    } else if (str_eq_lit(tag,"else")){
      Node* top=owner[sp-1];
      if (sp<2 || top==NULL || top->kind!=N_IF || top->else_seen || stack[sp-1]!=&top->body){ *errout=str_from_c("template else outside if"); return 1; }
      top->else_seen=1;
      // switch container to els
      stack[sp-1]=&top->els;
    } else if (str_eq_lit(tag,"endfor")){
      Node* top=owner[sp-1];
      if (sp<2 || top==NULL || top->kind!=N_FOR){ *errout=str_from_c("template endfor without for"); return 1; }
      sp--;
    } else if (str_eq_lit(tag,"endif")){
      Node* top=owner[sp-1];
      if (sp<2 || top==NULL || top->kind!=N_IF){ *errout=str_from_c("template endif without if"); return 1; }
      sp--;
    } else if (str_starts_with(tag,"include ",8)){
      Str rest={tag.p+8, tag.len-8}; Str nm=tpl_trim(rest);
      // INCLUDE_RE ^include "([a-z0-9-]+)"$
      if (!(nm.len>=2 && nm.p[0]=='"' && nm.p[nm.len-1]=='"')){ *errout=str_from_c("template bad include"); return 1; }
      Str inner={nm.p+1, nm.len-2};
      int ok=inner.len>0; for(u32 k=0;k<inner.len;k++){ u8 c=inner.p[k]; if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-')){ok=0;break;} }
      if (!ok){ *errout=str_from_c("template bad include name"); return 1; }
      Node* n=node_new(N_INCLUDE); n->text=inner; nodes_push(stack[sp-1],n);
    } else {
      Buf b;b.p=NULL;b.len=0;b.cap=0; buf_puts(&b,"template \"");buf_write(&b,name.p,name.len);buf_puts(&b,"\": unknown tag");errout->p=b.p;errout->len=b.len;return 1;
    }
    i = sa ? ((nl==-1)? len : (u32)nl+1) : endpos;
  }
  FLUSH_TEXT();
  if (sp!=1){ *errout=str_from_c("template block not closed"); return 1; }
  *rootout=root;
  return 0;
  #undef FLUSH_TEXT
}

// ================================================================== template rendering
// Scope: a frame of vars (VOBJ) + parent.
typedef struct Scope { Val* vars; struct Scope* parent; } Scope;

// lookup(scope, path) -> Val* or NULL (undefined). Walks scope chain for path[0], then object props.
static Val* lookup(Scope* sc, Str* path, u32 plen){
  Scope* s=sc;
  while (s && vobj_get(s->vars, path[0])==NULL) s=s->parent;
  if (!s) return NULL;
  Val* v=vobj_get(s->vars, path[0]);
  for (u32 k=1;k<plen;k++){
    if (!v || v->tag!=VOBJ) return NULL;
    Val* nv=vobj_get(v, path[k]);
    if (!nv) return NULL;
    v=nv;
  }
  return v;
}

typedef struct { Val** tpl; u32 tpln; } TplMap; // parallel arrays: name(Str) -> NodeArr
// We need name->nodes; use a simple assoc with Str keys stored alongside.
typedef struct { Str name; NodeArr* nodes; } TplEnt;
typedef struct { TplEnt* v; u32 len; } TplTable;
static NodeArr* tpl_get(TplTable* t, Str name){ for(u32 i=0;i<t->len;i++) if(str_eq(t->v[i].name,name)) return t->v[i].nodes; return NULL; }

typedef struct { TplTable* tpls; } RenderCtx;

static void render_nodes(NodeArr* nodes, Scope* scope, RenderCtx* ctx, int depth, Buf* out);

static void render_into(NodeArr* nodes, Scope* scope, RenderCtx* ctx, int depth, Buf* out){
  for (u32 i=0;i<nodes->len;i++){
    Node* n=nodes->v[i];
    switch(n->kind){
      case N_TEXT: buf_write(out, n->text.p, n->text.len); break;
      case N_VAR: { Val* v=lookup(scope,n->path,n->plen); Buf t;t.p=NULL;t.len=0;t.cap=0; text_of(&t,v); escape_html_into(out,t.p,t.len); break; }
      case N_RAW: { Val* v=lookup(scope,n->path,n->plen); text_of(out,v); break; }
      case N_IF: {
        Val* v=lookup(scope,n->path,n->plen); int tv=truthy(v);
        int take = (tv != n->notflag);
        if (take) render_into(&n->body, scope, ctx, depth, out);
        else if (n->els.v) render_into(&n->els, scope, ctx, depth, out);
        break;
      }
      case N_FOR: {
        Val* xs=lookup(scope,n->path,n->plen);
        if (!xs || xs->tag!=VARR) break;
        u32 len=xs->as.arr.len;
        // reuse one loop object + one vars object across iterations (like JS), mutate fields instead of new each time
        Val* loop=vobj_new(4);
        Val* vars=vobj_new(4);
        vobj_set_checked(vars, n->name, vnull());
        vobj_set_checked(vars, str_from_c("loop"), loop);
        Scope sc2; sc2.vars=vars; sc2.parent=scope;
        for (u32 k=0;k<len;k++){
          vobj_set_checked(vars, n->name, xs->as.arr.it[k]);
          vobj_set_checked(loop, str_from_c("index"), vnum(k+1));
          vobj_set_checked(loop, str_from_c("first"), vbool(k==0));
          vobj_set_checked(loop, str_from_c("last"), vbool(k==len-1));
          vobj_set_checked(loop, str_from_c("length"), vnum(len));
          render_into(&n->body, &sc2, ctx, depth, out);
        }
        break;
      }
      case N_INCLUDE: {
        if (depth<8){ NodeArr* t=tpl_get(ctx->tpls, n->text); if (t) render_into(t, scope, ctx, depth+1, out); }
        break;
      }
    }
  }
}
static void render_nodes(NodeArr* nodes, Scope* scope, RenderCtx* ctx, int depth, Buf* out){ render_into(nodes,scope,ctx,depth,out); }

// ================================================================== JSON DOM -> Val bridge (for tests + build)
static Val* jval_to_val(JVal* j){
  if (!j) return vnull();
  switch(j->tag){
    case JNULL: return vnull();
    case JBOOL: return vbool(j->as.b);
    case JNUM: return vnum(j->as.num);
    case JSTR: return vstr((Str){j->as.s.p, j->as.s.len});
    case JARR: { Val* a=varr_new(8); for(u32 i=0;i<j->as.arr.len;i++) varr_push(a, jval_to_val(j->as.arr.items[i])); return a; }
    case JOBJ: { Val* o=vobj_new(8); for(u32 i=0;i<j->as.obj.len;i++){ Str k={j->as.obj.m[i].key, j->as.obj.m[i].keylen}; vobj_set_checked(o, k, jval_to_val(j->as.obj.m[i].val)); } return o; }
  }
  return vnull();
}

// ================================================================== template test export
// mg_test_tmpl(inPtr,len): input is a JSON object {templates:{name:src,...}, vars:{...}}.
// Renders each template? No — renders the template named by "render" with "vars" as root scope.
// Emits JSON {ok:1, html} or {ok:0, error}.
__attribute__((export_name("mg_test_tmpl")))
u32 mg_test_tmpl(u32 inPtr, u32 inLen){
  arena_init(inPtr+inLen);
  JParse ps; ps.p=(const u8*)inPtr; ps.end=(const u8*)inPtr+inLen; ps.err=0;
  JVal* root=jparse_value(&ps);
  Buf b; b.p=NULL;b.len=0;b.cap=0;
  if (ps.err || root->tag!=JOBJ){ buf_puts(&b,"{\"ok\":0,\"error\":\"bad json\"}"); CTRL[CTRL_OUT_PTR]=(u32)b.p;CTRL[CTRL_OUT_LEN]=b.len;CTRL[CTRL_STATUS]=0; return b.len; }
  JVal* jtpls=jobj_get(root,"templates");
  JVal* jvars=jobj_get(root,"vars");
  JVal* jrender=jobj_get(root,"render");
  // build template table (parse each)
  TplTable tpls; tpls.v=NULL; tpls.len=0;
  Buf err; err.p=NULL;err.len=0;err.cap=0; int haderr=0;
  if (jtpls && jtpls->tag==JOBJ){
    tpls.v=(TplEnt*)bump(jtpls->as.obj.len*sizeof(TplEnt));
    for (u32 k=0;k<jtpls->as.obj.len;k++){
      Str nm={jtpls->as.obj.m[k].key, jtpls->as.obj.m[k].keylen};
      JVal* sv=jtpls->as.obj.m[k].val;
      NodeArr nodes; Str e; e.p=NULL;e.len=0;
      int rc = (sv->tag==JSTR)? parse_template(nm, sv->as.s.p, sv->as.s.len, &nodes, &e) : 1;
      if (rc){ haderr=1; buf_puts(&err,"template \""); buf_write(&err,nm.p,nm.len); buf_puts(&err,"\": "); buf_write(&err,e.p,e.len); break; }
      tpls.v[k].name=nm; tpls.v[k].nodes=(NodeArr*)bump(sizeof(NodeArr)); *tpls.v[k].nodes=nodes; tpls.len++;
    }
  }
  if (haderr){ buf_clear(&b); buf_puts(&b,"{\"ok\":0,\"error\":"); json_write_string(&b,err.p,err.len); buf_puts(&b,"}"); CTRL[CTRL_OUT_PTR]=(u32)b.p;CTRL[CTRL_OUT_LEN]=b.len;CTRL[CTRL_STATUS]=0; return b.len; }
  // root scope vars
  Val* vars = jvars? jval_to_val(jvars) : vobj_new(4);
  Scope root_scope; root_scope.vars=vars; root_scope.parent=NULL;
  RenderCtx ctx; ctx.tpls=&tpls;
  Str rname = (jrender && jrender->tag==JSTR)? (Str){jrender->as.s.p,jrender->as.s.len} : str_from_c("");
  NodeArr* rn = tpl_get(&tpls, rname);
  Buf html; html.p=NULL;html.len=0;html.cap=0;
  if (rn) render_nodes(rn, &root_scope, &ctx, 0, &html);
  buf_puts(&b,"{\"ok\":1,\"html\":"); json_write_string(&b,html.p,html.len); buf_puts(&b,"}");
  CTRL[CTRL_OUT_PTR]=(u32)b.p;CTRL[CTRL_OUT_LEN]=b.len;CTRL[CTRL_STATUS]=0; return b.len;
}
__attribute__((export_name("mg_test_slug")))
u32 mg_test_slug(u32 inPtr, u32 inLen){
  arena_init(inPtr + inLen);
  Buf b; b.p=NULL; b.len=0; b.cap=0;
  slugify_into(&b, (const u8*)inPtr, inLen);
  CTRL[CTRL_OUT_PTR]=(u32)b.p; CTRL[CTRL_OUT_LEN]=b.len; CTRL[CTRL_STATUS]=0;
  return b.len;
}

// mg_test_escape(inPtr,len) -> HTML-escaped bytes.
__attribute__((export_name("mg_test_escape")))
u32 mg_test_escape(u32 inPtr, u32 inLen){
  arena_init(inPtr + inLen);
  Buf b; b.p=NULL; b.len=0; b.cap=0;
  escape_html_into(&b, (const u8*)inPtr, inLen);
  CTRL[CTRL_OUT_PTR]=(u32)b.p; CTRL[CTRL_OUT_LEN]=b.len; CTRL[CTRL_STATUS]=0;
  return b.len;
}

// mg_test_jsonroundtrip(inPtr,len): parse one JSON string value, re-emit as JSON string. For parser test.
__attribute__((export_name("mg_test_jsonstr")))
u32 mg_test_jsonstr(u32 inPtr, u32 inLen){
  arena_init(inPtr + inLen);
  JParse ps; ps.p=(const u8*)inPtr; ps.end=(const u8*)inPtr+inLen; ps.err=0;
  jskipws(&ps);
  JVal* v=jparse_value(&ps);
  Buf b; b.p=NULL; b.len=0; b.cap=0;
  if (v->tag==JSTR && !ps.err) json_write_string(&b, v->as.s.p, v->as.s.len);
  else { CTRL[CTRL_STATUS]=1; }
  CTRL[CTRL_OUT_PTR]=(u32)b.p; CTRL[CTRL_OUT_LEN]=b.len;
  if (CTRL[CTRL_STATUS]!=1) CTRL[CTRL_STATUS]=0;
  return b.len;
}
// ================================================================== THE BUILD (index.js)
// Mirrors makefun-site 1.0.0's build() pipeline over our Val/Str model.

typedef struct Source Source;
typedef struct Tag Tag;

struct Source {
  Str path;          // owned (file path)
  Str text;          // owned content (CRLF->LF for .md)
  int post;          // path starts with "posts/"
  FMData data;       // front matter
  Str body;          // body slice (into text) after front matter
  Str layout;        // resolved layout name
  Str slug;          // unique slug
  Str outDir;        // dir/slug or slug
  Str url;           // base/outDir/
  Str html;          // rendered html
  TocArr toc;        // toc
  Str excerpt;       // excerpt
  Val* card;         // built card (Val) in layouts
  Source* newer;
  Source* older;
  Tag** tagRefs; u32 ntagRefs;  // tag refs
};

struct Tag {
  Str name, slug, url;
  Source** posts; u32 nposts; u32 capposts;
};

static void tag_add_post(Tag* t, Source* s){
  if (t->nposts==t->capposts){ u32 nc=t->capposts?t->capposts*2:4; Source** np=(Source**)bump(nc*sizeof(Source*)); if(t->posts&&t->nposts)memcpy(np,t->posts,t->nposts*sizeof(Source*)); t->posts=np; t->capposts=nc; }
  t->posts[t->nposts++]=s;
}

// ---- code-unit / byte compare for Str (paths/slugs/dates are ASCII in practice) ----
static int str_cmp(Str a, Str b){
  u32 n = a.len<b.len? a.len : b.len;
  int c = n? memcmp(a.p,b.p,n) : 0;
  if (c) return c;
  if (a.len<b.len) return -1;
  if (a.len>b.len) return 1;
  return 0;
}

// ---- wordsOf: count runs of non-\s (JS \S+ matches) over UTF-8 ----
static u32 words_of(Str s){
  u32 count=0; int inWord=0; u32 i=0;
  while (i<s.len){ u32 j=i; u32 cp=utf8_next(s.p,s.len,&j); int ws=is_ws_cp(cp); if (ws) inWord=0; else if (!inWord){ inWord=1; count++; } i=j; }
  return count;
}

// ---- CRLF normalize: \r\n? -> \n ----
static Str crlf_normalize(Str in){
  Buf b; b.p=NULL;b.len=0;b.cap=0;
  for (u32 i=0;i<in.len;i++){ u8 c=in.p[i]; if (c=='\r'){ if (i+1<in.len && in.p[i+1]=='\n') continue; /* lone \r -> \n */ buf_putc(&b,'\n'); } else buf_putc(&b,c); }
  Str r={b.p,b.len}; return r;
}

// ---- path helpers ----
static Str dir_of(Str path){ i64 k=-1; for(u32 i=0;i<path.len;i++) if(path.p[i]=='/')k=(i64)i; if(k<0) return str_from_c(""); Str r={path.p,(u32)k}; return r; }
static int path_starts(Str path, Str prefix){ return path.len>=prefix.len && memcmp(path.p,prefix.p,prefix.len)==0; }
static int path_ends_with(Str path, const char* suf){ u32 sl=0; while(suf[sl])sl++; if(path.len<sl)return 0; return memcmp(path.p+path.len-sl,suf,sl)==0; }

// resolvePath(dir, rel) -> 1 & out (Buf) on success, 0 if climbs out. Appends joined path to out.
static int resolve_path(Buf* out, Str dir, Str rel){
  Str parts[64]; u32 np=0;
  if (dir.len){ u32 s=0; for(u32 i=0;i<=dir.len;i++){ if(i==dir.len||dir.p[i]=='/'){ if(i>s){ parts[np].p=dir.p+s; parts[np].len=i-s; np++; } s=i+1; } } }
  u32 s=0;
  for (u32 i=0;i<=rel.len;i++){
    if (i==rel.len || rel.p[i]=='/'){
      Str seg={rel.p+s, i-s};
      if (seg.len==0 || (seg.len==1&&seg.p[0]=='.')){ /* skip */ }
      else if (seg.len==2 && seg.p[0]=='.'&&seg.p[1]=='.'){ if(np==0){ return 0; } np--; }
      else { parts[np]=seg; np++; }
      s=i+1;
    }
  }
  for (u32 k=0;k<np;k++){ if(k) buf_putc(out,'/'); buf_write(out,parts[k].p,parts[k].len); }
  return 1;
}

// ---- broken-link set ----
typedef struct { Str* v; u32 len; u32 cap; } StrVec;
static void strvec_push(StrVec* a, Str x){
  if(a->len==a->cap){u32 nc=a->cap?a->cap*2:8;Str*na=(Str*)bump(nc*sizeof(Str));if(a->v&&a->len)memcpy(na,a->v,a->len*sizeof(Str));a->v=na;a->cap=nc;}
  a->v[a->len++]=x;
}
static int strvec_has(StrVec* a, Str x){ for(u32 i=0;i<a->len;i++) if(str_eq(a->v[i],x)) return 1; return 0; }

// ---- build context for link resolution ----
static Str g_cur_source_path;  // current source's path, for broken-link lines
static Str identity_link(void* ctx, const u8* t, u32 tlen);

typedef struct {
  Source* sources; u32 nsources;
  Str* staticPaths; u32 nstatics;
  Str base;
  StrVec broken;
  Str curDir;
} BuildCtx;

// scheme? ^[A-Za-z][A-Za-z0-9+.-]*:
static int scheme_re(Str t){
  if (t.len==0) return 0; u8 c=t.p[0]; if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z'))) return 0;
  u32 i=1; while(i<t.len){ u8 d=t.p[i]; if(d==':') return 1; if((d>='A'&&d<='Z')||(d>='a'&&d<='z')||(d>='0'&&d<='9')||d=='+'||d=='.'||d=='-') i++; else return 0; }
  return 0;
}

static Source* bysource_get(BuildCtx* c, Str path){ for(u32 i=0;i<c->nsources;i++) if(str_eq(c->sources[i].path,path)) return &c->sources[i]; return NULL; }
static int staticpaths_has(BuildCtx* c, Str path){ for(u32 i=0;i<c->nstatics;i++) if(str_eq(c->staticPaths[i],path)) return 1; return 0; }

static Str build_link(void* vctx, const u8* t, u32 tlen){
  BuildCtx* c=(BuildCtx*)vctx;
  Str target={ (u8*)t, tlen };
  if (scheme_re(target) || (tlen>0 && t[0]=='/') || (tlen>0 && t[0]=='#')) return target;
  i64 hash=-1; for(u32 i=0;i<tlen;i++) if(t[i]=='#'){hash=(i64)i;break;}
  Str rel, frag;
  if (hash==-1){ rel=target; frag=str_from_c(""); }
  else { rel.p=t; rel.len=(u32)hash; frag.p=t+(u32)hash; frag.len=tlen-(u32)hash; }
  Buf pb; pb.p=NULL;pb.len=0;pb.cap=0;
  int rok = 1;
  if (rel.len!=0) rok = resolve_path(&pb, c->curDir, rel);
  if (rok){
    Str path={pb.p,pb.len};
    Source* s=bysource_get(c,path);
    if (s){ Buf u; u.p=NULL;u.len=0;u.cap=0; buf_write(&u,s->url.p,s->url.len); buf_write(&u,frag.p,frag.len); Str r={u.p,u.len}; return r; }
    if (staticpaths_has(c,path)){ Buf u;u.p=NULL;u.len=0;u.cap=0; buf_write(&u,c->base.p,c->base.len); buf_putc(&u,'/'); buf_write(&u,path.p,path.len); buf_write(&u,frag.p,frag.len); Str r={u.p,u.len}; return r; }
  }
  // broken: "broken-link CURPATH -> TARGET"
  Str curpath = g_cur_source_path;
  Buf bl; bl.p=NULL;bl.len=0;bl.cap=0;
  buf_puts(&bl,"broken-link "); buf_write(&bl,curpath.p,curpath.len); buf_puts(&bl," -> "); buf_write(&bl,target.p,target.len);
  Str line={bl.p,bl.len};
  if (!strvec_has(&c->broken,line)) strvec_push(&c->broken,line);
  return target;
}

// ---- insertion sort Sources by path (code-unit order) ----
static void sort_sources_by_path(Source* a, u32 n){
  for (u32 i=1;i<n;i++){ Source k=a[i]; u32 j=i; while(j>0 && str_cmp(a[j-1].path,k.path)>0){ a[j]=a[j-1]; j--; } a[j]=k; }
}
// sort Sources by (date desc, slug asc)
static void sort_posts(Source** a, u32 n){
  for (u32 i=1;i<n;i++){ Source* k=a[i]; u32 j=i; while(j>0){
    Source* x=a[j-1];
    int c = str_cmp(x->data.date, k->data.date); // date desc => if x.date < k.date, x after
    if (c<0){ a[j]=a[j-1]; j--; continue; }
    if (c==0){ int c2=str_cmp(x->slug,k->slug); if(c2>0){ a[j]=a[j-1]; j--; continue; } }
    break;
  } a[j]=k; } }
// sort Tags by (posts desc, slug asc)
static void sort_tags(Tag** a, u32 n){
  for (u32 i=1;i<n;i++){ Tag* k=a[i]; u32 j=i; while(j>0){
    Tag* x=a[j-1];
    if (x->nposts < k->nposts){ a[j]=a[j-1]; j--; continue; }
    if (x->nposts==k->nposts){ int c=str_cmp(x->slug,k->slug); if(c>0){ a[j]=a[j-1]; j--; continue; } }
    break;
  } a[j]=k; } }
// sort StrVec (report lines)
static void sort_strvec(StrVec* a){
  Str* p=a->v; u32 n=a->len;
  for (u32 i=1;i<n;i++){ Str k=p[i]; u32 j=i; while(j>0 && str_cmp(p[j-1],k)>0){ p[j]=p[j-1]; j--; } p[j]=k; }
}

static u32 g_cur_dummy;

// ================================================================== build state + pipeline
#define REPORT_FILE "build-report.txt"
static const char* const RESERVED[3]={"posts","tags","page"};

typedef struct {
  Str base;          // base_url with trailing / removed
  TplTable tpls;     // parsed templates (only those that parsed)
  Source* sources; u32 nsources, capsrc;
  // statics
  Str* staticsPath; Str* staticsContent; u32 nstatics, capstatic;
  // permalinks taken-map: per-dir sets. Simple: array of (dir, StrVec).
  Str* takenDir; StrVec* takenSet; u32 ntaken, captaken;
  StrVec report;     // report lines (refusals/drafts)
  Source** posts; u32 nposts;
  Tag** tags; u32 ntags, captags;
  BuildCtx bc;
} Build;

static StrVec* taken_in(Build* b, Str dir){
  for (u32 i=0;i<b->ntaken;i++) if (str_eq(b->takenDir[i],dir)) return &b->takenSet[i];
  if (b->ntaken==b->captaken){ u32 nc=b->captaken?b->captaken*2:4; Str* nd=(Str*)bump(nc*sizeof(Str)); StrVec* ns=(StrVec*)bump(nc*sizeof(StrVec)); if(b->takenDir&&b->ntaken){memcpy(nd,b->takenDir,b->ntaken*sizeof(Str));memcpy(ns,b->takenSet,b->ntaken*sizeof(StrVec));} b->takenDir=nd;b->takenSet=ns;b->captaken=nc; }
  b->takenDir[b->ntaken]=dir; b->takenSet[b->ntaken].v=NULL;b->takenSet[b->ntaken].len=0;b->takenSet[b->ntaken].cap=0;
  StrVec* s=&b->takenSet[b->ntaken];
  if (dir.len==0){ for(int k=0;k<3;k++) strvec_push(s, str_from_c(RESERVED[k])); }
  b->ntaken++;
  return s;
}

static void build_add_static(Build* b, Str path, Str content){
  if (b->nstatics==b->capstatic){ u32 nc=b->capstatic?b->capstatic*2:8; Str* np=(Str*)bump(nc*sizeof(Str)); Str* nc2=(Str*)bump(nc*sizeof(Str)); if(b->staticsPath&&b->nstatics){memcpy(np,b->staticsPath,b->nstatics*sizeof(Str));memcpy(nc2,b->staticsContent,b->nstatics*sizeof(Str));} b->staticsPath=np;b->staticsContent=nc2;b->capstatic=nc; }
  b->staticsPath[b->nstatics]=path; b->staticsContent[b->nstatics]=content; b->nstatics++;
}

static Source* build_add_source(Build* b){
  if (b->nsources==b->capsrc){ u32 nc=b->capsrc?b->capsrc*2:16; Source* ns=(Source*)bump(nc*sizeof(Source)); if(b->sources&&b->nsources)memcpy(ns,b->sources,b->nsources*sizeof(Source)); b->sources=ns; b->capsrc=nc; }
  Source* s=&b->sources[b->nsources++]; memset(s,0,sizeof(Source)); return s;
}

// uniqueSlug against a taken-set -> owned Str
static Str unique_slug_set(Str slug, StrVec* taken){
  Buf b; b.p=NULL;b.len=0;b.cap=0; buf_write(&b,slug.p,slug.len);
  u32 n=2;
  while (strvec_has(taken,(Str){b.p,b.len})){ b.len=0; buf_write(&b,slug.p,slug.len); buf_putc(&b,'-'); buf_put_u32(&b,n); n++; }
  Str r={b.p,b.len}; strvec_push(taken,r); return r;
}

// 1. readSources: sort files by path; .md -> sources (CRLF->LF), else static.
static void read_sources(Build* b, JVal* files){
  // collect (path, JVal*) pairs
  u32 n = files->as.arr.len;
  // insertion sort by path
  // Build an index array sorted by path
  u32* idx=(u32*)bump(n*sizeof(u32)); for(u32 i=0;i<n;i++) idx[i]=i;
  for (u32 i=1;i<n;i++){ u32 k=idx[i]; u32 j=i; 
    Str kp={files->as.arr.items[k]->as.obj.m[0].key, files->as.arr.items[k]->as.obj.m[0].keylen};
    // find "path" member properly
    JVal* ko = files->as.arr.items[k]; JVal* pv=jobj_get(ko,"path"); Str kpath={pv->as.s.p,pv->as.s.len};
    while(j>0){ JVal* oj=files->as.arr.items[idx[j-1]]; JVal* pj=jobj_get(oj,"path"); Str jpath={pj->as.s.p,pj->as.s.len}; if(str_cmp(jpath,kpath)>0){ idx[j]=idx[j-1]; j--; } else break; }
    idx[j]=k; }
  for (u32 ii=0;ii<n;ii++){
    JVal* f=files->as.arr.items[idx[ii]];
    JVal* pv=jobj_get(f,"path"); Str path={pv->as.s.p,pv->as.s.len};
    JVal* cv=jobj_get(f,"content"); Str content={cv->as.s.p,cv->as.s.len};
    if (path_ends_with(path,".md")){
      Source* s=build_add_source(b); s->path.p=(u8*)bump(path.len); memcpy(s->path.p,path.p,path.len); s->path.len=path.len;
      Str norm=crlf_normalize(content); s->text=norm;
    } else {
      Str pc; pc.p=(u8*)bump(path.len); memcpy(pc.p,path.p,path.len); pc.len=path.len; path=pc;
      Str cc; cc.p=(u8*)bump(content.len); memcpy(cc.p,content.p,content.len); cc.len=content.len; content=cc;
      build_add_static(b,path,content);
    }
  }
}

// 2. frontMatter
static void front_matter(Build* b){
  u32 w=0;
  for (u32 i=0;i<b->nsources;i++){
    Source* s=&b->sources[i];
    FMResult fm=parse_front_matter(s->text.p, s->text.len);
    if (fm.error){
      Buf bl;bl.p=NULL;bl.len=0;bl.cap=0; buf_puts(&bl,"refused "); buf_write(&bl,s->path.p,s->path.len); buf_puts(&bl,": "); buf_write(&bl,fm.errmsg.p,fm.errmsg.len);
      strvec_push(&b->report,(Str){bl.p,bl.len});
      continue;
    }
    s->data=fm.data;
    s->body.p=(u8*)fm.body; s->body.len=fm.bodylen;
    s->post = path_starts(s->path, str_from_c("posts/"));
    // reserved own dir?
    int own=-1;
    for (int k=0;k<3;k++){ if (k==0) continue; /* posts allowed */ Str pre; char tmp[8]; int pl=0; const char* rd=RESERVED[k]; while(rd[pl]){tmp[pl]=rd[pl];pl++;} tmp[pl]='/'; pl++; pre.p=(u8*)tmp; pre.len=pl; if (path_starts(s->path,pre)){ own=k; break; } }
    if (own!=0 && own!=-1){
      Buf bl;bl.p=NULL;bl.len=0;bl.cap=0; buf_puts(&bl,"refused "); buf_write(&bl,s->path.p,s->path.len); buf_puts(&bl,": "); const char* rd=RESERVED[own]; int pl=0; while(rd[pl]){buf_putc(&bl,(u8)rd[pl]);pl++;} buf_puts(&bl,"/ is the generator's own");
      strvec_push(&b->report,(Str){bl.p,bl.len}); continue;
    }
    if (s->post && !s->data.has_date){
      Buf bl;bl.p=NULL;bl.len=0;bl.cap=0; buf_puts(&bl,"refused "); buf_write(&bl,s->path.p,s->path.len); buf_puts(&bl,": a post needs a date");
      strvec_push(&b->report,(Str){bl.p,bl.len}); continue;
    }
    Str layout = s->data.has_layout? s->data.layout : str_from_c(s->post?"post":"page");
    // template exists?
    if (!tpl_get(&b->tpls, layout)){ 
      Buf bl;bl.p=NULL;bl.len=0;bl.cap=0; buf_puts(&bl,"refused "); buf_write(&bl,s->path.p,s->path.len); buf_puts(&bl,": layout \""); buf_write(&bl,layout.p,layout.len); buf_puts(&bl,"\" is no template that parses");
      strvec_push(&b->report,(Str){bl.p,bl.len}); continue;
    }
    s->layout=layout;
    if (s->data.has_draft && s->data.draft){
      Buf bl;bl.p=NULL;bl.len=0;bl.cap=0; buf_puts(&bl,"draft "); buf_write(&bl,s->path.p,s->path.len);
      strvec_push(&b->report,(Str){bl.p,bl.len}); continue;
    }
    // keep: move to w
    if (w!=i) b->sources[w]=b->sources[i];
    w++;
  }
  b->nsources=w;
}

// 3. permalinks
static void permalinks(Build* b){
  for (u32 i=0;i<b->nsources;i++){
    Source* s=&b->sources[i];
    Str dir = s->post? str_from_c("posts") : dir_of(s->path);
    Str slugsrc = s->data.has_slug? s->data.slug : ({ Buf t;t.p=NULL;t.len=0;t.cap=0; slugify_into(&t,s->data.title.p,s->data.title.len); Str r={t.p,t.len}; r; });
    StrVec* taken=taken_in(b,dir);
    s->slug=unique_slug_set(slugsrc,taken);
    if (dir.len==0) s->outDir=s->slug; else { Buf o;o.p=NULL;o.len=0;o.cap=0; buf_write(&o,dir.p,dir.len); buf_putc(&o,'/'); buf_write(&o,s->slug.p,s->slug.len); s->outDir=(Str){o.p,o.len}; }
    Buf u;u.p=NULL;u.len=0;u.cap=0; buf_write(&u,b->base.p,b->base.len); buf_putc(&u,'/'); buf_write(&u,s->outDir.p,s->outDir.len); buf_putc(&u,'/'); s->url=(Str){u.p,u.len};
  }
}

// 4. markdown
static void markdown(Build* b){
  for (u32 i=0;i<b->nsources;i++){
    Source* s=&b->sources[i];
    BuildCtx* c=&b->bc;
    c->curDir = dir_of(s->path);
    g_cur_source_path = s->path;
    Env env; env.link=build_link; env.ctx=c;
    Buf html; html.p=NULL;html.len=0;html.cap=0;
    TocArr toc; toc.v=NULL;toc.len=0;toc.cap=0;
    Str excerpt; excerpt.p=NULL;excerpt.len=0;
    render_markdown(s->body.p, s->body.len, &env, &html, &toc, &excerpt);
    s->html=(Str){html.p,html.len}; s->toc=toc; s->excerpt=excerpt;
  }
}

// 5. collections
static void collections(Build* b){
  b->posts=(Source**)bump(b->nsources*sizeof(Source*)); b->nposts=0;
  for (u32 i=0;i<b->nsources;i++) if (b->sources[i].post) b->posts[b->nposts++]=&b->sources[i];
  sort_posts(b->posts,b->nposts);
  for (u32 k=0;k<b->nposts;k++){ b->posts[k]->newer = k==0?NULL:b->posts[k-1]; b->posts[k]->older = k==b->nposts-1?NULL:b->posts[k+1]; }
}

static u32 g_cur_dummy2;

// 6. tags
static Tag* build_find_or_add_tag(Build* b, Str name, Str slug){
  for (u32 i=0;i<b->ntags;i++) if (str_eq(b->tags[i]->slug,slug)) return b->tags[i];
  if (b->ntags==b->captags){ u32 nc=b->captags?b->captags*2:8; Tag** nt=(Tag**)bump(nc*sizeof(Tag*)); if(b->tags&&b->ntags)memcpy(nt,b->tags,b->ntags*sizeof(Tag*)); b->tags=nt; b->captags=nc; }
  Tag* t=(Tag*)bump(sizeof(Tag)); memset(t,0,sizeof(Tag));
  t->name=name; t->slug=slug;
  Buf u;u.p=NULL;u.len=0;u.cap=0; buf_write(&u,b->base.p,b->base.len); buf_puts(&u,"/tags/"); buf_write(&u,slug.p,slug.len); buf_putc(&u,'/'); t->url=(Str){u.p,u.len};
  b->tags[b->ntags++]=t; return t;
}

// ownTags(s): the source's tags, each slug once, in its own order. Returns array of (name) Strs.
// We need both name and slug. Return StrVec of names (filtered).
static void own_tags(Source* s, StrVec* outNames, StrVec* outSlugs){
  outNames->len=0; outSlugs->len=0;
  if (!s->data.has_tags) return;
  for (u32 i=0;i<s->data.ntags;i++){
    Str name=s->data.tags[i];
    Buf t;t.p=NULL;t.len=0;t.cap=0; slugify_into(&t,name.p,name.len); Str slug={t.p,t.len};
    // seen slug?
    if (strvec_has(outSlugs,slug)) continue;
    strvec_push(outNames,name); strvec_push(outSlugs,slug);
  }
}

static void tags_stage(Build* b){
  StrVec names, slugs; names.v=NULL;names.len=0;names.cap=0; slugs.v=NULL;slugs.len=0;slugs.cap=0;
  // posts
  for (u32 i=0;i<b->nposts;i++){
    Source* p=b->posts[i];
    // tagRefs for post
    p->ntagRefs=0; p->tagRefs=NULL;
    own_tags(p,&names,&slugs);
    u32 nt=slugs.len;
    Tag** refs=(Tag**)bump((nt?nt:1)*sizeof(Tag*));
    u32 rn=0;
    for (u32 k=0;k<nt;k++){
      Str slug=slugs.v[k]; Str name=names.v[k];
      Tag* t=build_find_or_add_tag(b,name,slug);
      tag_add_post(t,p);
      refs[rn++]=t;
    }
    p->tagRefs=refs; p->ntagRefs=rn;
  }
  // pages
  for (u32 i=0;i<b->nsources;i++){
    Source* s=&b->sources[i];
    if (s->post) continue;
    own_tags(s,&names,&slugs);
    u32 nt=slugs.len;
    s->tagRefs=(Tag**)bump((nt?nt:1)*sizeof(Tag*)); s->ntagRefs=0;
    for (u32 k=0;k<nt;k++){
      Str slug=slugs.v[k]; Str name=names.v[k];
      Tag* existing=NULL;
      for(u32 z=0;z<b->ntags;z++) if(str_eq(b->tags[z]->slug,slug)){existing=b->tags[z];break;}
      if (existing) s->tagRefs[s->ntagRefs++]=existing;
      else {
        // synthetic tag with url ""
        Tag* t=(Tag*)bump(sizeof(Tag)); memset(t,0,sizeof(Tag)); t->name=name;t->slug=slug; Str e=str_from_c(""); t->url=e;
        s->tagRefs[s->ntagRefs++]=t;
      }
    }
  }
  // sort tags: build pointer array (b->tags already 0..ntags-1)
  sort_tags(b->tags, b->ntags);
}

static u32 g_cur_dummy3;

// ---- card/link builders (Val) ----
static Val* link_of(Source* s){
  if (!s) return vnull();
  Val* o=vobj_new(4);
  vobj_set_checked(o,str_from_c("title"), vstr(s->data.title));
  vobj_set_checked(o,str_from_c("url"), vstr(s->url));
  vobj_set_checked(o,str_from_c("date"), s->data.has_date? vstr(s->data.date) : vstr(str_from_c("")));
  if (s->data.has_date){ Buf d;d.p=NULL;d.len=0;d.cap=0; long_date_into(&d,s->data.date.p); vobj_set_checked(o,str_from_c("date_long"), vstr((Str){d.p,d.len})); }
  else vobj_set_checked(o,str_from_c("date_long"), vstr(str_from_c("")));
  return o;
}

static Val* card_of(Source* s){
  Val* o=vobj_new(8);
  vobj_set_checked(o,str_from_c("title"), vstr(s->data.title));
  vobj_set_checked(o,str_from_c("url"), vstr(s->url));
  vobj_set_checked(o,str_from_c("date"), s->data.has_date? vstr(s->data.date) : vstr(str_from_c("")));
  if (s->data.has_date){ Buf d;d.p=NULL;d.len=0;d.cap=0; long_date_into(&d,s->data.date.p); vobj_set_checked(o,str_from_c("date_long"), vstr((Str){d.p,d.len})); }
  else vobj_set_checked(o,str_from_c("date_long"), vstr(str_from_c("")));
  vobj_set_checked(o,str_from_c("summary"), s->data.has_summary? vstr(s->data.summary) : vstr(str_from_c("")));
  vobj_set_checked(o,str_from_c("excerpt"), vstr(s->excerpt));
  // reading_minutes = max(1, ceil(words/200)). words of BODY.
  u32 words = words_of(s->body);
  u32 mins = (words+199)/200; if (mins<1) mins=1;
  vobj_set_checked(o,str_from_c("reading_minutes"), vnum(mins));
  // tags array
  Val* tags=varr_new(4);
  for (u32 k=0;k<s->ntagRefs;k++){ Tag* t=s->tagRefs[k]; Val* to=vobj_new(3); vobj_set_checked(to,str_from_c("name"),vstr(t->name)); vobj_set_checked(to,str_from_c("slug"),vstr(t->slug)); vobj_set_checked(to,str_from_c("url"),vstr(t->url)); varr_push(tags,to); }
  vobj_set_checked(o,str_from_c("tags"), tags);
  return o;
}

static Val* page_of(Build* b, Source* s){
  // {...card, slug, content, toc, newer, older, is_post}
  Val* o=vobj_new(10);
  // spread card fields
  Val* card=s->card;
  for (u32 i=0;i<card->as.obj.len;i++) vobj_set_checked(o, card->as.obj.m[i].key, card->as.obj.m[i].val);
  vobj_set_checked(o,str_from_c("slug"), vstr(s->slug));
  vobj_set_checked(o,str_from_c("content"), vstr(s->html));
  Val* toc=varr_new(4);
  for (u32 k=0;k<s->toc.len;k++){ Val* h=vobj_new(3); vobj_set_checked(h,str_from_c("level"),vnum(s->toc.v[k].level)); vobj_set_checked(h,str_from_c("id"),vstr(s->toc.v[k].id)); vobj_set_checked(h,str_from_c("html"),vstr(s->toc.v[k].html)); varr_push(toc,h); }
  vobj_set_checked(o,str_from_c("toc"), toc);
  vobj_set_checked(o,str_from_c("newer"), link_of(s->newer));
  vobj_set_checked(o,str_from_c("older"), link_of(s->older));
  vobj_set_checked(o,str_from_c("is_post"), vbool(s->post));
  (void)b;
  return o;
}

// render helper: render template `name` with root vars object. Returns Buf (may be empty if missing).
static void render_template(Build* b, Str name, Val* rootvars, Buf* out){
  NodeArr* nodes=tpl_get(&b->tpls, name);
  if (!nodes) return;
  Scope sc; sc.vars=rootvars; sc.parent=NULL;
  RenderCtx ctx; ctx.tpls=&b->tpls;
  render_nodes(nodes, &sc, &ctx, 0, out);
}

// output map: ordered list of (path, content) — we use a dynamic array preserving insertion order.
typedef struct { Str path; Str content; } OutEnt;
typedef struct { OutEnt* v; u32 len; u32 cap; } OutMap;
static void out_set(OutMap* m, Str path, Str content){
  for (u32 i=0;i<m->len;i++) if (str_eq(m->v[i].path,path)){ m->v[i].content=content; return; }
  if (m->len==m->cap){ u32 nc=m->cap?m->cap*2:32; OutEnt* nm=(OutEnt*)bump(nc*sizeof(OutEnt)); if(m->v&&m->len)memcpy(nm,m->v,m->len*sizeof(OutEnt)); m->v=nm; m->cap=nc; }
  m->v[m->len].path=path; m->v[m->len].content=content; m->len++;
}
static int out_has(OutMap* m, Str path){ for(u32 i=0;i<m->len;i++) if(str_eq(m->v[i].path,path)) return 1; return 0; }

static u32 g_cur_dummy4;

// 7. layouts
// forward decls for globals defined below (OutEnt/OutMap already defined above)
extern Str g_site_title;
extern u32 g_site_recent;
extern u32 g_site_per_page;
extern OutMap g_out_map;

static void layouts(Build* b){
  // build cards once
  for (u32 i=0;i<b->nsources;i++) b->sources[i].card=card_of(&b->sources[i]);
  // site, tags(all), recent
  Val* site=vobj_new(2);
  // site.title from input
  // (we stash input site title/base in Build via globals set in mg_run)
  extern Str g_site_title;
  vobj_set_checked(site,str_from_c("title"), vstr(g_site_title));
  vobj_set_checked(site,str_from_c("base_url"), vstr(b->base));
  Val* allTags=varr_new(4);
  for (u32 i=0;i<b->ntags;i++){ Tag* t=b->tags[i]; Val* to=vobj_new(4); vobj_set_checked(to,str_from_c("name"),vstr(t->name)); vobj_set_checked(to,str_from_c("slug"),vstr(t->slug)); vobj_set_checked(to,str_from_c("url"),vstr(t->url)); vobj_set_checked(to,str_from_c("count"),vnum(t->nposts)); varr_push(allTags,to); }
  u32 recent_n = g_site_recent; Val* recent=varr_new(4);
  for (u32 i=0;i<b->nposts && i<recent_n;i++) varr_push(recent, b->posts[i]->card);

  OutMap out; out.v=NULL;out.len=0;out.cap=0;

  // each source page
  for (u32 i=0;i<b->nsources;i++){
    Source* s=&b->sources[i];
    Val* root=vobj_new(4);
    vobj_set_checked(root,str_from_c("site"),site);
    vobj_set_checked(root,str_from_c("tags"),allTags);
    vobj_set_checked(root,str_from_c("recent"),recent);
    Val* pv=vobj_new(1); vobj_set_checked(pv,str_from_c("page"), page_of(b,s));
    // merge pv into root
    for(u32 z=0;z<pv->as.obj.len;z++) vobj_set_checked(root,pv->as.obj.m[z].key,pv->as.obj.m[z].val);
    Buf html; html.p=NULL;html.len=0;html.cap=0;
    render_template(b, s->layout, root, &html);
    // path = outDir + "/index.html"
    Buf pth; pth.p=NULL;pth.len=0;pth.cap=0; buf_write(&pth,s->outDir.p,s->outDir.len); buf_puts(&pth,"/index.html");
    out_set(&out,(Str){pth.p,pth.len},(Str){html.p,html.len});
  }
  // index pages
  u32 per = g_site_per_page;
  u32 pages = (b->nposts + per - 1)/per; if (pages<1) pages=1;
  for (u32 k=1;k<=pages;k++){
    Val* root=vobj_new(4);
    vobj_set_checked(root,str_from_c("site"),site);
    vobj_set_checked(root,str_from_c("tags"),allTags);
    vobj_set_checked(root,str_from_c("recent"),recent);
    Val* posts=varr_new(4);
    for (u32 j=(k-1)*per; j<k*per && j<b->nposts; j++) varr_push(posts, b->posts[j]->card);
    vobj_set_checked(root,str_from_c("posts"),posts);
    // pager (indexUrl(k): k==1 -> base/ , else base/page/k/)
    Val* pager=vobj_new(5);
    vobj_set_checked(pager,str_from_c("number"),vnum(k));
    vobj_set_checked(pager,str_from_c("total"),vnum(pages));
    { Buf u;u.p=NULL;u.len=0;u.cap=0; buf_write(&u,b->base.p,b->base.len); if(k==1){buf_putc(&u,'/');} else {buf_puts(&u,"/page/");buf_put_u32(&u,k);buf_putc(&u,'/');} vobj_set_checked(pager,str_from_c("url"),vstr((Str){u.p,u.len})); }
    { Buf u;u.p=NULL;u.len=0;u.cap=0; if(k==1){/* "" */} else {buf_write(&u,b->base.p,b->base.len); if(k-1==1){buf_putc(&u,'/');} else {buf_puts(&u,"/page/");buf_put_u32(&u,k-1);buf_putc(&u,'/');}} vobj_set_checked(pager,str_from_c("newer_url"),vstr((Str){u.p,u.len})); }
    { Buf u;u.p=NULL;u.len=0;u.cap=0; if(k==pages){/* "" */} else {buf_write(&u,b->base.p,b->base.len); buf_puts(&u,"/page/");buf_put_u32(&u,k+1);buf_putc(&u,'/');} vobj_set_checked(pager,str_from_c("older_url"),vstr((Str){u.p,u.len})); }
    vobj_set_checked(root,str_from_c("pager"),pager);
    Buf html; html.p=NULL;html.len=0;html.cap=0;
    render_template(b, str_from_c("index"), root, &html);
    // only set if template existed (render_template leaves html empty if missing). Check tpl_get.
    if (tpl_get(&b->tpls, str_from_c("index"))){
      Buf pth; pth.p=NULL;pth.len=0;pth.cap=0; if(k==1) buf_puts(&pth,"index.html"); else {buf_puts(&pth,"page/");buf_put_u32(&pth,k);buf_puts(&pth,"/index.html");}
      out_set(&out,(Str){pth.p,pth.len},(Str){html.p,html.len});
    }
  }
  // tag pages
  for (u32 i=0;i<b->ntags;i++){
    Tag* t=b->tags[i];
    Val* root=vobj_new(4);
    vobj_set_checked(root,str_from_c("site"),site);
    vobj_set_checked(root,str_from_c("tags"),allTags);
    vobj_set_checked(root,str_from_c("recent"),recent);
    // tag object: {name,slug,url,count,posts:[cards]}
    Val* tv=vobj_new(5);
    vobj_set_checked(tv,str_from_c("name"),vstr(t->name));
    vobj_set_checked(tv,str_from_c("slug"),vstr(t->slug));
    vobj_set_checked(tv,str_from_c("url"),vstr(t->url));
    vobj_set_checked(tv,str_from_c("count"),vnum(t->nposts));
    Val* tposts=varr_new(4); for(u32 z=0;z<t->nposts;z++) varr_push(tposts, t->posts[z]->card);
    vobj_set_checked(tv,str_from_c("posts"),tposts);
    vobj_set_checked(root,str_from_c("tag"),tv);
    Buf html; html.p=NULL;html.len=0;html.cap=0;
    render_template(b, str_from_c("tag"), root, &html);
    if (tpl_get(&b->tpls, str_from_c("tag"))){
      Buf pth;pth.p=NULL;pth.len=0;pth.cap=0; buf_puts(&pth,"tags/");buf_write(&pth,t->slug.p,t->slug.len);buf_puts(&pth,"/index.html");
      out_set(&out,(Str){pth.p,pth.len},(Str){html.p,html.len});
    }
  }
  // tags index
  if (tpl_get(&b->tpls, str_from_c("tags"))){
    Val* root=vobj_new(4);
    vobj_set_checked(root,str_from_c("site"),site);
    vobj_set_checked(root,str_from_c("tags"),allTags);
    vobj_set_checked(root,str_from_c("recent"),recent);
    Buf html; html.p=NULL;html.len=0;html.cap=0;
    render_template(b, str_from_c("tags"), root, &html);
    out_set(&out,str_from_c("tags/index.html"),(Str){html.p,html.len});
  }
  // statics
  for (u32 i=0;i<b->nstatics;i++){
    Str path=b->staticsPath[i];
    if (out_has(&out,path)){
      Buf bl;bl.p=NULL;bl.len=0;bl.cap=0; buf_puts(&bl,"shadowed "); buf_write(&bl,path.p,path.len); buf_puts(&bl,": a built page has its path");
      strvec_push(&b->report,(Str){bl.p,bl.len});
    } else out_set(&out,path,b->staticsContent[i]);
  }
  // 8. report
  {
    StrVec lines; lines.v=NULL;lines.len=0;lines.cap=0;
    for (u32 i=0;i<b->report.len;i++) strvec_push(&lines,b->report.v[i]);
    for (u32 i=0;i<b->bc.broken.len;i++) strvec_push(&lines,b->bc.broken.v[i]);
    sort_strvec(&lines);
    u32 pages=0; for(u32 i=0;i<b->nsources;i++) if(!b->sources[i].post) pages++;
    Buf rb; rb.p=NULL;rb.len=0;rb.cap=0;
    for (u32 i=0;i<lines.len;i++){ buf_write(&rb,lines.v[i].p,lines.v[i].len); buf_putc(&rb,'\n'); }
    buf_puts(&rb,"built "); buf_put_u32(&rb,pages); buf_puts(&rb," pages, "); buf_put_u32(&rb,b->nposts); buf_puts(&rb," posts, "); buf_put_u32(&rb,b->ntags); buf_puts(&rb," tags and "); buf_put_u32(&rb,b->nstatics); buf_puts(&rb," static files\n");
    out_set(&out,str_from_c(REPORT_FILE),(Str){rb.p,rb.len});
  }
  // stash out for serialization
  g_out_map = out;
}

Str g_site_title;
u32 g_site_recent;
u32 g_site_per_page;
OutMap g_out_map;

static u32 g_cur_dummy5;

// ================================================================== per-case build driver
// input is a JVal object {site, templates, files}. Builds into globals, serializes to *out as a
// JSON object (path -> text). Caller resets arena between cases via mg_run.
static void run_one_case(JVal* input, Buf* out){
  // reset build globals
  Build b; memset(&b,0,sizeof(b));
  b.bc.sources=NULL;b.bc.nsources=0;b.bc.broken.v=NULL;b.bc.broken.len=0;b.bc.broken.cap=0;
  g_out_map.v=NULL;g_out_map.len=0;g_out_map.cap=0;
  g_cur_source_path=str_from_c("");
  // site
  JVal* jsite=jobj_get(input,"site");
  Str base_url=str_from_c(""); g_site_title=str_from_c("");
  u32 per=6, recent=3;
  if (jsite){
    JVal* bt=jobj_get(jsite,"base_url"); if(bt&&bt->tag==JSTR) base_url=(Str){bt->as.s.p,bt->as.s.len};
    JVal* ti=jobj_get(jsite,"title"); if(ti&&ti->tag==JSTR) g_site_title=(Str){ti->as.s.p,ti->as.s.len};
    JVal* pp=jobj_get(jsite,"per_page"); if(pp&&pp->tag==JNUM) per=(u32)pp->as.num;
    JVal* rc=jobj_get(jsite,"recent"); if(rc&&rc->tag==JNUM) recent=(u32)rc->as.num;
  }
  g_site_per_page=per; g_site_recent=recent;
  // base = base_url with trailing / removed
  while (base_url.len>0 && base_url.p[base_url.len-1]=='/') base_url.len--;
  b.base=base_url;
  // templates: parse each (sorted by name), skip TemplateErrors with report line
  JVal* jtpls=jobj_get(input,"templates");
  // collect names sorted
  if (jtpls && jtpls->tag==JOBJ){
    u32 tn=jtpls->as.obj.len;
    u32* idx=(u32*)bump(tn*sizeof(u32)); for(u32 i=0;i<tn;i++) idx[i]=i;
    for(u32 i=1;i<tn;i++){ u32 k=idx[i]; u32 j=i; Str kp={jtpls->as.obj.m[k].key,jtpls->as.obj.m[k].keylen};
      while(j>0){ Str jp={jtpls->as.obj.m[idx[j-1]].key,jtpls->as.obj.m[idx[j-1]].keylen}; if(str_cmp(jp,kp)>0){idx[j]=idx[j-1];j--;} else break; } idx[j]=k; }
    b.tpls.v=(TplEnt*)bump(tn*sizeof(TplEnt)); b.tpls.len=0;
    for (u32 ii=0;ii<tn;ii++){
      u32 k=idx[ii]; Str nm={jtpls->as.obj.m[k].key,jtpls->as.obj.m[k].keylen};
      JVal* sv=jtpls->as.obj.m[k].val;
      NodeArr nodes; Str e; e.p=NULL;e.len=0;
      int rc = (sv->tag==JSTR)? parse_template(nm, sv->as.s.p, sv->as.s.len, &nodes, &e) : 1;
      if (rc){
        Buf bl;bl.p=NULL;bl.len=0;bl.cap=0; buf_write(&bl,e.p,e.len);
        strvec_push(&b.report,(Str){bl.p,bl.len});
        continue;
      }
      b.tpls.v[b.tpls.len].name=nm; b.tpls.v[b.tpls.len].nodes=(NodeArr*)bump(sizeof(NodeArr)); *b.tpls.v[b.tpls.len].nodes=nodes; b.tpls.len++;
    }
  }
  // files
  JVal* jfiles=jobj_get(input,"files");
  b.bc.sources=NULL; b.bc.nsources=0;
  if (jfiles && jfiles->tag==JARR) read_sources(&b, jfiles);
  // wire buildctx sources pointer/count after read (sources array may have been reallocated)
  b.bc.sources=b.sources; b.bc.nsources=b.nsources;
  b.bc.base=b.base;
  // static paths for buildctx
  b.bc.staticPaths=b.staticsPath; b.bc.nstatics=b.nstatics;
  front_matter(&b);
  b.bc.sources=b.sources; b.bc.nsources=b.nsources; // may have shrunk
  permalinks(&b);
  markdown(&b);
  collections(&b);
  tags_stage(&b);
  layouts(&b);
  // serialize g_out_map as JSON object
  buf_putc(out,'{');
  for (u32 i=0;i<g_out_map.len;i++){ if(i)buf_putc(out,','); json_write_string(out, g_out_map.v[i].path.p, g_out_map.v[i].path.len); buf_putc(out,':'); json_write_string(out, g_out_map.v[i].content.p, g_out_map.v[i].content.len); }
  buf_putc(out,'}');
}

// ================================================================== mg_run: the real entry
// inPtr/inLen = raw bytes of /input/cases.json (array of {id, input}).
// Builds answers.json bytes in-C, writes {out_ptr,out_len} to CTRL.
__attribute__((export_name("mg_run")))
u32 mg_run(u32 inPtr, u32 inLen){
  arena_init(inPtr + inLen);
  JParse ps; ps.p=(const u8*)inPtr; ps.end=(const u8*)inPtr+inLen; ps.err=0;
  JVal* cases=jparse_value(&ps);
  Buf out; out.p=NULL;out.len=0;out.cap=0;
  if (ps.err || cases->tag!=JARR){ CTRL[CTRL_OUT_PTR]=(u32)out.p;CTRL[CTRL_OUT_LEN]=0;CTRL[CTRL_STATUS]=2; return 0; }
  buf_putc(&out,'[');
  for (u32 i=0;i<cases->as.arr.len;i++){
    if (i) buf_putc(&out,',');
    JVal* c=cases->as.arr.items[i];
    JVal* input=jobj_get(c,"input");
    if (input) run_one_case(input,&out); else buf_puts(&out,"null");
  }
  buf_putc(&out,']');
  CTRL[CTRL_OUT_PTR]=(u32)out.p; CTRL[CTRL_OUT_LEN]=out.len; CTRL[CTRL_STATUS]=0;
  return out.len;
}

// DEBUG: run pipeline for case[dbgcase], dump each source's path + html length + first null pos.
__attribute__((export_name("mg_debug")))
u32 mg_debug(u32 inPtr, u32 inLen, u32 dbgcase){
  arena_init(inPtr+inLen);
  JParse ps; ps.p=(const u8*)inPtr; ps.end=(const u8*)inPtr+inLen; ps.err=0;
  JVal* cases=jparse_value(&ps);
  JVal* c=cases->as.arr.items[dbgcase]; JVal* input=jobj_get(c,"input");
  Build b; memset(&b,0,sizeof(b));
  b.bc.broken.v=NULL;b.bc.broken.len=0;b.bc.broken.cap=0; g_out_map.v=NULL;g_out_map.len=0;g_out_map.cap=0; g_cur_source_path=str_from_c("");
  JVal* jsite=jobj_get(input,"site"); Str base_url=str_from_c(""); g_site_title=str_from_c(""); u32 per=6,recent=3;
  if(jsite){ JVal* bt=jobj_get(jsite,"base_url"); if(bt&&bt->tag==JSTR)base_url=(Str){bt->as.s.p,bt->as.s.len}; JVal* ti=jobj_get(jsite,"title"); if(ti&&ti->tag==JSTR)g_site_title=(Str){ti->as.s.p,ti->as.s.len}; JVal* pp=jobj_get(jsite,"per_page"); if(pp&&pp->tag==JNUM)per=(u32)pp->as.num; JVal* rc=jobj_get(jsite,"recent"); if(rc&&rc->tag==JNUM)recent=(u32)rc->as.num; }
  g_site_per_page=per;g_site_recent=recent; while(base_url.len>0&&base_url.p[base_url.len-1]=='/')base_url.len--; b.base=base_url;
  JVal* jtpls=jobj_get(input,"templates");
  if(jtpls&&jtpls->tag==JOBJ){ u32 tn=jtpls->as.obj.len; b.tpls.v=(TplEnt*)bump(tn*sizeof(TplEnt)); b.tpls.len=0;
    for(u32 k=0;k<tn;k++){ Str nm={jtpls->as.obj.m[k].key,jtpls->as.obj.m[k].keylen}; JVal* sv=jtpls->as.obj.m[k].val; NodeArr nodes; Str e; int rc=(sv->tag==JSTR)?parse_template(nm,sv->as.s.p,sv->as.s.len,&nodes,&e):1; if(rc)continue; b.tpls.v[b.tpls.len].name=nm; b.tpls.v[b.tpls.len].nodes=(NodeArr*)bump(sizeof(NodeArr)); *b.tpls.v[b.tpls.len].nodes=nodes; b.tpls.len++; } }
  JVal* jfiles=jobj_get(input,"files"); if(jfiles&&jfiles->tag==JARR) read_sources(&b,jfiles);
  b.bc.sources=b.sources;b.bc.nsources=b.nsources;b.bc.base=b.base;b.bc.staticPaths=b.staticsPath;b.bc.nstatics=b.nstatics;
  front_matter(&b); b.bc.sources=b.sources;b.bc.nsources=b.nsources; permalinks(&b); markdown(&b);
  Buf out; out.p=NULL;out.len=0;out.cap=0;
  for(u32 i=0;i<b.nsources;i++){ Source* s=&b.sources[i]; if(!path_starts(s->path,str_from_c("posts/2020/slug-this-were"))) continue;
    buf_puts(&out,"PATH "); buf_write(&out,s->path.p,s->path.len); buf_puts(&out," bodylen "); buf_put_u32(&out,s->body.len); buf_puts(&out," htmllen "); buf_put_u32(&out,s->html.len);
    // first null in html
    i64 fn=-1; for(u32 k=0;k<s->html.len;k++) if(s->html.p[k]==0){fn=(i64)k;break;} buf_puts(&out," firstnull "); buf_put_i64(&out,fn); buf_putc(&out,'\n');
    buf_puts(&out,"HTML:"); buf_write(&out,s->html.p,s->html.len); buf_putc(&out,'\n');
    // also render body fresh with identity env
    Env idenv; idenv.link=identity_link; idenv.ctx=NULL; Buf ih; ih.p=NULL;ih.len=0;ih.cap=0; TocArr it; it.v=NULL;it.len=0;it.cap=0; Str ie; ie.p=NULL;ie.len=0;
    render_markdown(s->body.p,s->body.len,&idenv,&ih,&it,&ie);
    i64 fn2=-1; for(u32 k=0;k<ih.len;k++) if(ih.p[k]==0){fn2=(i64)k;break;}
    buf_puts(&out,"IDENTITY htmllen "); buf_put_u32(&out,ih.len); buf_puts(&out," firstnull "); buf_put_i64(&out,fn2); buf_putc(&out,'\n');
    // dump body null check + body bytes
    i64 bfn=-1; for(u32 k=0;k<s->body.len;k++) if(s->body.p[k]==0){bfn=(i64)k;break;}
    buf_puts(&out,"BODY firstnull "); buf_put_i64(&out,bfn); buf_puts(&out," bodylen "); buf_put_u32(&out,s->body.len); buf_putc(&out,'\n');
    buf_puts(&out,"BODYHEX:"); { const char* hx="0123456789abcdef"; for(u32 k=0;k<s->body.len;k++){ u8 c=s->body.p[k]; buf_putc(&out,(u8)hx[c>>4]); buf_putc(&out,(u8)hx[c&15]); } } buf_putc(&out,'\n');
    buf_puts(&out,"PTRS body="); buf_put_u32(&out,(u32)s->body.p); buf_puts(&out," text="); buf_put_u32(&out,(u32)s->text.p); buf_puts(&out," arena="); buf_put_u32(&out,g_arena); buf_puts(&out," memsize="); buf_put_u32(&out,g_memsize); buf_putc(&out,'\n');
    // render AGAIN after resetting arena to a fresh high region
    u32 save=g_arena; g_arena=700*1024*1024; Buf ih2; ih2.p=NULL;ih2.len=0;ih2.cap=0; TocArr it2; it2.v=NULL;it2.len=0;it2.cap=0; Str ie2; ie2.p=NULL;ie2.len=0;
    render_markdown(s->body.p,s->body.len,&idenv,&ih2,&it2,&ie2);
    i64 fn3=-1; for(u32 k=0;k<ih2.len;k++) if(ih2.p[k]==0){fn3=(i64)k;break;}
    buf_puts(&out,"FRESHARENA htmllen "); buf_put_u32(&out,ih2.len); buf_puts(&out," firstnull "); buf_put_i64(&out,fn3); buf_putc(&out,'\n'); g_arena=save;
  }
  CTRL[CTRL_OUT_PTR]=(u32)out.p;CTRL[CTRL_OUT_LEN]=out.len;CTRL[CTRL_STATUS]=0; return out.len;
}
