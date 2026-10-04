/*
** reclaim_test.c -- proves pocketparse_parser_free reclaims (W-explore-4). Parses
** the SAME large file many times in ONE engine, freeing each parser, and checks
** peak RSS stays bounded -- i.e. the arena returns each parse's memory to the pool
** for the next, instead of accreting every tree until it exhausts RAM.
**
** Before the fix the parser shared the engine arena and parser_free reclaimed
** nothing, so N parses cost ~N trees of memory; the React benchmark had to re-open
** the engine per file to avoid OOM. With per-parser child arenas reset on free, N
** parses cost ~one tree regardless of N.
**
** Usage: reclaim_test <grammar.ppg> <file> <iterations> <ceiling_kb>
** Exit 0 iff all iterations parse and peak RSS <= ceiling_kb.
*/
#include "bench_common.h"
#include "pocketparse.h"

#define GRANT (8u * 1024u * 1024u)
static void  **blocks; static size_t nblocks, capblocks;
static void track(void *p){ if(!p)return; if(nblocks==capblocks){size_t nc=capblocks?capblocks*2:256; void**g=realloc(blocks,nc*sizeof*g); if(!g)return; blocks=g; capblocks=nc;} blocks[nblocks++]=p; }
static void *grow(void *u,size_t n,size_t *got){ void*p=malloc(n); (void)u; if(got)*got=n; track(p); return p; }
static void freeall(void){ size_t i; for(i=0;i<nblocks;i++) free(blocks[i]); nblocks=0; }

static pocketparse_string text(const char*s){ pocketparse_string t; t.pointer=(const uint8_t*)s; t.length=(uint32_t)strlen(s); return t; }
static int cls_alpha(void*u,uint32_t b){(void)u;return (b>='A'&&b<='Z')||(b>='a'&&b<='z');}
static int cls_digit(void*u,uint32_t b){(void)u;return b>='0'&&b<='9';}
static int cls_alnum(void*u,uint32_t b){return cls_alpha(u,b)||cls_digit(u,b);}
static int cls_space(void*u,uint32_t b){(void)u;return b==' '||b=='\t'||b=='\n'||b=='\r';}
static void bindc(pocketparse_engine*e,const char*n,pocketparse_class_fn f){ pocketparse_binding b; b.name=text(n); b.kind=POCKETPARSE_BIND_CLASS; b.function=(void*)f; b.user=NULL; pocketparse_bind(e,&b); }

int main(int argc, char **argv)
{
    char *gbytes, *fbytes; size_t glen, flen, iters, i;
    pocketparse_configuration cfg; pocketparse_engine *engine;
    pocketparse_grammar *grammar = NULL; pocketparse_source gsrc;
    void *block; long ceiling;

    if (argc < 5) { fprintf(stderr,"usage: %s <grammar.ppg> <file> <iters> <ceiling_kb>\n",argv[0]); return 2; }
    gbytes = bench_read_file(argv[1], &glen);
    fbytes = bench_read_file(argv[2], &flen);
    iters  = (size_t)strtoul(argv[3], NULL, 10);
    ceiling = strtol(argv[4], NULL, 10);
    if (!gbytes || !fbytes || iters == 0) { fprintf(stderr,"reclaim: bad args/inputs\n"); return 2; }

    /* ONE engine for the whole run -- the point of the test. */
    block = malloc(GRANT); track(block);
    memset(&cfg,0,sizeof cfg);
    cfg.memory.base=block; cfg.memory.size=GRANT; cfg.memory.grow=grow; cfg.memory.user=NULL; cfg.memory.chunk_minimum=0; cfg.flags=0;
    engine = pocketparse_open(cfg);
    if (!engine) { fprintf(stderr,"reclaim: open failed\n"); return 2; }
    bindc(engine,"alpha",cls_alpha); bindc(engine,"digit",cls_digit); bindc(engine,"alnum",cls_alnum); bindc(engine,"space",cls_space);
    gsrc.name=text("std.tsx.ppg"); gsrc.bytes.pointer=(const uint8_t*)gbytes; gsrc.bytes.length=(uint32_t)glen;
    if (pocketparse_load_binary_grammar(engine,gsrc,&grammar)!=POCKETPARSE_OK || !grammar) {
        pocketparse_string f=pocketparse_fault(engine); fprintf(stderr,"reclaim: grammar load failed: %.*s\n",(int)f.length,(const char*)f.pointer); return 2; }

    for (i = 0; i < iters; i++) {
        pocketparse_parser *p = NULL; pocketparse_string empty, bytes;
        empty.pointer=NULL; empty.length=0;
        if (pocketparse_parser_new(engine,grammar,empty,&p)!=POCKETPARSE_OK || !p) { fprintf(stderr,"reclaim: parser_new failed at iter %zu\n",i); return 2; }
        bytes.pointer=(const uint8_t*)fbytes; bytes.length=(uint32_t)flen;
        pocketparse_push(p,bytes);
        pocketparse_finish(p);
        pocketparse_parser_free(p);   /* must reclaim, or peak RSS climbs with i */
    }

    {
        long rss = bench_peak_rss_kb();
        printf("RECLAIM file_bytes=%zu iterations=%zu peak_rss_kb=%ld ceiling_kb=%ld\n",
               flen, iters, rss, ceiling);
        pocketparse_close(engine); freeall();
        if (rss > ceiling) { printf("RESULT: RED (peak RSS exceeded ceiling -- arena not reclaiming)\n"); return 1; }
        printf("RESULT: GREEN (peak RSS bounded across %zu parses in one engine)\n", iters);
        return 0;
    }
}
