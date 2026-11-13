/* Independent finite-domain oracle: enumerate the four inhabitants of a
   two-Boolean payload and compare coverage/reachability to the compiler. */
static unsigned pattern_mask(unsigned code) {
    unsigned mask=0;
    for(unsigned value=0;value<4;++value) {
        unsigned a=code/3,b=code%3;
        if((!a||(a==2)==((value&2)!=0))&&(!b||(b==2)==((value&1)!=0))) mask|=1u<<value;
    }
    return mask;
}
static void pattern_matrix_tests(void) {
    const char *parts[]={"_","false","true"}; unsigned cases=0;
    for(unsigned shape=0;shape<4;++shape) for(unsigned a=0;a<9;++a) for(unsigned b=0;b<9;++b)
    for(unsigned guarded=0;guarded<2;++guarded) for(unsigned fallback=0;fallback<2;++fallback) {
        unsigned first=guarded?0:pattern_mask(a)|(shape==3?1:0),second=pattern_mask(b)|(shape==3?8:0),covered=first|second;
        bool reachable=(second&~first)!=0;
        bool accepted=reachable&&(fallback?covered!=15:covered==15);
        const char *formats[]={
            "choice Pair{Both(bool,bool)}safe pure uint32 f(const Pair p){return match p{Pair::Both(%s,%s)%s->1,Pair::Both(%s,%s)->2%s};}safe io void main(){output(f(Pair::Both(false,false)));output(f(Pair::Both(false,true)));output(f(Pair::Both(true,false)));output(f(Pair::Both(true,true)));}",
            "struct Pair{bool a;bool b;}safe pure uint32 f(const Pair p){return match p{Pair{a=%s,b=%s}%s->1,Pair{a=%s,b=%s}->2%s};}safe io void main(){output(f(Pair{a=false,b=false}));output(f(Pair{a=false,b=true}));output(f(Pair{a=true,b=false}));output(f(Pair{a=true,b=true}));}",
            "safe pure uint32 f(const bool[2] p){return match p{{%s,%s}%s->1,{%s,%s}->2%s};}safe io void main(){output(f({false,false}));output(f({false,true}));output(f({true,false}));output(f({true,true}));}",
            "safe pure uint32 f(const bool[2] p){return match p{{%s,%s}|{false,false}%s->1,{%s,%s}|{true,true}->2%s};}safe io void main(){output(f({false,false}));output(f({false,true}));output(f({true,false}));output(f({true,true}));}"};
        char source[2048]; snprintf(source,sizeof source,formats[shape],
            parts[a/3],parts[a%3],guarded?" if false":"",parts[b/3],parts[b%3],fallback?",_->3":"");
        VxnProgram *p=vxn_compile(source); ++cases;
        expect(p&&(vxn_ok(p)!=0)==accepted,"finite pattern coverage and reachability oracle");
        if(p&&!vxn_ok(p)&&!accepted) expect(strstr(p->error,reachable&&!fallback?"non-exhaustive":"unreachable")!=NULL,"finite pattern rejection reason");
        if(p&&vxn_ok(p)&&accepted) {
            FILE *out=tmpfile(),*err=tmpfile();
            expect(out&&err,"pattern oracle streams");
            if(out&&err) {
                expect(vxn_run(p,out,err)==0,"finite pattern selection executes");
                char actual[80],expected[80]; size_t used=0;
                for(unsigned value=0;value<4;++value) used+=(size_t)snprintf(expected+used,sizeof expected-used,"%u\n",first&(1u<<value)?1:second&(1u<<value)?2:3);
                contents(out,actual,sizeof actual); expect(!strcmp(actual,expected),"finite pattern first accepted arm oracle");
            }
            if(out) fclose(out);
            if(err) fclose(err);
        }
        vxn_free(p);
    }
    printf("%u finite pattern-matrix oracle cases\n",cases);
}
static void range_matrix_tests(void) {
    const int points[]={0,64,128,255},samples[]={0,1,63,64,127,128,254,255}; unsigned cases=0;
    for(int sign=0;sign<2;++sign) for(int al=0;al<4;++al) for(int ah=al;ah<4;++ah)
    for(int bl=0;bl<4;++bl) for(int bh=bl;bh<4;++bh)
    for(int guard=0;guard<2;++guard) for(int fallback=0;fallback<2;++fallback) {
        bool reachable=false,exhaustive=true; int selected[256];
        for(int value=0;value<256;++value) {
            bool a=!guard&&points[al]<=value&&value<=points[ah],b=points[bl]<=value&&value<=points[bh];
            if(b&&!a) reachable=true;
            if(!a&&!b) exhaustive=false;
            selected[value]=a?1:b?2:3;
        }
        bool accepted=reachable&&(fallback?!exhaustive:exhaustive); int delta=sign?128:0;
        char source[2048],expected[80]; size_t used=(size_t)snprintf(source,sizeof source,
            "safe pure uint32 f(const %s n){return match n{%d..=%d%s->1,%d..=%d->2%s};}safe io void main(){",
            sign?"sint8":"uint8",points[al]-delta,points[ah]-delta,guard?" if false":"",points[bl]-delta,points[bh]-delta,fallback?",_->3":"");
        size_t written=0;
        for(int i=0;i<8;++i) {
            used+=(size_t)snprintf(source+used,sizeof source-used,"output(f(%d));",samples[i]-delta);
            written+=(size_t)snprintf(expected+written,sizeof expected-written,"%d\n",selected[samples[i]]);
        }
        snprintf(source+used,sizeof source-used,"}"); VxnProgram *p=vxn_compile(source); ++cases;
        expect(p&&(vxn_ok(p)!=0)==accepted,"range matrix exhaustive 8-bit-domain oracle");
        if(p&&!vxn_ok(p)&&!accepted) expect(strstr(p->error,reachable&&!fallback?"non-exhaustive":"unreachable")!=NULL,"range matrix rejection reason");
        if(p&&vxn_ok(p)&&accepted) {
            FILE *out=tmpfile(),*err=tmpfile(); expect(out&&err,"range oracle streams");
            if(out&&err) { char actual[80]; expect(vxn_run(p,out,err)==0,"range oracle executes"); contents(out,actual,sizeof actual); expect(!strcmp(actual,expected),"range oracle signed/unsigned selection"); }
            if(out) fclose(out);
            if(err) fclose(err);
        }
        vxn_free(p);
    }
    printf("%u range-matrix oracle cases\n",cases);
}
