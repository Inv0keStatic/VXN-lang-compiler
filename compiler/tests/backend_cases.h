/* Regression cases for native lowering, also checked by the interpreter. */
static const Case backend_cases[] = {
    {"aggregate loop temporaries have bounded stack storage",
     "struct Block { wrap uint8[16] bytes; }"
     "safe pure Block change(mut Block b) { b.bytes[0] += 1; return b; }"
     "safe io void main() { mut Block b = Block { bytes = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} };"
     "for (let mut i = 0; i < 15000; i++) { b = change(b); } output(b.bytes[0]); }",
     "152\n","",0},
    {"aggregate match result snapshots and nested calls",
     "struct Pair { wrap uint32 x; wrap uint32 y; } choice Holder { Full(Pair), Empty }"
     "safe pure Pair get(const Holder b) { return match b { Holder::Full(mut p) -> p, Holder::Empty -> Pair { x = 0, y = 0 } }; }"
     "safe pure Holder change(mut Holder b) { b = Holder::Full(Pair { x = 7, y = 8 }); return b; }"
     "safe io void main() { const Holder b = Holder::Full(Pair { x = 3, y = 4 });"
     "mut Pair a = get(change(b)); a.x = 9; output(a.x); output(a.y); output(get(b).x); output(get(Holder::Empty).y); }",
     "9\n8\n3\n0\n","",0},
    {"floating rounding, signed zero, unordered comparisons",
     "safe pure float32 product(const float32 a, const float32 b) { return a * b - 1.0; }"
     "safe io void main() { output(product(1.00000011920928955078125,0.99999988079071044921875));"
     "const float32 z = -0.0; output(z); output(1.0 / z < 0.0); output(-z);"
     "const float64 nan = 0.0 / 0.0;"
     "output(nan < 0.0); output(nan <= 0.0); output(nan > 0.0); output(nan >= 0.0); output(nan != 0.0);"
     "const float32 small = 1.401298464324817e-45; output(small * 2.0 > small); }",
     "0\n-0\ntrue\n0\nfalse\nfalse\nfalse\nfalse\ntrue\ntrue\n","",0},
    {"nested short circuit branch merges",
     "safe bool mark(const wrap uint32 n, const bool b) { output(n); return b; }"
     "safe io void main() { output((mark(1,false) && mark(2,true)) || (mark(3,true) && !mark(4,false)));"
     "output((mark(5,true) || mark(6,false)) && (mark(7,false) || mark(8,true))); }",
     "1\n3\n4\ntrue\n5\n7\n8\ntrue\n","",0},
};

/* Independent unsigned arithmetic checks every wrap width, with operands
 * arriving as parameters (so Vxn's literal folder cannot mask a lowering bug).
 * Signed order is computed by flipping the sign bit, without signed overflow. */
static void native_numeric_tests(const char *cc,Files *f) {
    for(unsigned width=8;width<=64;width*=2) for(unsigned sign=0;sign<2;++sign) {
        uint64_t mask=UINT64_MAX>>(64-width),top=UINT64_C(1)<<(width-1);
        uint64_t operands[]={0,1,top-1,top,mask-1,mask};
        char source[8192],wanted[8192],type[32];
        snprintf(type,sizeof type,"wrap %sint%u",sign?"s":"u",width);
        size_t n=(size_t)snprintf(source,sizeof source,
            "safe io void numbers(const %s a, const %s b) {"
            "output(narrow[wrap uint64](narrow[wrap uint%u](a + b)));"
            "output(narrow[wrap uint64](narrow[wrap uint%u](a - b)));"
            "output(narrow[wrap uint64](narrow[wrap uint%u](a * b)));"
            "output(a < b); output(a >= b);"
            "output(narrow[wrap uint64](narrow[wrap uint%u](~a))); }"
            "safe io void main() {",type,type,width,width,width,width);
        size_t m=0;
        for(unsigned i=0;i<6;++i) {
            uint64_t a=operands[i],b=operands[5-i],aa=sign?a^top:a,bb=sign?b^top:b;
            n+=(size_t)snprintf(source+n,sizeof source-n,
                "const wrap uint64 a%u = %" PRIu64 "; const wrap uint64 b%u = %" PRIu64 ";"
                "numbers(narrow[%s](a%u),narrow[%s](b%u));",
                i,a,i,b,type,i,type,i);
            m+=(size_t)snprintf(wanted+m,sizeof wanted-m,
                "%" PRIu64 "\n%" PRIu64 "\n%" PRIu64 "\n%s\n%s\n%" PRIu64 "\n",
                (a+b)&mask,(a-b)&mask,(a*b)&mask,aa<bb?"true":"false",aa>=bb?"true":"false",mask^a);
        }
        snprintf(source+n,sizeof source-n,"}");
        Case c={"native wrap width oracle",source,wanted,"",0}; native_case(&c,cc,f);
    }
}
