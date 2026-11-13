/* Independent exhaustive 8-bit arithmetic oracle. Test every input pair in
   each interval, using ordinary wide C arithmetic, then compare the compiler's
   pure-contract acceptance with the presence of any possible failure. */
static void range_arithmetic_oracle(void) {
    static const int signed_ranges[][2]={{-128,-128},{-128,-120},{-128,127},{-12,12},{-10,-1},{-1,1},{0,0},{0,10},{1,12},{120,127}};
    static const int unsigned_ranges[][2]={{0,0},{0,1},{0,255},{1,10},{10,20},{100,200},{200,255},{255,255}};
    static const char *operations[]={"+","-","*","/","%"};
    unsigned samples=0;
    for(int sign=0;sign<2;++sign) {
        const int (*ranges)[2]=sign?signed_ranges:unsigned_ranges;
        int count=sign?(int)(sizeof signed_ranges/sizeof signed_ranges[0]):(int)(sizeof unsigned_ranges/sizeof unsigned_ranges[0]);
        for(int left=0;left<count;++left) for(int right=0;right<count;++right) for(int op=0;op<5;++op) {
            bool safe=true;
            for(int a=ranges[left][0];a<=ranges[left][1]&&safe;++a) for(int b=ranges[right][0];b<=ranges[right][1];++b) {
                int result=0;
                if((op==3||op==4)&&!b) { safe=false; break; }
                if(op==0) result=a+b;
                else if(op==1) result=a-b;
                else if(op==2) result=a*b;
                else if(op==3) result=a/b;
                else result=a%b;
                if(result<(sign?-128:0)||result>(sign?127:255)) { safe=false; break; }
            }
            char source[1024];
            snprintf(source,sizeof source,
                "safe pure %s8 test(const %s8 a,const %s8 b){if(a>=%d&&a<=%d&&b>=%d&&b<=%d){return a%sb;}return 0;}",
                sign?"sint":"uint",sign?"sint":"uint",sign?"sint":"uint",ranges[left][0],ranges[left][1],ranges[right][0],ranges[right][1],operations[op]);
            VxnProgram *program=vxn_compile(source);
            bool accepted=vxn_ok(program)!=0;
            expect(accepted==safe,"8-bit interval proof agrees with exhaustive arithmetic");
            if(accepted!=safe) { fprintf(stderr,"oracle expected %s: %s\n",safe?"safe":"failure",source); vxn_diagnostic(program,"interval oracle",stderr); }
            vxn_free(program); ++samples;
        }
    }
    printf("%u exhaustive interval proof cases\n",samples);
}
