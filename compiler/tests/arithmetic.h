/* An independent small-integer oracle: intermediates fit in int64_t. */
static void arithmetic_tests(void) {
    bool all=true; unsigned long count=0;
    for(unsigned sign=0;sign<2;++sign) for(unsigned mode=0;mode<3;++mode)
    for(unsigned aa=0;aa<256;++aa) for(unsigned bb=0;bb<256;++bb) {
        int64_t a=sign&&aa>=128?(int64_t)aa-256:(int64_t)aa;
        int64_t b=sign&&bb>=128?(int64_t)bb-256:(int64_t)bb;
        int64_t minimum=sign?-128:0, maximum=sign?127:255;
        for(int op=VX_ADD;op<=VX_GE;++op) {
            bool expected_ok=true; int64_t result=0;
            switch(op) {
            case VX_ADD:result=a+b;break;case VX_SUB:result=a-b;break;case VX_MUL:result=a*b;break;
            case VX_DIV:if(b) result=a/b;else expected_ok=false;break;
            case VX_MOD:if(b) result=a%b;else expected_ok=false;break;
            case VX_EQ:result=a==b;break;case VX_NE:result=a!=b;break;
            case VX_LT:result=a<b;break;case VX_LE:result=a<=b;break;
            case VX_GT:result=a>b;break;case VX_GE:result=a>=b;break;
            }
            if(op<=VX_MOD&&(result<minimum||result>maximum)) {
                if(mode==1) expected_ok=false;
                if(mode==2) result=result<minimum?minimum:maximum;
            }
            bool ok=true;
            uint64_t actual=vx_integer_op(op,aa,bb,8,sign!=0,mode,&ok);
            if(ok!=expected_ok||(ok&&actual!=((uint64_t)result&255))) {
                fprintf(stderr,"arithmetic mismatch: sign=%u mode=%u a=%u b=%u op=%d\n",sign,mode,aa,bb,op);
                all=false; goto done;
            }
            ++count;
        }
    }
done:
    expect(all,"exhaustive 8-bit arithmetic and comparison oracle");
    printf("%lu exhaustive arithmetic cases\n",count);
    for(unsigned width=8;width<=64;width*=2) {
        uint64_t mask=vx_mask(width),top=UINT64_C(1)<<(width-1); bool ok=true;
        expect(vx_integer_op(VX_ADD,mask,1,width,false,0,&ok)==0&&ok,"unsigned wrap at every width");
        ok=true; (void)vx_integer_op(VX_ADD,mask,1,width,false,1,&ok); expect(!ok,"unsigned checked overflow at every width");
        ok=true; expect(vx_integer_op(VX_MUL,top,mask,width,true,2,&ok)==top-1&&ok,"signed saturating minimum times negative one");
        ok=true; expect(vx_integer_op(VX_SHR,top,width-1,width,true,0,&ok)==mask&&ok,"arithmetic right shift at every width");
        ok=true; (void)vx_integer_op(VX_SHL,1,width,width,false,0,&ok); expect(!ok,"shift by width fails before C shift");
        expect(vx_integer_cast(mask,width,true,64)==UINT64_MAX,"sign extension at every width");
    }
}
