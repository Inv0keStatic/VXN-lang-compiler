#include "internal.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <process.h>
#define make_directory(path) _mkdir(path)
#define remove_directory(path) _rmdir(path)
#define process_id() _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define make_directory(path) mkdir(path,0700)
#define remove_directory(path) rmdir(path)
#define process_id() getpid()
#endif

static unsigned checks, failures;
static const char *llvm_driver;
static bool unit_driver;
static int owner_probe=-1;
static void expect(bool ok, const char *label) {
    if(getenv("VXN_TEST_TRACE")) fprintf(stderr,"CHECK %u: %s\n",checks+1,label);
    ++checks;
    if (!ok) { ++failures; fprintf(stderr,"FAIL: %s\n",label); }
}
static void contents(FILE *file, char *out, size_t cap) {
    rewind(file); size_t n = fread(out,1,cap-1,file); out[n] = 0;
}
static void read_file(const char *path, char *out, size_t cap) {
    FILE *file = fopen(path,"r");
    if (!file) { out[0] = 0; expect(false,"open result file"); return; }
    contents(file,out,cap); fclose(file);
}
static bool has_line(const char *path,const char *text) {
    FILE *file=fopen(path,"r"); if(!file) return false;
    char line[4096]; bool found=false;
    while(fgets(line,sizeof line,file)) if(strstr(line,text)) { found=true; break; }
    fclose(file); return found;
}
typedef struct { const char *label, *source, *out, *err; int status; } Case;
static const Case cases[] = {
    {"empty main", "safe pure void main() {}", "", "", 0},
    {"wrapping addition", "safe io nopanic void main() { const wrap uint32 n = 4294967295; output(n + 1); output(n + 0xffff_ffff); }", "0\n4294967294\n", "", 0},
    {"boolean branches", "safe io void main() { if (!!false) { output(1); } else { output(2); } if (!false) { output(3); } }", "2\n3\n", "", 0},
    {"lexical scopes", "safe io void main() { const wrap uint32 x = 4; { let const x = x + 1; output(x); } output(x); }", "5\n4\n", "", 0},
    {"forward call", "safe io void main() { output(add(40,2)); } safe pure wrap uint32 add(const wrap uint32 a, const wrap uint32 b) { return a + b; }", "42\n", "", 0},
    {"transitive calls", "safe io void main() { middle(); } safe void middle() { leaf(); } safe void leaf() { output(7); }", "7\n", "", 0},
    {"unused values", "safe pure void main() { const bool x = true; 7 + 9; unused(3); } safe pure void unused(const wrap uint32 value) {}", "", "", 0},
    {"void return expression", "safe io void main() { return output(6); }", "6\n", "", 0},
    {"boolean function", "safe pure bool invert(const bool value) { return !value; } safe io void main() { if (invert(false)) { output(8); } else { output(9); } }", "8\n", "", 0},
    {"left to right arguments", "safe wrap uint32 mark(const wrap uint32 n) { output(n); return n; } safe pure wrap uint32 add(const wrap uint32 a, const wrap uint32 b) { return a + b; } safe io void main() { output(add(mark(1),mark(2)) + mark(3)); }", "1\n2\n3\n6\n", "", 0},
    {"panic stops binary rhs", "safe wrap uint32 fail() { panic(); } safe wrap uint32 mark() { output(9); return 9; } safe io void main() { output(1); output(fail() + mark()); output(8); }", "1\n", "panic\n", 1},
    {"panic stops later argument", "safe pure wrap uint32 add(const wrap uint32 a, const wrap uint32 b) { return a + b; } safe wrap uint32 fail() { panic(); } safe wrap uint32 mark() { output(9); return 9; } safe io void main() { output(add(fail(),mark())); }", "", "panic\n", 1},
    {"panic preserves earlier argument", "safe pure wrap uint32 add(const wrap uint32 a, const wrap uint32 b) { return a + b; } safe wrap uint32 fail() { panic(); } safe wrap uint32 mark() { output(9); return 9; } safe io void main() { output(add(mark(),fail())); }", "9\n", "panic\n", 1},
    {"panic in condition", "safe bool fail() { return panic(); } safe io void main() { if (fail()) { output(1); } else { output(2); } output(3); }", "", "panic\n", 1},
    {"panic with contextual type", "safe io void main() { output(1 + panic()); }", "", "panic\n", 1},
    {"dead branch effects", "safe io void main() { if (false) { panic(); } else { output(7); } }", "7\n", "", 0},
    {"early nested return", "safe pure wrap uint32 choose(const bool b) { { if (b) { return 4; } else { return 5; } } } safe io void main() { output(choose(false)); }", "5\n", "", 0},
    {"typed panic binding", "safe io void main() { const bool x = panic(); }", "", "panic\n", 1},
    {"comments and literals", "/* header */ safe io void main() { // comment\n output(4_000 + 0xFF); }", "4255\n", "", 0},
    {"parameter shadowing", "safe pure wrap uint32 f(const wrap uint32 n) { { const wrap uint32 n = n + 1; return n; } } safe io void main() { output(f(5)); }", "6\n", "", 0},
};
typedef struct { const char *source, *error; } Bad;
static const Bad bad[] = {
    {"", "at least one function"},
    {"safe pure void Main() {}", "lower_snake_case"},
    {"safe pure void main_() {}", "lower_snake_case"},
    {"safe pure void main() { let const badName = 1; }", "lower_snake_case"},
    {"safe pure void main() { let const a__b = 1; }", "lower_snake_case"},
    {"safe pure void main() { let x = 1; }", "require const or mut"},
    {"safe pure void f(wrap uint32 x) {}", "parameters require const or mut"},
    {"safe pure void main() { const bool x = 1; }", "expected bool, got checked uint32"},
    {"safe pure void main() { true + 1; }", "numeric operands"},
    {"safe pure void main() { !1; }", "expected bool"},
    {"safe pure void main() { if (1) {} }", "expected bool"},
    {"safe pure void main() { let const x = output(1); }", "binding cannot have type void"},
    {"safe pure void f(const void x) {}", "binding cannot have type void"},
    {"safe pure void main() { unknown; }", "unknown value"},
    {"safe pure void main() { unknown(); }", "unknown function"},
    {"safe pure void main() { const bool x = false; } safe pure void f() { x; }", "unknown value"},
    {"safe pure void main() { { const bool x = false; } x; }", "unknown value"},
    {"safe pure void main() { let const x = 1; let const x = 2; }", "duplicate binding"},
    {"safe pure void f(const bool x, const bool x) {}", "duplicate binding"},
    {"safe pure void f(const bool x) { const bool x = true; }", "duplicate binding"},
    {"safe pure void main() {} safe pure void main() {}", "duplicate function"},
    {"safe pure void output() {}", "redefine a builtin"},
    {"safe pure void main() { output(); }", "output expects one"},
    {"safe io void main() { panic(1); }", "panic expects no"},
    {"safe pure void f(const bool a) {} safe pure void main() { f(); }", "expected 1 arguments"},
    {"safe pure void f(const bool a) {} safe pure void main() { f(1); }", "expected bool"},
    {"safe pure void f() {} safe pure void main() { const bool f = true; f(); }", "not callable"},
    {"safe pure bool f() {}", "not every path returns"},
    {"safe pure bool f(const bool b) { if (b) { return true; } }", "not every path returns"},
    {"safe pure bool f() { return; }", "return requires a value"},
    {"safe pure void main() { return 1; }", "expected void"},
    {"safe pure void main() { return; 1; }", "unreachable statement"},
    {"safe io void main() { panic(); 1; }", "unreachable statement"},
    {"safe pure bool main() { return true; }", "main must return void"},
    {"safe pure void main(const bool b) {}", "main must return void"},
    {"safe void main() {}", "require explicit effect annotations"},
    {"pub safe void f() {}", "require explicit effect annotations"},
    {"safe pure io void main() {}", "conflicting effect annotations"},
    {"safe pure pure void main() {}", "duplicate effect annotation"},
    {"safe pure void main() { output(1); }", "main -> output"},
    {"safe hot void main() { middle(); } safe void middle() { leaf(); } safe void leaf() { output(1); }", "main -> middle -> leaf -> output"},
    {"safe noalloc void main() { output(1); }", "violates `noalloc`"},
    {"safe nopanic void main() { panic(); }", "violates `nopanic`"},
    {"safe noblock void main() { output(1); }", "violates `noblock`"},
    {"safe pure void main() { if (false) { output(1); } }", "violates `pure`"},
    {"safe pure ref Book f() {}", "unknown type"},
    {"safe pure bool f(const bool b) from(b) { return b; }", "requires a shared reference"},
    {"safe pure void main() { emit(1); }", "not implemented"},
    {"dyntime safe pure void main() {}", "staging are not implemented"},
    {"safe pure void main() { 4294967296; }", "must fit checked uint32"},
    {"safe pure void main() { 0x100000000; }", "must fit checked uint32"},
    {"safe pure void main() { 0x; }", "malformed integer literal"},
    {"safe pure void main() { 1__2; }", "malformed integer literal"},
    {"safe pure void main() { 1_; }", "malformed integer literal"},
    {"safe pure void main() { 12x; }", "must fit uint64"},
    {"safe pure void main() { 0x_1; }", "malformed integer literal"},
    {"/* open", "unterminated block comment"},
    {"safe pure void main() {", "before end of file"},
    {"safe pure void main() { true }", "expected ';'"},
};
#include "features.h"
#include "io_cases.h"
#include "references.h"
#include "library_cases.h"
#include "comptime_cases.h"
#include "calls.h"
#include "callables.h"
#include "slices.h"
#include "generics.h"
#include "results.h"
#include "ownership.h"
#include "exclusive.h"
#include "arithmetic.h"
#include "parse_cases.h"
#include "parse_runtime.h"
#include "float_runtime.h"
#include "utf8_cases.h"
#include "utf8_runtime.h"
#include "string_cases.h"
#include "text_cases.h"
#include "tail_cases.h"
#include "range_cases.h"
#include "extended_cases.h"
#include "borrowed_aggregate_cases.h"
#include "returned_exclusive_cases.h"
#include "split_cases.h"
#include "memory_cases.h"
#include "lazy_cases.h"
#include "equation_cases.h"
#include "partial_cases.h"
#include "kind_cases.h"
#include "recursive_lazy_cases.h"
#include "pattern_cases.h"
#include "builder_cases.h"
#include "modules.h"
static void interpret(const Case *c) {
    if(getenv("VXN_TRACE_TESTS")) fprintf(stderr,"case: %s\n",c->label);
    if(getenv("VXN_TEST_TRACE")) fprintf(stderr,"CASE: %s\n",c->label);
    VxnProgram *p = vxn_compile(c->source);
    expect(vxn_ok(p),c->label);
    if (!vxn_ok(p)) { vxn_diagnostic(p,c->label,stderr); vxn_free(p); return; }
    FILE *out = tmpfile(), *err = tmpfile();
    if (!out || !err) { expect(false,"tmpfile"); if(out) fclose(out); if(err) fclose(err); vxn_free(p); return; }
    expect(vxn_run(p,out,err) == c->status,c->label);
    char buffer[8192]; contents(out,buffer,sizeof buffer); expect(!strcmp(buffer,c->out),c->label);
    if (strcmp(buffer,c->out)) fprintf(stderr,"expected stdout:\n%sactual stdout:\n%s",c->out,buffer);
    contents(err,buffer,sizeof buffer); expect(!strcmp(buffer,c->err),c->label);
    fclose(out); fclose(err); vxn_free(p);
}
#include "pattern_matrix.h"
#include "range_oracle.h"
static uint32_t random_state = UINT32_C(0x12345678);
static uint32_t random_word(void) {
    random_state ^= random_state << 13; random_state ^= random_state >> 17; random_state ^= random_state << 5;
    return random_state;
}
static void lazy_tests(void) {
    for(size_t i=0;i<sizeof lazy_cases/sizeof lazy_cases[0];++i) interpret(&lazy_cases[i]);
    for(size_t i=0;i<sizeof lazy_bad/sizeof lazy_bad[0];++i) {
        VxnProgram *p=vxn_compile(lazy_bad[i].source);
        bool ok=p&&!vxn_ok(p)&&strstr(p->error,lazy_bad[i].error);
        expect(ok,lazy_bad[i].error);
        if(!ok) { fprintf(stderr,"source: %s\n",lazy_bad[i].source); if(p) vxn_diagnostic(p,"lazy rejection",stderr); }
        vxn_free(p);
    }
}
static void equation_tests(void) {
    for(size_t i=0;i<sizeof equation_cases/sizeof equation_cases[0];++i) interpret(&equation_cases[i]);
    for(size_t i=0;i<sizeof equation_bad/sizeof equation_bad[0];++i) {
        VxnProgram *p=vxn_compile(equation_bad[i].source);
        bool ok=p&&!vxn_ok(p)&&strstr(p->error,equation_bad[i].error);
        expect(ok,equation_bad[i].error);
        if(!ok) { fprintf(stderr,"source: %s\n",equation_bad[i].source); if(p) vxn_diagnostic(p,"equation rejection",stderr); }
        vxn_free(p);
    }
}
static void partial_tests(void) {
    for(size_t i=0;i<sizeof partial_cases/sizeof partial_cases[0];++i) interpret(&partial_cases[i]);
    for(size_t i=0;i<sizeof partial_bad/sizeof partial_bad[0];++i) {
        VxnProgram *p=vxn_compile(partial_bad[i].source);
        bool ok=p&&!vxn_ok(p)&&strstr(p->error,partial_bad[i].error);
        expect(ok,partial_bad[i].error);
        if(!ok&&p) vxn_diagnostic(p,partial_bad[i].source,stderr);
        vxn_free(p);
    }
}
static void kind_tests(void) {
    for(size_t i=0;i<sizeof kind_cases/sizeof kind_cases[0];++i) interpret(&kind_cases[i]);
    for(size_t i=0;i<sizeof kind_bad/sizeof kind_bad[0];++i) {
        VxnProgram *p=vxn_compile(kind_bad[i].source);
        bool ok=p&&!vxn_ok(p)&&strstr(p->error,kind_bad[i].error);
        expect(ok,kind_bad[i].error);
        if(!ok&&p) vxn_diagnostic(p,kind_bad[i].source,stderr);
        vxn_free(p);
    }
}
static void recursive_lazy_tests(void) {
    for(size_t i=0;i<sizeof recursive_lazy_cases/sizeof recursive_lazy_cases[0];++i) interpret(&recursive_lazy_cases[i]);
    for(size_t i=0;i<sizeof recursive_lazy_bad/sizeof recursive_lazy_bad[0];++i) {
        VxnProgram *p=vxn_compile(recursive_lazy_bad[i].source);
        bool ok=p&&!vxn_ok(p)&&strstr(p->error,recursive_lazy_bad[i].error);
        expect(ok,recursive_lazy_bad[i].error);
        if(!ok&&p) vxn_diagnostic(p,recursive_lazy_bad[i].source,stderr);
        vxn_free(p);
    }
}
static void diagnostic_limit_tests(void) {
    for (size_t i = 0; i < sizeof io_bad / sizeof io_bad[0]; ++i) {
        if(getenv("VXN_TRACE_TESTS")) fprintf(stderr,"rejection: io_bad %zu\n",i);
        VxnProgram *program=vxn_compile(io_bad[i].source);
        bool ok=program && !vxn_ok(program) && strstr(program->error,io_bad[i].error);
        expect(ok,io_bad[i].error);
        if(!ok && program) vxn_diagnostic(program,"I/O rejection",stderr);
        vxn_free(program);
    }
    for (size_t i = 0; i < sizeof feature_bad / sizeof feature_bad[0]; ++i) {
        if(getenv("VXN_TRACE_TESTS")) fprintf(stderr,"rejection: feature_bad %zu\n",i);
        VxnProgram *program = vxn_compile(feature_bad[i].source);
        bool ok = program && !vxn_ok(program) && strstr(program->error,feature_bad[i].error);
        expect(ok,feature_bad[i].error);
        if (!ok && program) { fprintf(stderr,"source: %s\n",feature_bad[i].source); vxn_diagnostic(program,"rejection",stderr); }
        vxn_free(program);
    }
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        if(getenv("VXN_TRACE_TESTS")) fprintf(stderr,"rejection: bad %zu\n",i);
        VxnProgram *p = vxn_compile(bad[i].source);
        bool ok = p && !vxn_ok(p) && strstr(p->error,bad[i].error);
        expect(ok,bad[i].error);
        if (!ok && p) { fprintf(stderr,"source: %s\n",bad[i].source); vxn_diagnostic(p,"rejection",stderr); }
        vxn_free(p);
    }
    VxnProgram *library = vxn_compile("pub safe pure bool invert(const bool b) { return !b; }");
    expect(vxn_ok(library),"library without main"); vxn_free(library);
    if(getenv("VXN_TRACE_TESTS")) fputs("limit: syntax depth\n",stderr);
    char depth[4096]; size_t n = (size_t)sprintf(depth,"safe pure void main() {");
    for (int i = 0; i < 200; ++i) depth[n++] = '(';
    depth[n++] = '1';
    for (int i = 0; i < 200; ++i) depth[n++] = ')';
    strcpy(depth+n,"; }");
    VxnProgram *p = vxn_compile(depth);
    expect(p && !vxn_ok(p) && strstr(p->error,"nesting"),"nesting limit"); vxn_free(p);
    if(getenv("VXN_TRACE_TESTS")) fputs("limit: AST depth\n",stderr);
    n = (size_t)sprintf(depth,"safe pure void main() { 0");
    for (int i = 0; i < VX_MAX_DEPTH; ++i) n += (size_t)sprintf(depth+n," + 1");
    strcpy(depth+n,"; }"); p = vxn_compile(depth);
    expect(p && !vxn_ok(p) && strstr(p->error,"expression tree"),"AST depth limit"); vxn_free(p);
    if(getenv("VXN_TRACE_TESTS")) fputs("limit: source budget\n",stderr);
    char *large = malloc(VX_MAX_SOURCE + 2);
    if (large) {
        memset(large,' ',VX_MAX_SOURCE + 1); large[VX_MAX_SOURCE + 1] = 0;
        p = vxn_compile(large); expect(p && !vxn_ok(p) && strstr(p->error,"1 MiB"),"source size limit");
        vxn_free(p); free(large);
    } else expect(false,"source limit fixture allocation");
    if(getenv("VXN_TRACE_TESTS")) fputs("limit: execution budget\n",stderr);
    char budget[8192]; n = (size_t)sprintf(budget,"safe void f0() {} ");
    for (int i = 1; i < 24; ++i)
        n += (size_t)sprintf(budget+n,"safe void f%d() { f%d(); f%d(); } ",i,i-1,i-1);
    strcpy(budget+n,"safe pure void main() { f23(); }");
    p = vxn_compile(budget); expect(vxn_ok(p),"acyclic exponential call tree");
    if (vxn_ok(p)) {
        FILE *out = tmpfile(), *err = tmpfile();
        if (out && err) {
            expect(vxn_run(p,out,err) == 2,"execution budget is a host failure");
            char message[128]; contents(err,message,sizeof message);
            expect(strstr(message,"resource limit") != NULL,"resource diagnostic is not a language panic");
        } else expect(false,"budget stream allocation");
        if (out) fclose(out);
        if (err) fclose(err);
    }
    vxn_free(p);
    if(getenv("VXN_TRACE_TESTS")) fputs("limit: call depth\n",stderr);
    p=vxn_compile("safe pure void main() { main(); }");
    expect(vxn_ok(p),"recursive source is accepted");
    if(vxn_ok(p)) {
        FILE *out=tmpfile(),*err=tmpfile();
        if(out&&err) expect(vxn_run(p,out,err)==2,"recursive execution is bounded");
        else expect(false,"recursion stream allocation");
        if(out) fclose(out);
        if(err) fclose(err);
    }
    vxn_free(p);
    p=vxn_compile("safe pure void main() { while (true) {} }");
    expect(vxn_ok(p),"loop source is accepted");
    if(vxn_ok(p)) {
        FILE *out=tmpfile(),*err=tmpfile();
        if(out&&err) expect(vxn_run(p,out,err)==2,"loop execution is bounded");
        else expect(false,"loop stream allocation");
        if(out) fclose(out);
        if(err) fclose(err);
    }
    vxn_free(p);
    /* A negative sint8 index must not become array element 255. */
    char negative_index[4096]; n=(size_t)sprintf(negative_index,"safe io void main() { const wrap uint8[256] a = {");
    for(int i=0;i<256;++i) n+=(size_t)sprintf(negative_index+n,"%s0",i?",":"");
    strcpy(negative_index+n,"}; const wrap sint8 index = -1; output(a[index]); }");
    Case negative_case={"negative narrow index",negative_index,"","panic\n",1}; interpret(&negative_case);
}
static void core_tests(void) {
    recursive_lazy_tests();
    kind_tests();
    partial_tests();
    lazy_tests();
    equation_tests();
    {
        char source[8192]; size_t used=(size_t)snprintf(source,sizeof source,"struct Layer0{uint32 n;}");
        for(int i=1;i<40;++i) used+=(size_t)snprintf(source+used,sizeof source-used,"struct Layer%d{Layer%d[0] a;Layer%d[0] b;}",i,i-1,i-1);
        snprintf(source+used,sizeof source-used,"safe pure void main(){}");
        VxnProgram *p=vxn_compile(source); expect(vxn_ok(p),"shared type graph ownership analysis is bounded"); vxn_free(p);
    }
    for(size_t i=0;i<sizeof range_cases/sizeof range_cases[0];++i) interpret(&range_cases[i]);
    for(size_t i=0;i<sizeof range_bad/sizeof range_bad[0];++i) {
        VxnProgram *p=vxn_compile(range_bad[i].source); bool ok=p&&!vxn_ok(p)&&strstr(p->error,range_bad[i].error);
        expect(ok,"unsafe bounds retain failure effects"); if(!ok&&p) vxn_diagnostic(p,"range rejection",stderr); vxn_free(p);
    }
    for(size_t i=0;i<sizeof tail_cases/sizeof tail_cases[0];++i) interpret(&tail_cases[i]);
    for(size_t i=0;i<sizeof tail_bad/sizeof tail_bad[0];++i) {
        if(getenv("VXN_TRACE_TESTS")) fprintf(stderr,"rejection: tail_bad %zu\n",i);
        VxnProgram *p=vxn_compile(tail_bad[i].source); bool ok=p&&!vxn_ok(p)&&strstr(p->error,tail_bad[i].error);
        expect(ok,tail_bad[i].error); if(!ok&&p) vxn_diagnostic(p,"tail rejection",stderr); vxn_free(p);
    }
    const Case tail_limit={"tail recursion retains interpreter step budget","safe pure void repeat(){return tail repeat();}safe pure void main(){return tail repeat();}","","interpreter resource limit exceeded\n",2};
    interpret(&tail_limit);
    module_tests();
    pattern_matrix_tests();
    range_matrix_tests();
    range_arithmetic_oracle();
    arithmetic_tests();
    parse_runtime_tests();
    float_runtime_tests();
    utf8_runtime_tests();
    for(size_t i=0;i<sizeof builder_cases/sizeof builder_cases[0];++i) interpret(&builder_cases[i]);
    for(size_t i=0;i<sizeof builder_bad/sizeof builder_bad[0];++i) {
        if(getenv("VXN_TRACE_TESTS")) fprintf(stderr,"rejection: builder_bad %zu\n",i);
        VxnProgram *p=vxn_compile(builder_bad[i].source); bool ok=p&&!vxn_ok(p)&&strstr(p->error,builder_bad[i].error);
        expect(ok,builder_bad[i].error); if(!ok&&p) vxn_diagnostic(p,"builder rejection",stderr); vxn_free(p);
    }
    for(size_t i=0;i<sizeof pattern_cases/sizeof pattern_cases[0];++i) interpret(&pattern_cases[i]);
    for(size_t i=0;i<sizeof pattern_bad/sizeof pattern_bad[0];++i) {
        if(getenv("VXN_TRACE_TESTS")) fprintf(stderr,"rejection: pattern_bad %zu\n",i);
        VxnProgram *p=vxn_compile(pattern_bad[i].source); bool ok=p&&!vxn_ok(p)&&strstr(p->error,pattern_bad[i].error);
        expect(ok,pattern_bad[i].error); if(!ok&&p) vxn_diagnostic(p,"pattern rejection",stderr); vxn_free(p);
    }
    for(size_t i=0;i<sizeof text_cases/sizeof text_cases[0];++i) interpret(&text_cases[i]);
    for(size_t i=0;i<sizeof text_bad/sizeof text_bad[0];++i) {
        if(getenv("VXN_TRACE_TESTS")) fprintf(stderr,"rejection: text_bad %zu\n",i);
        VxnProgram *p=vxn_compile(text_bad[i].source); bool ok=p&&!vxn_ok(p)&&strstr(p->error,text_bad[i].error);
        expect(ok,text_bad[i].error); if(!ok&&p) vxn_diagnostic(p,"str rejection",stderr); vxn_free(p);
    }
    for(size_t i=0;i<sizeof string_cases/sizeof string_cases[0];++i) interpret(&string_cases[i]);
    for(size_t i=0;i<sizeof string_bad/sizeof string_bad[0];++i) {
        if(getenv("VXN_TRACE_TESTS")) fprintf(stderr,"rejection: string_bad %zu\n",i);
        VxnProgram *p=vxn_compile(string_bad[i].source); bool ok=p&&!vxn_ok(p)&&strstr(p->error,string_bad[i].error);
        expect(ok,string_bad[i].error); if(!ok&&p) vxn_diagnostic(p,"String rejection",stderr); vxn_free(p);
    }
    for(size_t i=0;i<sizeof utf8_cases/sizeof utf8_cases[0];++i) interpret(&utf8_cases[i]);
    for(size_t i=0;i<sizeof utf8_bad/sizeof utf8_bad[0];++i) {
        if(getenv("VXN_TRACE_TESTS")) fprintf(stderr,"rejection: utf8_bad %zu\n",i);
        VxnProgram *p=vxn_compile(utf8_bad[i].source); bool ok=p&&!vxn_ok(p)&&strstr(p->error,utf8_bad[i].error);
        expect(ok,utf8_bad[i].error); if(!ok&&p) vxn_diagnostic(p,"UTF-8 rejection",stderr); vxn_free(p);
    }
    for(size_t i=0;i<sizeof parse_cases/sizeof parse_cases[0];++i) interpret(&parse_cases[i]);
    for(size_t i=0;i<sizeof parse_bad/sizeof parse_bad[0];++i) {
        if(getenv("VXN_TRACE_TESTS")) fprintf(stderr,"rejection: parse_bad %zu\n",i);
        VxnProgram *p=vxn_compile(parse_bad[i].source); bool ok=p&&!vxn_ok(p)&&strstr(p->error,parse_bad[i].error);
        expect(ok,parse_bad[i].error); if(!ok&&p) vxn_diagnostic(p,"parse rejection",stderr); vxn_free(p);
    }
    slice_range_tests();
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) interpret(&cases[i]);
    for (size_t i = 0; i < sizeof feature_cases / sizeof feature_cases[0]; ++i) interpret(&feature_cases[i]);
    for (size_t i = 0; i < sizeof io_cases / sizeof io_cases[0]; ++i) interpret(&io_cases[i]);
    for(size_t i=0;i<sizeof reference_cases/sizeof reference_cases[0];++i) interpret(&reference_cases[i]);
    for(size_t i=0;i<sizeof library_cases/sizeof library_cases[0];++i) interpret(&library_cases[i]);
    for(size_t i=0;i<sizeof comptime_cases/sizeof comptime_cases[0];++i) interpret(&comptime_cases[i]);
    for(size_t i=0;i<sizeof call_cases/sizeof call_cases[0];++i) interpret(&call_cases[i]);
    for(size_t i=0;i<sizeof callable_cases/sizeof callable_cases[0];++i) interpret(&callable_cases[i]);
    for(size_t i=0;i<sizeof slice_cases/sizeof slice_cases[0];++i) interpret(&slice_cases[i]);
    for(size_t i=0;i<sizeof generic_cases/sizeof generic_cases[0];++i) interpret(&generic_cases[i]);
    for(size_t i=0;i<sizeof result_cases/sizeof result_cases[0];++i) interpret(&result_cases[i]);
    for(size_t i=0;i<sizeof insertion_cases/sizeof insertion_cases[0];++i) interpret(&insertion_cases[i]);
    for(size_t i=0;i<sizeof owner_cases/sizeof owner_cases[0];++i) interpret(&owner_cases[i]);
    for(size_t i=0;i<sizeof exclusive_cases/sizeof exclusive_cases[0];++i) interpret(&exclusive_cases[i]);
    for(size_t i=0;i<sizeof exclusive_bad/sizeof exclusive_bad[0];++i) {
        if(getenv("VXN_TRACE_TESTS")) fprintf(stderr,"rejection: exclusive_bad %zu\n",i);
        VxnProgram *p=vxn_compile(exclusive_bad[i].source); bool ok=p&&!vxn_ok(p)&&strstr(p->error,exclusive_bad[i].error);
        expect(ok,exclusive_bad[i].error); if(!ok&&p) vxn_diagnostic(p,"exclusive rejection",stderr); vxn_free(p);
    }
    for(size_t i=0;i<sizeof owner_bad/sizeof owner_bad[0];++i) {
        if(getenv("VXN_TRACE_TESTS")) fprintf(stderr,"rejection: owner_bad %zu\n",i);
        VxnProgram *p=vxn_compile(owner_bad[i].source); bool ok=p&&!vxn_ok(p)&&strstr(p->error,owner_bad[i].error);
        expect(ok,owner_bad[i].error); if(!ok&&p) vxn_diagnostic(p,"ownership rejection",stderr); vxn_free(p);
    }
    for(size_t i=0;i<sizeof result_bad/sizeof result_bad[0];++i) {
        if(getenv("VXN_TRACE_TESTS")) fprintf(stderr,"rejection: result_bad %zu\n",i);
        VxnProgram *p=vxn_compile(result_bad[i].source); bool ok=p&&!vxn_ok(p)&&strstr(p->error,result_bad[i].error);
        expect(ok,result_bad[i].error); if(!ok&&p) vxn_diagnostic(p,"result rejection",stderr); vxn_free(p);
    }
    for(size_t i=0;i<sizeof generic_bad/sizeof generic_bad[0];++i) {
        if(getenv("VXN_TRACE_TESTS")) fprintf(stderr,"rejection: generic_bad %zu\n",i);
        VxnProgram *p=vxn_compile(generic_bad[i].source); bool ok=p&&!vxn_ok(p)&&strstr(p->error,generic_bad[i].error);
        expect(ok,generic_bad[i].error); if(!ok&&p) vxn_diagnostic(p,"generic rejection",stderr); vxn_free(p);
    }
    for(size_t i=0;i<sizeof slice_bad/sizeof slice_bad[0];++i) {
        if(getenv("VXN_TRACE_TESTS")) fprintf(stderr,"rejection: slice_bad %zu\n",i);
        VxnProgram *p=vxn_compile(slice_bad[i].source); bool ok=p&&!vxn_ok(p)&&strstr(p->error,slice_bad[i].error);
        expect(ok,slice_bad[i].error); if(!ok&&p) vxn_diagnostic(p,"slice rejection",stderr); vxn_free(p);
    }
    for(size_t i=0;i<sizeof callable_bad/sizeof callable_bad[0];++i) {
        if(getenv("VXN_TRACE_TESTS")) fprintf(stderr,"rejection: callable_bad %zu\n",i);
        VxnProgram *p=vxn_compile(callable_bad[i].source); bool ok=p&&!vxn_ok(p)&&strstr(p->error,callable_bad[i].error);
        expect(ok,callable_bad[i].error); if(!ok&&p) vxn_diagnostic(p,"callable rejection",stderr); vxn_free(p);
    }
    for(size_t i=0;i<sizeof call_bad/sizeof call_bad[0];++i) {
        if(getenv("VXN_TRACE_TESTS")) fprintf(stderr,"rejection: call_bad %zu\n",i);
        VxnProgram *p=vxn_compile(call_bad[i].source); bool ok=p&&!vxn_ok(p)&&strstr(p->error,call_bad[i].error);
        expect(ok,call_bad[i].error); if(!ok&&p) vxn_diagnostic(p,"call rejection",stderr); vxn_free(p);
    }
    for(size_t i=0;i<sizeof comptime_bad/sizeof comptime_bad[0];++i) {
        if(getenv("VXN_TRACE_TESTS")) fprintf(stderr,"comptime rejection: %zu\n",i);
        VxnProgram *p=vxn_compile(comptime_bad[i].source);
        bool ok=p&&!vxn_ok(p)&&strstr(p->error,comptime_bad[i].error);
        expect(ok,comptime_bad[i].error);
        if(!ok&&p) vxn_diagnostic(p,"comptime rejection",stderr);
        vxn_free(p);
    }
    {
        VxnProgram *p=vxn_compile("comptime safe pure T answer[T](const T x){return x;}comptime safe pure T forward[T](const T x){return answer(x);}safe io void main(){output(forward(42));output(forward(true));}");
        expect(vxn_ok(p),"compile-time erasure fixture");
        if(vxn_ok(p)) for(int optimize=0;optimize<2;++optimize) for(int llvm=0;llvm<2;++llvm) {
            FILE *out=tmpfile(); expect(out!=NULL,"compile-time erasure stream"); if(!out) continue;
            expect((llvm?vxn_emit_llvm(p,out,optimize):vxn_emit_c(p,out,optimize))==0,"compile-time erasure emission");
            char symbol[64],line[4096]; bool retained=false;
            rewind(out); while(fgets(line,sizeof line,out)) {
                for(int i=p->builtin_functions;i<p->function_count;++i) if(p->functions[i]->comptime) {
                    snprintf(symbol,sizeof symbol,llvm?"@vxn_fn_%d(":"vxfn_%d(",i);
                    if(strstr(line,symbol)) retained=true;
                }
            }
            expect(!retained,"comptime function is absent even without optimization"); fclose(out);
        }
        vxn_free(p);
    }
    for(size_t i=0;i<sizeof library_bad/sizeof library_bad[0];++i) {
        if(getenv("VXN_TRACE_TESTS")) fprintf(stderr,"rejection: library_bad %zu\n",i);
        VxnProgram *p=vxn_compile(library_bad[i].source);
        bool ok=p&&!vxn_ok(p)&&strstr(p->error,library_bad[i].error);
        expect(ok,library_bad[i].error);
        if(!ok&&p) vxn_diagnostic(p,"library rejection",stderr);
        vxn_free(p);
    }
    for(size_t i=0;i<sizeof reference_bad/sizeof reference_bad[0];++i) {
        if(getenv("VXN_TRACE_TESTS")) fprintf(stderr,"rejection: reference_bad %zu\n",i);
        VxnProgram *p=vxn_compile(reference_bad[i].source);
        bool ok=p&&!vxn_ok(p)&&strstr(p->error,reference_bad[i].error);
        expect(ok,reference_bad[i].error);
        if(!ok&&p) { fprintf(stderr,"source: %s\n",reference_bad[i].source); vxn_diagnostic(p,"reference rejection",stderr); }
        vxn_free(p);
    }
    diagnostic_limit_tests();
    /* Mutate valid programs to exercise error recovery and cleanup. */
    for (int i = 0; i < 3000; ++i) {
        const char *source = i%11==10?exclusive_cases[random_word() % (sizeof exclusive_cases / sizeof exclusive_cases[0])].source:
            i%10==9?owner_cases[random_word() % (sizeof owner_cases / sizeof owner_cases[0])].source:
            i%9==8?result_cases[random_word() % (sizeof result_cases / sizeof result_cases[0])].source:
            i%8==7?generic_cases[random_word() % (sizeof generic_cases / sizeof generic_cases[0])].source:
            i%7==6?slice_cases[random_word() % (sizeof slice_cases / sizeof slice_cases[0])].source:
            i%6==5?call_cases[random_word() % (sizeof call_cases / sizeof call_cases[0])].source:
            i%5==4?comptime_cases[random_word() % (sizeof comptime_cases / sizeof comptime_cases[0])].source:
            i%4==3?reference_cases[random_word() % (sizeof reference_cases / sizeof reference_cases[0])].source:
            i%3==0?io_cases[random_word() % (sizeof io_cases / sizeof io_cases[0])].source:
            i%3==1?cases[random_word() % (sizeof cases / sizeof cases[0])].source:
            feature_cases[random_word() % (sizeof feature_cases / sizeof feature_cases[0])].source;
        char mutated[4096]; size_t len = strlen(source); memcpy(mutated,source,len+1);
        unsigned changes = 1 + random_word() % 6;
        while (changes--) mutated[random_word() % len] = (char)(1 + random_word() % 126);
        VxnProgram *p = vxn_compile(mutated); expect(p != NULL,"mutation allocation"); vxn_free(p);
    }
}

typedef struct { char dir[128], source[256], exe[256], out[256], err[256]; unsigned executables; } Files;
static bool setup(Files *f) {
    memset(f,0,sizeof *f);
    for (unsigned i = 0; i < 1000; ++i) {
        snprintf(f->dir,sizeof f->dir,"vxn test %lu-%u",(unsigned long)process_id(),i);
        if (!make_directory(f->dir)) {
            snprintf(f->source,sizeof f->source,"./%s/program.c",f->dir);
            snprintf(f->exe,sizeof f->exe,"./%s/program.exe",f->dir);
            snprintf(f->out,sizeof f->out,"./%s/stdout.txt",f->dir);
            snprintf(f->err,sizeof f->err,"./%s/stderr.txt",f->dir);
            return true;
        }
    }
    expect(false,"create isolated test directory"); return false;
}
/* Windows scanners can briefly retain an executable after its process exits.
   Only call this for explicit paths created in the current isolated fixture. */
static void remove_fixture(const char *path) {
#ifdef _WIN32
    for(unsigned attempt=0;remove(path)!=0&&errno!=ENOENT&&attempt<10;++attempt) Sleep(20);
#else
    (void)remove(path);
#endif
}
static void cleanup(const Files *f) {
    remove(f->source); remove(f->exe); remove(f->out); remove(f->err);
    for (unsigned i = 0; i < f->executables; ++i) {
        char path[256]; snprintf(path,sizeof path,"./%s/program_%u.exe",f->dir,i); remove(path);
        snprintf(path,sizeof path,"./%s/program_%u.exe.ll",f->dir,i); remove(path);
        snprintf(path,sizeof path,"./%s/program_%u.exe.obj",f->dir,i); remove(path);
        snprintf(path,sizeof path,"./%s/program_%u.pdb",f->dir,i); remove(path);
        if(unit_driver) for(int unit=0;unit<3;++unit) {
            snprintf(path,sizeof path,"./%s/program_%u.exe.unit%d.ll",f->dir,i,unit); remove_fixture(path);
            snprintf(path,sizeof path,"./%s/program_%u.exe.unit%d.obj",f->dir,i,unit); remove_fixture(path);
        }
    }
    if(unit_driver) {
        const char *names[]={"units.vxnp","dep.vxnp","dep.vxn"};
        for(size_t i=0;i<3;++i) { char path[256]; snprintf(path,sizeof path,"./%s/%s",f->dir,names[i]); remove_fixture(path); }
    }
    expect(remove_directory(f->dir) == 0,"remove test directory");
}
#include "packages.h"
static int build_case(const VxnProgram *p,const char *source,const char *cc,Files *f,int optimize) {
    /* Bound fixture storage while retaining distinct output names for Windows
       scanners. cleanup() retries any file still locked after its execution. */
    if(f->executables>1) {
        char previous[256]; unsigned id=f->executables-2;
        snprintf(previous,sizeof previous,"./%s/program_%u.exe",f->dir,id); remove(previous);
        snprintf(previous,sizeof previous,"./%s/program_%u.exe.ll",f->dir,id); remove(previous);
        snprintf(previous,sizeof previous,"./%s/program_%u.exe.obj",f->dir,id); remove(previous);
        snprintf(previous,sizeof previous,"./%s/program_%u.pdb",f->dir,id); remove(previous);
    }
    FILE *generated=fopen(f->source,"w");
    if(!generated) { expect(false,"open native input"); return 2; }
    int status=0;
    if(!llvm_driver&&owner_probe>=0) {
        fprintf(generated,"#ifndef _CRT_SECURE_NO_WARNINGS\n#define _CRT_SECURE_NO_WARNINGS\n#endif\n#include <stdlib.h>\n#include <stddef.h>\nstatic unsigned probe_live;\nstatic void probe_finish(void){if(probe_live)abort();}\nstatic void *probe_alloc(size_t n){static int started;if(!started){atexit(probe_finish);started=1;}if(probe_live==%d)return NULL;void *p=malloc(n);if(p)++probe_live;return p;}\nstatic void probe_free(void *p){if(!probe_live)abort();--probe_live;free(p);}\n#define VX_HEAP_ALLOC probe_alloc\n#define VX_HEAP_FREE probe_free\n",owner_probe);
    }
    if(llvm_driver) { if(fputs(source,generated)==EOF) status=2; }
    else status=vxn_emit_c(p,generated,optimize);
    expect(status==0,"write native input");
    int closed=fclose(generated); expect(closed==0,"close native input");
    if(closed) status=2;
    if(status) return status;
    const char *c[]={cc,"-std=c11","-Wall","-Wextra","-Wpedantic","-Werror",
#if defined(__linux__)
        "-D_POSIX_C_SOURCE=200809L",
#endif
        optimize?"-O2":"-O0",f->source,VXN_EXTENDED_PATH,
#ifdef _WIN32
        "-luser32","-lgdi32",
#elif defined(__linux__)
        "-lm",
#endif
        "-o",f->exe,NULL};
    const char *ir[]={llvm_driver,"build",f->source,"-o",f->exe,"--llvm",cc,
        optimize?"--backend":"--no-opt",optimize?"llvm":NULL,NULL};
    const char *fault_ir[]={llvm_driver,"build",f->source,"-o",f->exe,"--llvm",cc,"--runtime",owner_probe==2?VXN_LIMITED_RUNTIME:VXN_FAILING_RUNTIME,
        optimize?"--backend":"--no-opt",optimize?"llvm":NULL,NULL};
    if(unit_driver) {
        char manifest[256]; snprintf(manifest,sizeof manifest,"./%s/units.vxnp",f->dir);
        const char *units[]={llvm_driver,"build",f->source,"-o",f->exe,"--llvm",cc,"--manifest",manifest,
            optimize?"--backend":"--no-opt",optimize?"llvm":NULL,NULL};
        return vxn_process_redirect(units,f->out,f->err);
    }
    return vxn_process_redirect(llvm_driver?(owner_probe==0||owner_probe==2?fault_ir:ir):c,f->out,f->err);
}
static void native_case(const Case *c, const char *cc, Files *f) {
    VxnProgram *p = vxn_compile(c->source); expect(vxn_ok(p),c->label);
    if (!vxn_ok(p)) { vxn_diagnostic(p,c->label,stderr); vxn_free(p); return; }
    FILE *out = tmpfile(), *err = tmpfile();
    if (!out || !err) { expect(false,"tmpfile"); if(out) fclose(out); if(err) fclose(err); vxn_free(p); return; }
    int interpreted = vxn_run(p,out,err);
    char expected_out[8192], expected_err[8192], actual[8192];
    contents(out,expected_out,sizeof expected_out); contents(err,expected_err,sizeof expected_err);
    fclose(out); fclose(err);
    expect(interpreted == c->status && !strcmp(expected_out,c->out) && !strcmp(expected_err,c->err),c->label);
    for (int optimize = 0; optimize <= 1; ++optimize) {
        /* Windows scanners can briefly retain an exited executable. Use a
           different output for each build, rather than relinking over it. */
        snprintf(f->exe,sizeof f->exe,"./%s/program_%u.exe",f->dir,f->executables++);
        int status = build_case(p,c->source,cc,f,optimize);
        expect(status == 0,c->label);
        if (status) { read_file(f->err,actual,sizeof actual); fprintf(stderr,"%s\n",actual); break; }
        const char *run[] = {f->exe,NULL}; status = vxn_process_redirect(run,f->out,f->err);
        expect(status == interpreted,c->label);
        read_file(f->out,actual,sizeof actual); expect(!strcmp(actual,expected_out),c->label);
        read_file(f->err,actual,sizeof actual); expect(!strcmp(actual,expected_err),c->label);
    }
    vxn_free(p);
}
#include "io_tests.h"
static void owner_failure_tests(const char *cc,Files *f) {
    const char *empty[]={
        "safe io nopanic void main(){match string_new(){StringAlloc::Err(_)->output(7),_->output(0)}}",
        "safe io nopanic void main(){match string_from_str(\"source\"){StringAlloc::Err(_)->output(7),_->output(0)}}",
        "safe pure hot str literal()from(){return \"literal\";}safe io nopanic void main(){const str text=literal();output(str_byte_len(text));}",
        "safe io nopanic void main(){match box_new[uint32](42){BoxResult[uint32]::Ok(const b)->output(99),BoxResult[uint32]::Err(const e)->{match e{AllocError::OutOfMemory->output(7)}}}}",
        "safe io nopanic void main(){match box_fn[safe pure fn()->uint32](fn()->uint32{return 42;}){BoxResult[DynFn[safe pure fn()->uint32]]::Ok(const b)->output(999),BoxResult[DynFn[safe pure fn()->uint32]]::Err(const e)->output(7)}}"
    };
    VxnProgram *p;
    for(unsigned i=0;i<sizeof empty/sizeof empty[0];++i) {
        const char *source=empty[i]; p=vxn_compile(source); expect(vxn_ok(p),"allocation-failure program checks");
        if(!vxn_ok(p)) { vxn_diagnostic(p,"allocation failure",stderr); vxn_free(p); continue; }
        owner_probe=0;
        for(int optimize=0;optimize<2;++optimize) {
            snprintf(f->exe,sizeof f->exe,"./%s/program_%u.exe",f->dir,f->executables++);
            int status=build_case(p,source,cc,f,optimize); expect(status==0,"build failing allocator program");
            char buffer[8192];
            if(status) { read_file(f->err,buffer,sizeof buffer); fputs(buffer,stderr); continue; }
            const char *run[]={f->exe,NULL}; expect(vxn_process_redirect(run,f->out,f->err)==0,"allocation failure does not panic or abort");
            read_file(f->out,buffer,sizeof buffer); expect(!strcmp(buffer,"7\n"),"allocation failure reaches typed error arm");
            read_file(f->err,buffer,sizeof buffer); expect(!*buffer,"recoverable allocation failure stderr");
        }
        vxn_free(p);
    }
    const char *partial[]={
        "safe io nopanic void main(){match vec_new[Box[uint32]](){VecResult[Box[uint32]]::Ok(mut v)->{match box_new[uint32](42){BoxResult[uint32]::Ok(const b)->{match vec_insert(uniq mut v,0,b){VecInsert[Box[uint32]]::Allocation(_,const recovered)->output(box_ref(ref recovered).*),_->output(999)}output(vec_len(ref v));},_->output(999)}},_->output(999)}}",
        "safe io nopanic void main(){match string_from_str(\"abcd\"){StringAlloc::Ok(mut s)->{match string_append(uniq mut s,\"e\"){AllocResult::Err(_)->output(1),_->output(999)}output(str_equal(string_view(ref s),\"abcd\"));match string_push_scalar(uniq mut s,0x1f30d){StringEditResult::Err(StringEditError::Allocation(_))->output(1),_->output(999)}string_clear(uniq mut s);match string_append(uniq mut s,\"ok\"){AllocResult::Ok->output(1),_->output(999)}},_->output(999)}match string_from_str(\"freed\"){StringAlloc::Ok(const s)->output(string_byte_len(ref s)),_->output(999)}}",
        "safe io nopanic void main(){match string_new(){StringAlloc::Ok(const held)->{match string_from_str(\"buffer fails\"){StringAlloc::Err(_)->output(1),_->output(999)}output(string_byte_len(ref held));},_->output(999)}match string_from_str(\"clean\"){StringAlloc::Ok(const s)->output(string_byte_len(ref s)),_->output(999)}}",

        "safe io nopanic void main(){match vec_new[wrap uint8](){VecResult[wrap uint8]::Ok(mut v)->{vec_push(uniq mut v,65);match string_from_utf8(v){StringResult::Ok(const s)->{let const bytes=string_into_bytes(s);output(vec_len(ref bytes));},StringResult::Err(const e)->output(999)}},VecResult[wrap uint8]::Err(const e)->output(999)}match vec_new[wrap uint8](){VecResult[wrap uint8]::Ok(mut v)->{vec_push(uniq mut v,255);match string_from_utf8(v){StringResult::Ok(const s)->output(999),StringResult::Err(const e)->{let const bytes=e.bytes;output(vec_len(ref bytes));}}},VecResult[wrap uint8]::Err(const e)->output(999)}match box_new[uint32](42){BoxResult[uint32]::Ok(const b)->output(box_ref(ref b).*),BoxResult[uint32]::Err(const e)->output(999)}}",
        "safe nopanic BoxResult[Box[Box[uint32]]] make(){return box_new[Box[Box[uint32]]](box_new[Box[uint32]](box_new[uint32](42)?)?);}safe io nopanic void main(){match make(){BoxResult[Box[Box[uint32]]]::Ok(const b)->output(999),BoxResult[Box[Box[uint32]]]::Err(const e)->output(7)}}",
        "safe io nopanic void main(){match vec_new[uint32](){VecResult[uint32]::Ok(mut v)->{vec_push(uniq mut v,42);match vec_reserve(uniq mut v,100){AllocResult::Ok->output(999),AllocResult::Err(const e)->output(7)}output(len(v));match vec_pop(uniq mut v){VecPop[uint32]::Some(const n)->output(n),VecPop[uint32]::Empty->output(999)}},VecResult[uint32]::Err(const e)->output(999)}}",
        "safe io nopanic void main(){match vec_new[Box[uint32]](){VecResult[Box[uint32]]::Ok(mut v)->{match box_new[uint32](42){BoxResult[uint32]::Ok(const b)->{match vec_push(uniq mut v,b){AllocResult::Ok->output(999),AllocResult::Err(const e)->output(7)}output(len(v));},BoxResult[uint32]::Err(const e)->output(999)}},VecResult[Box[uint32]]::Err(const e)->output(999)}}",
        "safe nopanic BoxResult[DynFn[safe pure fn()->uint32]] make(){const Box[uint32] a=box_new[uint32](7)?;const Box[uint32] b=box_new[uint32](42)?;return box_fn[safe pure fn()->uint32](fn()->uint32{const uint32 first=box_ref(ref a).*;return box_ref(ref b).*;});}safe io nopanic void main(){match make(){BoxResult[DynFn[safe pure fn()->uint32]]::Ok(const b)->output(999),BoxResult[DynFn[safe pure fn()->uint32]]::Err(const e)->output(7)}match box_new[uint32](42){BoxResult[uint32]::Ok(const b)->output(box_ref(ref b).*),BoxResult[uint32]::Err(const e)->output(999)}}",
        "safe nopanic BoxResult[DynFn[safe pure fn()->uint32]] erase[F:Move&safe pure fn()->uint32](const F f){return box_fn[safe pure fn()->uint32](f);}safe io nopanic void main(){match box_new[uint32](42){BoxResult[uint32]::Ok(const owner)->{match erase(fn()->uint32{return box_ref(ref owner).*;}){BoxResult[DynFn[safe pure fn()->uint32]]::Ok(const b)->{match erase(b){BoxResult[DynFn[safe pure fn()->uint32]]::Ok(const c)->output(c()),BoxResult[DynFn[safe pure fn()->uint32]]::Err(const e)->output(999)}},BoxResult[DynFn[safe pure fn()->uint32]]::Err(const e)->output(999)}},BoxResult[uint32]::Err(const e)->output(999)}}"
    };
    const char *outputs[]={"42\n0\n","1\ntrue\n1\n1\n5\n","1\n0\n5\n","1\n1\n42\n","7\n","7\n1\n42\n","7\n0\n","7\n42\n","42\n"};
    owner_probe=2;
    for(unsigned i=0;i<sizeof partial/sizeof partial[0];++i) {
        p=vxn_compile(partial[i]); expect(vxn_ok(p),"partial allocation failure program checks");
        if(!vxn_ok(p)) { vxn_diagnostic(p,"partial allocation failure",stderr); vxn_free(p); continue; }
        for(int optimize=0;optimize<2;++optimize) {
            snprintf(f->exe,sizeof f->exe,"./%s/program_%u.exe",f->dir,f->executables++);
            int status=build_case(p,partial[i],cc,f,optimize); expect(status==0,"build partially failing allocator program");
            char buffer[8192]; if(status) { read_file(f->err,buffer,sizeof buffer); fputs(buffer,stderr); continue; }
            const char *run[]={f->exe,NULL};
            expect(vxn_process_redirect(run,f->out,f->err)==0,"partial allocation failure cleans ownership without abort");
            read_file(f->out,buffer,sizeof buffer); expect(!strcmp(buffer,outputs[i]),"partial failure preserves live values and reports error");
            read_file(f->err,buffer,sizeof buffer); expect(!*buffer,"partial allocation failure stderr");
        }
        vxn_free(p);
    }
    owner_probe=-1;
}
#include "backend_cases.h"
static uint32_t generate_expr(char *out, size_t cap, int depth) {
    if (!depth || (random_word() & 3u) == 0) {
        uint32_t value = random_word(); snprintf(out,cap,"%" PRIu32,value); return value;
    }
    char left[2048], right[2048];
    uint32_t a = generate_expr(left,sizeof left,depth-1), b = generate_expr(right,sizeof right,depth-1);
    size_t ln=strlen(left),rn=strlen(right);
    if(ln+rn+6>cap) {
        expect(false,"generated expression fits its buffer");
        if(cap) out[0]=0;
        return 0;
    }
    out[0]='('; memcpy(out+1,left,ln); memcpy(out+1+ln," + ",3);
    memcpy(out+4+ln,right,rn); out[4+ln+rn]=')'; out[5+ln+rn]=0;
    return (uint32_t)(((uint64_t)a + (uint64_t)b) & UINT64_C(0xffffffff));
}
static void emitted_library_tests(const char *cc,const Files *f) {
    const char *sources[]={
        "pub safe pure bool invert(const bool b) { return !b; }",
        "pub safe io nopanic IoResult greet(const IoFile file) { return io_write(file,b\"hello\"); }"
    };
    char object[256],error[8192]; snprintf(object,sizeof object,"./%s/library.obj",f->dir);
    for(unsigned i=0;i<sizeof sources/sizeof sources[0];++i) {
        VxnProgram *p=vxn_compile(sources[i]); expect(vxn_ok(p),"library compiles without main");
        if(!vxn_ok(p)) { vxn_free(p); continue; }
        FILE *file=fopen(f->source,"w");
        if(!file) { expect(false,"library C output"); vxn_free(p); continue; }
        expect((llvm_driver?vxn_emit_llvm(p,file,1):vxn_emit_c(p,file,1))==0,"emit library without main");
        expect(fclose(file)==0,"close library C");
        const char *command[]={cc,"-std=c11","-Wall","-Wextra","-Wpedantic","-Werror","-c",f->source,"-o",object,NULL};
        const char *ir[]={cc,"-x","ir","-Werror","-Wno-override-module","-c",f->source,"-o",object,NULL};
        int status=vxn_process_redirect(llvm_driver?ir:command,f->out,f->err);
        expect(status==0,"library object compiles without main");
        if(status) { read_file(f->err,error,sizeof error); fputs(error,stderr); }
        vxn_free(p);
    }
    remove(object);
}
static void driver_tests(const char *driver, const char *cc, const Files *f) {
    char input[256], executable[256], generated[260], object[260], buffer[65536];
    snprintf(input,sizeof input,"./%s/source & args.vxn",f->dir);
    snprintf(executable,sizeof executable,"./%s/driver & result.exe",f->dir);
    snprintf(generated,sizeof generated,"%s%s",executable,llvm_driver?".ll":".c");
    snprintf(object,sizeof object,"%s.obj",executable);
    FILE *file = fopen(input,"w"); if (!file) { expect(false,"driver source file"); return; }
    const char *source = "safe io void main() { output(40 + 2); }";
    fputs(source,file); fclose(file);
    const char *check[] = {driver,"check",input,NULL};
    expect(vxn_process_redirect(check,f->out,f->err) == 0,"driver check");
    read_file(f->out,buffer,sizeof buffer); expect(strstr(buffer,"io, alloc, block") != NULL,"effect report");
    const char *run[] = {driver,"run",input,NULL};
    expect(vxn_process_redirect(run,f->out,f->err) == 0,"driver run");
    read_file(f->out,buffer,sizeof buffer); expect(!strcmp(buffer,"42\n"),"driver interpreter output");
    const char *emit[] = {driver,llvm_driver?"emit-llvm":"emit-c",input,"--no-opt",NULL};
    expect(vxn_process_redirect(emit,f->out,f->err) == 0,"driver emit stdout");
    expect(has_line(f->out,llvm_driver?"define i32 @main()":"int main(void)"),"driver emission");
    const char *build[] = {driver,"build",input,"-o",executable,llvm_driver?"--llvm":"--cc",cc,
        llvm_driver?NULL:"--backend","c",NULL};
    expect(vxn_process_redirect(build,f->out,f->err) == 0,"driver build with spaces and ampersand");
    if(llvm_driver) {
        char cpath[260]; snprintf(cpath,sizeof cpath,"%s.c",executable);
        FILE *unexpected=fopen(cpath,"r");
        expect(!unexpected,"default LLVM build produces no C source");
        if(unexpected) fclose(unexpected);
    }
    const char *native[] = {executable,NULL};
    expect(vxn_process_redirect(native,f->out,f->err) == 0,"driver executable");
    read_file(f->out,buffer,sizeof buffer); expect(!strcmp(buffer,"42\n"),"driver native output");
    const char *overwrite[] = {driver,llvm_driver?"emit-llvm":"emit-c",input,"-o",input,NULL};
    expect(vxn_process_redirect(overwrite,f->out,f->err) == 2,"source overwrite rejected");
    read_file(input,buffer,sizeof buffer); expect(!strcmp(buffer,source),"source preserved");
    const char *missing_cc[] = {driver,"build",input,"-o",executable,llvm_driver?"--llvm":"--cc","vxn-no-such-compiler-125839",
        llvm_driver?NULL:"--backend","c",NULL};
    expect(vxn_process_redirect(missing_cc,f->out,f->err) == 2,"missing native compiler");
    if(llvm_driver) {
        read_file(f->err,buffer,sizeof buffer);
        expect(strstr(buffer,"LLVM compilation failed")!=NULL,"missing LLVM reports failure without C fallback");
        file=fopen(generated,"w");
        if(file) { fputs(source,file); fclose(file); }
        const char *collision[]={driver,"build",generated,"-o",executable,NULL};
        expect(vxn_process_redirect(collision,f->out,f->err)==2,"generated IR cannot overwrite its source");
        read_file(generated,buffer,sizeof buffer); expect(!strcmp(buffer,source),"colliding source remains intact");
        const char *runtime_alias[]={driver,"build",input,"-o",executable,"--runtime",executable,NULL};
        expect(vxn_process_redirect(runtime_alias,f->out,f->err)==2,"runtime cannot alias executable output");
        const char *bad_runtime[]={driver,"build",input,"-o",executable,"--runtime",input,NULL};
        expect(vxn_process_redirect(bad_runtime,f->out,f->err)==2,"runtime cannot alias source");
        const char *link_failure[]={driver,"build",input,"-o",executable,"--runtime","vxn-no-such-runtime-915137.a",NULL};
        expect(vxn_process_redirect(link_failure,f->out,f->err)==2,"missing runtime link fails");
        read_file(f->err,buffer,sizeof buffer);
        expect(strstr(buffer,"vxn-no-such-runtime-915137.a")!=NULL,"nested linker diagnostics reach caller");
    }
    file = fopen(input,"wb"); if (file) { fwrite("safe\0pure",1,9,file); fclose(file); }
    expect(vxn_process_redirect(check,f->out,f->err) == 2,"embedded NUL source rejected");
    remove(input); remove(executable); remove(generated); remove(object);
    snprintf(object,sizeof object,"./%s/driver & result.pdb",f->dir); remove(object);
}
#include "module_driver.h"
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1],"--echo-args")) {
        for (int i = 2; i < argc; ++i) printf("%zu:%s\n",strlen(argv[i]),argv[i]);
        return 0;
    }
    if(argc==4 && (!strcmp(argv[1],"--module-native")||!strcmp(argv[1],"--module-llvm"))) {
        if(!strcmp(argv[1],"--module-llvm")) llvm_driver=argv[2];
        Files f; if(setup(&f)) { module_driver_tests(argv[2],argv[3],&f); cleanup(&f); }
    }
    else if(argc==4 && (!strcmp(argv[1],"--slice-native")||!strcmp(argv[1],"--slice-llvm"))) {
        if(!strcmp(argv[1],"--slice-llvm")) llvm_driver=argv[2];
        Files f;
        if(setup(&f)) {
            for(size_t i=0;i<sizeof slice_cases/sizeof slice_cases[0];++i) native_case(&slice_cases[i],argv[3],&f);
            cleanup(&f);
        }
    }
    else if(argc==4 && (!strcmp(argv[1],"--io-native")||!strcmp(argv[1],"--io-llvm"))) {
        if(!strcmp(argv[1],"--io-llvm")) llvm_driver=argv[2];
        io_file_tests(argv[3]); io_stream_tests(argv[3]);
    }
    else if(argc==4 && (!strcmp(argv[1],"--exclusive-native")||!strcmp(argv[1],"--exclusive-llvm"))) {
        if(strstr(argv[1],"-llvm")) llvm_driver=argv[2];
        Files f; if(setup(&f)) {
            for(size_t i=0;i<sizeof exclusive_cases/sizeof exclusive_cases[0];++i) native_case(&exclusive_cases[i],argv[3],&f);
            cleanup(&f);
        }
    }
    else if(argc==4 && (!strcmp(argv[1],"--owner-native")||!strcmp(argv[1],"--owner-llvm"))) {
        if(strstr(argv[1],"-llvm")) llvm_driver=argv[2];
        Files f; if(setup(&f)) {
            owner_probe=4096; /* Deep recursive fixtures legitimately retain thousands of owners. */
            for(size_t i=0;i<sizeof insertion_cases/sizeof insertion_cases[0];++i) native_case(&insertion_cases[i],argv[3],&f);
            for(size_t i=0;i<sizeof owner_cases/sizeof owner_cases[0];++i) native_case(&owner_cases[i],argv[3],&f);
            owner_probe=-1;
            owner_failure_tests(argv[3],&f);
            cleanup(&f);
        }
    }
    else if(argc==4 && (!strcmp(argv[1],"--insert-native")||!strcmp(argv[1],"--insert-llvm"))) {
        if(strstr(argv[1],"-llvm")) llvm_driver=argv[2];
        Files f; if(setup(&f)) {
            owner_probe=4096;
            for(size_t i=0;i<sizeof insertion_cases/sizeof insertion_cases[0];++i) native_case(&insertion_cases[i],argv[3],&f);
            owner_probe=-1; owner_failure_tests(argv[3],&f); cleanup(&f);
        }
    }
    else if(argc==4 && (!strcmp(argv[1],"--allocation-native")||!strcmp(argv[1],"--allocation-llvm"))) {
        if(strstr(argv[1],"-llvm")) llvm_driver=argv[2];
        Files f; if(setup(&f)) { owner_failure_tests(argv[3],&f); cleanup(&f); }
    }
    else if(argc==4 && (!strcmp(argv[1],"--generic-native")||!strcmp(argv[1],"--generic-llvm")||!strcmp(argv[1],"--result-native")||!strcmp(argv[1],"--result-llvm"))) {
        if(strstr(argv[1],"-llvm")) llvm_driver=argv[2];
        Files f; if(setup(&f)) {
            for(size_t i=0;i<sizeof generic_cases/sizeof generic_cases[0];++i) native_case(&generic_cases[i],argv[3],&f);
            for(size_t i=0;i<sizeof result_cases/sizeof result_cases[0];++i) native_case(&result_cases[i],argv[3],&f);
            cleanup(&f);
        }
    }
    else if(argc==4 && (!strcmp(argv[1],"--builder-native")||!strcmp(argv[1],"--builder-llvm"))) {
        if(strstr(argv[1],"-llvm")) llvm_driver=argv[2];
        Files f; if(setup(&f)) {
            owner_probe=4096;
            for(size_t i=0;i<sizeof builder_cases/sizeof builder_cases[0];++i) native_case(&builder_cases[i],argv[3],&f);
            owner_probe=-1; cleanup(&f);
        }
    }
    else if(argc==4 && !strcmp(argv[1],"--unit-llvm")) {
        llvm_driver=argv[2]; unit_driver=true;
        Files f; if(setup(&f)) {
            package_file(&f,"units.vxnp","package unit_fixture\nmodule app=\"program.c\"\ndependency extra=\"dep.vxnp\"\n");
            package_file(&f,"dep.vxnp","package unit_support\nmodule extra=\"dep.vxn\"\n");
            package_file(&f,"dep.vxn","pub safe pure void package_noop(){}");
            for(size_t i=0;i<sizeof tail_cases/sizeof tail_cases[0];++i) native_case(&tail_cases[i],argv[3],&f);
            for(size_t i=0;i<sizeof memory_cases/sizeof memory_cases[0];++i) native_case(&memory_cases[i],argv[3],&f);
            for(size_t i=0;i<sizeof extended_cases/sizeof extended_cases[0];++i) native_case(&extended_cases[i],argv[3],&f);
            for(size_t i=0;i<sizeof borrowed_aggregate_cases/sizeof borrowed_aggregate_cases[0];++i) native_case(&borrowed_aggregate_cases[i],argv[3],&f);
            for(size_t i=0;i<sizeof split_cases/sizeof split_cases[0];++i) native_case(&split_cases[i],argv[3],&f);
            cleanup(&f);
        }
        unit_driver=false;
    }
    else if(argc==4 && (!strcmp(argv[1],"--recursive-native")||!strcmp(argv[1],"--recursive-llvm")||!strcmp(argv[1],"--recursive-unit-llvm")||!strcmp(argv[1],"--kinds-native")||!strcmp(argv[1],"--kinds-llvm")||!strcmp(argv[1],"--kinds-unit-llvm")||!strcmp(argv[1],"--partial-native")||!strcmp(argv[1],"--partial-llvm")||!strcmp(argv[1],"--partial-unit-llvm")||!strcmp(argv[1],"--equations-native")||!strcmp(argv[1],"--equations-llvm")||!strcmp(argv[1],"--equations-unit-llvm")||!strcmp(argv[1],"--lazy-native")||!strcmp(argv[1],"--lazy-llvm")||!strcmp(argv[1],"--lazy-unit-llvm"))) {
        if(strstr(argv[1],"-llvm")) llvm_driver=argv[2];
        unit_driver=strstr(argv[1],"-unit-llvm")!=NULL;
        Files f; if(setup(&f)) {
            if(unit_driver) {
                package_file(&f,"units.vxnp","package lazy_fixture\nmodule app=\"program.c\"\ndependency extra=\"dep.vxnp\"\n");
                package_file(&f,"dep.vxnp","package lazy_support\nmodule extra=\"dep.vxn\"\n");
                package_file(&f,"dep.vxn","pub safe pure void package_noop(){}");
            }
            owner_probe=2;
            if(strstr(argv[1],"--recursive")) {
                for(size_t i=0;i<sizeof recursive_lazy_cases/sizeof recursive_lazy_cases[0];++i) native_case(&recursive_lazy_cases[i],argv[3],&f);
            } else if(strstr(argv[1],"--kinds")) {
                for(size_t i=0;i<sizeof kind_cases/sizeof kind_cases[0];++i) native_case(&kind_cases[i],argv[3],&f);
            } else if(strstr(argv[1],"--partial")) {
                for(size_t i=0;i<sizeof partial_cases/sizeof partial_cases[0];++i) native_case(&partial_cases[i],argv[3],&f);
            } else if(strstr(argv[1],"--equations")) {
                for(size_t i=0;i<sizeof equation_cases/sizeof equation_cases[0];++i) native_case(&equation_cases[i],argv[3],&f);
            } else for(size_t i=0;i<sizeof lazy_cases/sizeof lazy_cases[0];++i) native_case(&lazy_cases[i],argv[3],&f);
            owner_probe=-1; cleanup(&f);
        }
        unit_driver=false;
    }
    else if(argc==4 && (!strcmp(argv[1],"--memory-native")||!strcmp(argv[1],"--memory-llvm"))) {
        if(strstr(argv[1],"-llvm")) llvm_driver=argv[2];
        Files f; if(setup(&f)) {
            owner_probe=4096;
            for(size_t i=0;i<sizeof memory_cases/sizeof memory_cases[0];++i) native_case(&memory_cases[i],argv[3],&f);
            owner_probe=-1; cleanup(&f);
        }
    }
    else if(argc==4 && (!strcmp(argv[1],"--borrowed-native")||!strcmp(argv[1],"--borrowed-llvm"))) {
        if(strstr(argv[1],"-llvm")) llvm_driver=argv[2];
        Files f; if(setup(&f)) {
            for(size_t i=0;i<sizeof borrowed_aggregate_cases/sizeof borrowed_aggregate_cases[0];++i) native_case(&borrowed_aggregate_cases[i],argv[3],&f);
            for(size_t i=0;i<sizeof returned_exclusive_cases/sizeof returned_exclusive_cases[0];++i) native_case(&returned_exclusive_cases[i],argv[3],&f);
            for(size_t i=0;i<sizeof split_cases/sizeof split_cases[0];++i) native_case(&split_cases[i],argv[3],&f);
            cleanup(&f);
        }
    }
    else if(argc==4 && (!strcmp(argv[1],"--extended-native")||!strcmp(argv[1],"--extended-llvm"))) {
        if(strstr(argv[1],"-llvm")) llvm_driver=argv[2];
        Files f; if(setup(&f)) {
            for(size_t i=0;i<sizeof extended_cases/sizeof extended_cases[0];++i) native_case(&extended_cases[i],argv[3],&f);
            cleanup(&f);
        }
    }
    else if(argc==4 && (!strcmp(argv[1],"--range-native")||!strcmp(argv[1],"--range-llvm"))) {
        if(strstr(argv[1],"-llvm")) llvm_driver=argv[2];
        Files f; if(setup(&f)) {
            for(size_t i=0;i<sizeof range_cases/sizeof range_cases[0];++i) native_case(&range_cases[i],argv[3],&f);
            cleanup(&f);
        }
    }
    else if(argc==4 && (!strcmp(argv[1],"--tail-native")||!strcmp(argv[1],"--tail-llvm"))) {
        if(strstr(argv[1],"-llvm")) llvm_driver=argv[2];
        Files f; if(setup(&f)) {
            owner_probe=8;
            for(size_t i=0;i<sizeof tail_cases/sizeof tail_cases[0];++i) native_case(&tail_cases[i],argv[3],&f);
            owner_probe=2;
            for(size_t i=0;i<sizeof tail_cases/sizeof tail_cases[0];++i)
                if(!strcmp(tail_cases[i].label,"owned tail argument and discarded temporary cleanup")||!strcmp(tail_cases[i].label,"tail boxed environment loop reclaims shells")||!strcmp(tail_cases[i].label,"tail concrete environment loop with moved owner")) native_case(&tail_cases[i],argv[3],&f);
            owner_probe=8;
            const Case stresses[]={
                {"million native tail transfers","safe pure bool even(const wrap uint32 n){if(n==0){return true;}return tail odd(n-1);}safe pure bool odd(const wrap uint32 n){if(n==0){return false;}return tail even(n-1);}safe io void main(){output(even(1000000));}","true\n","",0},
                {"million dynamic native tail transfers","safe pure bool even(const wrap uint32 n){if(n==0){return true;}const safe pure fn(const wrap uint32)->bool next=odd;return tail next(n-1);}safe pure bool odd(const wrap uint32 n){if(n==0){return false;}const safe pure fn(const wrap uint32)->bool next=even;return tail next(n-1);}safe io void main(){output(even(1000000));}","true\n","",0}
                ,{"million captured native tail transfers","safe uint32 hop(const Box[uint32] owner,const wrap uint32 n){let const f=fn()->uint32{if(n==0){return box_into_inner(owner);}return tail hop(owner,n-1);};return tail f();}safe io void main(){match box_new[uint32](42){BoxResult[uint32]::Ok(const b)->output(hop(b,500000)),_->panic()}}","42\n","",0}
            };
            for(size_t sample=0;sample<sizeof stresses/sizeof stresses[0];++sample) {
            const Case stress=stresses[sample];
            VxnProgram *p=vxn_compile(stress.source); expect(vxn_ok(p),stress.label);
            if(vxn_ok(p)) for(int optimize=0;optimize<2;++optimize) {
                snprintf(f.exe,sizeof f.exe,"./%s/program_%u.exe",f.dir,f.executables++);
                int status=build_case(p,stress.source,argv[3],&f,optimize); expect(status==0,stress.label);
                char buffer[8192];
                if(status) { read_file(f.err,buffer,sizeof buffer); fputs(buffer,stderr); continue; }
                const char *run[]={f.exe,NULL}; expect(vxn_process_redirect(run,f.out,f.err)==0,stress.label);
                read_file(f.out,buffer,sizeof buffer); expect(!strcmp(buffer,stress.out),stress.label);
                read_file(f.err,buffer,sizeof buffer); expect(!*buffer,stress.label);
            }
            vxn_free(p);
            }
            owner_probe=-1; cleanup(&f);
        }
    }
    else if(argc==4 && (!strcmp(argv[1],"--pattern-native")||!strcmp(argv[1],"--pattern-llvm"))) {
        if(strstr(argv[1],"-llvm")) llvm_driver=argv[2];
        Files f; if(setup(&f)) {
            owner_probe=4096;
            for(size_t i=0;i<sizeof pattern_cases/sizeof pattern_cases[0];++i) native_case(&pattern_cases[i],argv[3],&f);
            owner_probe=-1; cleanup(&f);
        }
    }
    else if(argc==4 && (!strcmp(argv[1],"--text-native")||!strcmp(argv[1],"--text-llvm"))) {
        if(strstr(argv[1],"-llvm")) llvm_driver=argv[2];
        Files f; if(setup(&f)) {
            owner_probe=4096;
            for(size_t i=0;i<sizeof text_cases/sizeof text_cases[0];++i) native_case(&text_cases[i],argv[3],&f);
            owner_probe=-1; cleanup(&f);
        }
    }
    else if(argc==4 && (!strcmp(argv[1],"--string-native")||!strcmp(argv[1],"--string-llvm"))) {
        if(strstr(argv[1],"-llvm")) llvm_driver=argv[2];
        Files f; if(setup(&f)) {
            owner_probe=4096;
            for(size_t i=0;i<sizeof string_cases/sizeof string_cases[0];++i) native_case(&string_cases[i],argv[3],&f);
            owner_probe=-1; cleanup(&f);
        }
    }
    else if(argc==4 && (!strcmp(argv[1],"--utf8-native")||!strcmp(argv[1],"--utf8-llvm"))) {
        if(strstr(argv[1],"-llvm")) llvm_driver=argv[2];
        Files f; if(setup(&f)) {
            for(size_t i=0;i<sizeof utf8_cases/sizeof utf8_cases[0];++i) native_case(&utf8_cases[i],argv[3],&f);
            cleanup(&f);
        }
    }
    else if(argc==4 && (!strcmp(argv[1],"--parse-native")||!strcmp(argv[1],"--parse-llvm"))) {
        if(strstr(argv[1],"-llvm")) llvm_driver=argv[2];
        Files f; if(setup(&f)) {
            owner_probe=4096;
            for(size_t i=0;i<sizeof parse_cases/sizeof parse_cases[0];++i) native_case(&parse_cases[i],argv[3],&f);
            owner_probe=-1; cleanup(&f);
        }
    }
    else if(argc==4 && (!strcmp(argv[1],"--comptime-native")||!strcmp(argv[1],"--comptime-llvm"))) {
        if(strstr(argv[1],"-llvm")) llvm_driver=argv[2];
        Files f; if(setup(&f)) {
            for(size_t i=0;i<sizeof comptime_cases/sizeof comptime_cases[0];++i) native_case(&comptime_cases[i],argv[3],&f);
            cleanup(&f);
        }
    }
    else if(argc==4 && (!strcmp(argv[1],"--callable-native")||!strcmp(argv[1],"--callable-llvm"))) {
        if(strstr(argv[1],"-llvm")) llvm_driver=argv[2];
        Files f; if(setup(&f)) {
            owner_probe=4096;
            for(size_t i=0;i<sizeof callable_cases/sizeof callable_cases[0];++i) native_case(&callable_cases[i],argv[3],&f);
            owner_probe=-1; cleanup(&f);
        }
    }
    else if(argc==2 && !strcmp(argv[1],"--limits")) diagnostic_limit_tests();
    else if(argc==2 && !strcmp(argv[1],"--lazy")) lazy_tests();
    else if(argc==2 && !strcmp(argv[1],"--equations")) equation_tests();
    else if(argc==2 && !strcmp(argv[1],"--partial")) partial_tests();
    else if(argc==2 && !strcmp(argv[1],"--kinds")) kind_tests();
    else if(argc==2 && !strcmp(argv[1],"--recursive")) recursive_lazy_tests();
    else if (argc == 1) {
        package_tests();
        for(size_t i=0;i<sizeof memory_cases/sizeof memory_cases[0];++i) interpret(&memory_cases[i]);
        for(size_t i=0;i<sizeof memory_bad/sizeof memory_bad[0];++i) {
            VxnProgram *p=vxn_compile(memory_bad[i].source);
            bool ok=p&&!vxn_ok(p)&&strstr(p->error,memory_bad[i].error);
            expect(ok,memory_bad[i].error); if(!ok) { fprintf(stderr,"source: %s\n",memory_bad[i].source); if(p) vxn_diagnostic(p,"memory rejection",stderr); } vxn_free(p);
        }
        for(size_t i=0;i<sizeof split_cases/sizeof split_cases[0];++i) interpret(&split_cases[i]);
        for(size_t i=0;i<sizeof split_bad/sizeof split_bad[0];++i) {
            VxnProgram *p=vxn_compile(split_bad[i].source);
            bool ok=p&&!vxn_ok(p)&&strstr(p->error,split_bad[i].error);
            expect(ok,split_bad[i].error); if(!ok) { fprintf(stderr,"source: %s\n",split_bad[i].source); if(p) vxn_diagnostic(p,"split rejection",stderr); } vxn_free(p);
        }
        for(size_t i=0;i<sizeof returned_exclusive_cases/sizeof returned_exclusive_cases[0];++i) interpret(&returned_exclusive_cases[i]);
        for(size_t i=0;i<sizeof returned_exclusive_bad/sizeof returned_exclusive_bad[0];++i) {
            VxnProgram *p=vxn_compile(returned_exclusive_bad[i].source);
            bool ok=p&&!vxn_ok(p)&&strstr(p->error,returned_exclusive_bad[i].error);
            expect(ok,returned_exclusive_bad[i].error); if(!ok) { fprintf(stderr,"source: %s\n",returned_exclusive_bad[i].source); if(p) vxn_diagnostic(p,"exclusive return rejection",stderr); } vxn_free(p);
        }
        for(size_t i=0;i<sizeof borrowed_aggregate_cases/sizeof borrowed_aggregate_cases[0];++i) interpret(&borrowed_aggregate_cases[i]);
        for(size_t i=0;i<sizeof borrowed_aggregate_bad/sizeof borrowed_aggregate_bad[0];++i) {
            VxnProgram *p=vxn_compile(borrowed_aggregate_bad[i].source);
            bool ok=p&&!vxn_ok(p)&&strstr(p->error,borrowed_aggregate_bad[i].error);
            expect(ok,borrowed_aggregate_bad[i].error); if(!ok) { fprintf(stderr,"source: %s\n",borrowed_aggregate_bad[i].source); if(p) vxn_diagnostic(p,"borrowed aggregate rejection",stderr); } vxn_free(p);
        }
        for(size_t i=0;i<sizeof extended_cases/sizeof extended_cases[0];++i) interpret(&extended_cases[i]);
        for(size_t i=0;i<sizeof extended_bad/sizeof extended_bad[0];++i) {
            VxnProgram *p=vxn_compile(extended_bad[i].source);
            bool ok=!vxn_ok(p)&&strstr(p->error,extended_bad[i].error);
            expect(ok,extended_bad[i].error); if(!ok&&p) vxn_diagnostic(p,"extended rejection",stderr); vxn_free(p);
        }
        core_tests(); io_file_tests(NULL); io_stream_tests(NULL);
        for(size_t i=0;i<sizeof backend_cases/sizeof backend_cases[0];++i) interpret(&backend_cases[i]);
    }
    else if (argc == 6 && (!strcmp(argv[1],"--native")||!strcmp(argv[1],"--llvm")) && !strcmp(argv[3],"--driver")) {
        if(!strcmp(argv[1],"--llvm")) llvm_driver=argv[4];
        /* argc includes an explicit launcher executable supplied by CMake. */
        Files f;
        if (setup(&f)) {
            const char *args[] = {argv[5],"--echo-args","two words","a\"b","trail\\","&;$(literal)","",NULL};
            expect(vxn_process_redirect(args,f.out,f.err) == 0,"argument launcher");
            char got[1024]; read_file(f.out,got,sizeof got);
            expect(!strcmp(got,"9:two words\n3:a\"b\n6:trail\\\n12:&;$(literal)\n0:\n"),"argv preserved without shell");
            for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) native_case(&cases[i],argv[2],&f);
            for (size_t i = 0; i < sizeof feature_cases / sizeof feature_cases[0]; ++i) native_case(&feature_cases[i],argv[2],&f);
            for (size_t i = 0; i < sizeof io_cases / sizeof io_cases[0]; ++i) native_case(&io_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof reference_cases/sizeof reference_cases[0];++i) native_case(&reference_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof library_cases/sizeof library_cases[0];++i) native_case(&library_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof comptime_cases/sizeof comptime_cases[0];++i) native_case(&comptime_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof call_cases/sizeof call_cases[0];++i) native_case(&call_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof callable_cases/sizeof callable_cases[0];++i) native_case(&callable_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof slice_cases/sizeof slice_cases[0];++i) native_case(&slice_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof generic_cases/sizeof generic_cases[0];++i) native_case(&generic_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof result_cases/sizeof result_cases[0];++i) native_case(&result_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof exclusive_cases/sizeof exclusive_cases[0];++i) native_case(&exclusive_cases[i],argv[2],&f);
            owner_probe=4096; /* Deep recursive fixtures legitimately retain thousands of owners. */
            for(size_t i=0;i<sizeof insertion_cases/sizeof insertion_cases[0];++i) native_case(&insertion_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof owner_cases/sizeof owner_cases[0];++i) native_case(&owner_cases[i],argv[2],&f);
            owner_probe=-1;
            owner_failure_tests(argv[2],&f);
            for(size_t i=0;i<sizeof backend_cases/sizeof backend_cases[0];++i) native_case(&backend_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof parse_cases/sizeof parse_cases[0];++i) native_case(&parse_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof utf8_cases/sizeof utf8_cases[0];++i) native_case(&utf8_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof string_cases/sizeof string_cases[0];++i) native_case(&string_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof text_cases/sizeof text_cases[0];++i) native_case(&text_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof pattern_cases/sizeof pattern_cases[0];++i) native_case(&pattern_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof builder_cases/sizeof builder_cases[0];++i) native_case(&builder_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof lazy_cases/sizeof lazy_cases[0];++i) native_case(&lazy_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof equation_cases/sizeof equation_cases[0];++i) native_case(&equation_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof partial_cases/sizeof partial_cases[0];++i) native_case(&partial_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof kind_cases/sizeof kind_cases[0];++i) native_case(&kind_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof recursive_lazy_cases/sizeof recursive_lazy_cases[0];++i) native_case(&recursive_lazy_cases[i],argv[2],&f);
            for(size_t i=0;i<sizeof tail_cases/sizeof tail_cases[0];++i) native_case(&tail_cases[i],argv[2],&f);
            native_numeric_tests(argv[2],&f);
            for(int assign=0;assign<2;++assign) {
                char source[4096]; size_t n=(size_t)sprintf(source,"safe io void main() { mut wrap uint8[256] a = {");
                for(int i=0;i<256;++i) n+=(size_t)sprintf(source+n,"%s0",i?",":"");
                snprintf(source+n,sizeof source-n,"}; const wrap sint8 index = -1; %s }",assign?"a[index] = 1;":"output(a[index]);");
                Case c={"negative narrow array index",source,"","panic\n",1}; native_case(&c,argv[2],&f);
            }
            for (int i = 0; i < 16; ++i) {
                char expr[4096], source[8192], output[64];
                uint32_t value = generate_expr(expr,sizeof expr,5);
                snprintf(source,sizeof source,"safe io void main() { const wrap uint32 result = %s; output(result); }",expr);
                snprintf(output,sizeof output,"%" PRIu32 "\n",value);
                Case c = {"generated arithmetic",source,output,"",0}; native_case(&c,argv[2],&f);
            }
            driver_tests(argv[4],argv[2],&f); module_driver_tests(argv[4],argv[2],&f); emitted_library_tests(argv[2],&f); cleanup(&f);
            io_file_tests(argv[2]); io_stream_tests(argv[2]);
        }
    } else { fputs("invalid test arguments\n",stderr); return 2; }
    printf("%u checks, %u failures\n",checks,failures);
    if(fflush(stdout)) return 1;
    return failures ? 1 : 0;
}
