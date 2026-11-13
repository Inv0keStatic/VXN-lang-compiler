#include "internal.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "vxn_io_declarations.h"
#include "vxn_graphics_declarations.h"
#include "vxn_window_declarations.h"
#include "vxn_time_declarations.h"
#include "vxn_memory_declarations.h"
#include "vxn_parse_declarations.h"
#include "vxn_text_declarations.h"
#include "vxn_lazy_declarations.h"
#include "../runtime/utf8.h"

_Noreturn void vx_fail(VxnProgram *p, Token at, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(p->error, sizeof p->error, format, args);
    va_end(args);
    p->error_pos = at.start;
    p->error_length = at.length ? at.length : 1;
    longjmp(p->failure, 1);
}

void *vx_alloc(VxnProgram *p, size_t bytes) {
    Token at = {0};
    if (bytes > VX_MAX_MEMORY - p->memory)
        vx_fail(p, at, "compiler allocation limit exceeded (64 MiB)");
    Allocation *a = malloc(sizeof *a);
    if (!a) vx_fail(p, at, "out of memory");
    a->memory = calloc(1, bytes ? bytes : 1);
    if (!a->memory) { free(a); vx_fail(p, at, "out of memory"); }
    a->next = p->allocations;
    p->allocations = a;
    p->memory += bytes;
    return a->memory;
}

void vxn_free(VxnProgram *p) {
    if (!p) return;
    Allocation *a = p->allocations;
    while (a) { Allocation *next = a->next; free(a->memory); free(a); a = next; }
    free(p);
}
int vxn_ok(const VxnProgram *p) { return p && !p->error[0]; }
bool vx_word(const VxnProgram *p, Token t, const char *word) {
    return !t.qualifier && t.kind == TK_WORD && t.length == strlen(word) &&
        memcmp(t.synthetic?t.synthetic:p->source + t.start, word, t.length) == 0;
}
bool vx_same(const VxnProgram *p, Token a, Token b) {
    return a.length == b.length && memcmp(a.synthetic?a.synthetic:p->source + a.start, b.synthetic?b.synthetic:p->source + b.start, a.length) == 0;
}
Token vx_synthetic(Token at,const char *name) {
    at.kind=TK_WORD; at.synthetic=name; at.length=strlen(name); at.qualifier=0; return at;
}
void vx_name(const VxnProgram *p, Token t, char *out, size_t cap) {
    if (!cap) return;
    size_t n = t.length < cap - 1 ? t.length : cap - 1;
    memcpy(out, t.synthetic?t.synthetic:p->source + t.start, n); out[n] = 0;
}

void vxn_diagnostic(const VxnProgram *p, const char *path, FILE *out) {
    if (!p) { fprintf(out, "error: out of memory\n"); return; }
    if (!p->error[0]) return;
    if (!p->source) { fprintf(out, "%s: error: %s\n", path, p->error); return; }
    size_t base=0,n=strlen(p->source),pos=p->error_pos;
    for(int i=0;i<p->module_count;++i) {
        const Module *m=&p->modules[i];
        if(pos>=m->start && pos<=m->start+m->length) {
            base=m->start; n=base+m->length; if(m->path) path=m->path; break;
        }
    }
    if(pos>n) pos=n;
    size_t line = 1, start = base, end = pos;
    for (size_t i = base; i < pos; ++i) if (p->source[i] == '\n') { ++line; start = i + 1; }
    while (end < n && p->source[end] != '\n' && p->source[end] != '\r') ++end;
    fprintf(out, "%s:%zu:%zu: error: %s\n  %.*s\n  ", path, line, pos - start + 1,
        p->error, (int)(end - start), p->source + start);
    for (size_t i = start; i < pos; ++i) fputc(p->source[i] == '\t' ? '\t' : ' ', out);
    size_t width = p->error_length;
    if (width > end - pos) width = end - pos;
    if (!width) width = 1;
    for (size_t i = 0; i < width; ++i) fputc('^', out);
    fputc('\n', out);
}


static bool letter(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool digit(unsigned char c) { return c >= '0' && c <= '9'; }
static unsigned hex_digit(unsigned c) {
    return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:99;
}
static int byte_string(VxnProgram *p,Token t,unsigned char *out) {
    const unsigned char *s=(const unsigned char *)p->source;
    size_t end=t.start+t.length-1; int count=0;
    for(size_t i=t.start+2;i<end;++i) {
        unsigned c=s[i];
        if(c=='\\') {
            if(++i==end) vx_fail(p,t,"unterminated byte escape");
            c=s[i];
            if(c=='n') c='\n'; else if(c=='r') c='\r'; else if(c=='t') c='\t';
            else if(c=='0') c=0;
            else if(c=='x') {
                if(i+2>=end || hex_digit(s[i+1])>15 || hex_digit(s[i+2])>15)
                    vx_fail(p,t,"byte hex escape requires two hex digits");
                c=hex_digit(s[i+1])*16+hex_digit(s[i+2]); i+=2;
            } else if(c!='\\'&&c!='"') vx_fail(p,t,"unsupported byte escape");
        } else if(c<32) vx_fail(p,t,"control bytes in literals require an escape");
        if(count==1024) vx_fail(p,t,"byte literal exceeds 1024 bytes");
        if(out) out[count]=(unsigned char)c;
        ++count;
    }
    return count;
}
static int text_string(VxnProgram *p,Token t,uint64_t *out) {
    const unsigned char *s=(const unsigned char *)p->source;
    size_t end=t.start+t.length-1; int count=0;
    for(size_t i=t.start+1;i<end;++i) {
        unsigned c=s[i],encoded[4],n=1; bool scalar=false;
        if(c=='\\') {
            if(++i==end) vx_fail(p,t,"unterminated text escape");
            c=s[i];
            if(c=='n') c='\n'; else if(c=='r') c='\r'; else if(c=='t') c='\t';
            else if(c=='0') c=0;
            else if(c=='x') {
                if(i+2>=end||hex_digit(s[i+1])>15||hex_digit(s[i+2])>15)
                    vx_fail(p,t,"text hex escape requires two hex digits");
                c=hex_digit(s[i+1])*16+hex_digit(s[i+2]); i+=2;
            } else if(c=='u') {
                if(++i==end||s[i]!='{') vx_fail(p,t,"Unicode escape requires \\u{hex digits}");
                unsigned digits=0; c=0;
                while(++i<end&&s[i]!='}') {
                    unsigned digit=hex_digit(s[i]);
                    if(digit>15||++digits>6) vx_fail(p,t,"Unicode escape requires one to six hex digits");
                    c=c*16+digit;
                }
                if(i==end||!digits) vx_fail(p,t,"Unicode escape requires one to six hex digits and '}'");
                if(c>0x10ffff||(c>=0xd800&&c<=0xdfff)) vx_fail(p,t,"Unicode escape must name a Unicode scalar value");
                scalar=true;
            } else if(c!='\\'&&c!='"') vx_fail(p,t,"unsupported text escape");
        } else if(c<32) vx_fail(p,t,"control bytes in text literals require an escape");
        if(scalar&&c>0x7f) {
            if(c<=0x7ff) { n=2; encoded[0]=0xc0|(c>>6); }
            else if(c<=0xffff) { n=3; encoded[0]=0xe0|(c>>12); }
            else { n=4; encoded[0]=0xf0|(c>>18); }
            for(unsigned j=1;j<n;++j) encoded[j]=0x80|((c>>(6*(n-j-1)))&63);
        } else encoded[0]=c;
        if(count+(int)n>1024) vx_fail(p,t,"text literal exceeds 1024 UTF-8 bytes");
        for(unsigned j=0;j<n;++j) { if(out) out[count]=encoded[j]; ++count; }
    }
    return count;
}
static void lex(VxnProgram *p,size_t offset,size_t length) {
    const unsigned char *s = (const unsigned char *)p->source;
    size_t n = offset+length, i = offset;
    p->at=p->token_count=0;
    p->tokens = vx_alloc(p, (length + 1) * sizeof *p->tokens);
    while (i < n) {
        if (strchr(" \t\r\n\f\v",s[i])) { ++i; continue; }
        if (s[i] == '/' && i + 1 < n && s[i+1] == '/') {
            while (i < n && s[i] != '\n') ++i;
            continue;
        }
        if (s[i] == '/' && i + 1 < n && s[i+1] == '*') {
            size_t start = i; i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i+1] == '/')) ++i;
            if (i + 1 >= n) vx_fail(p,(Token){.start=start,.length=2},"unterminated block comment");
            i += 2; continue;
        }
        Token t = {.start=i,.module=p->current_module};
        if(s[i]=='"'||(s[i]=='b' && i+1<n && s[i+1]=='"')) {
            bool text=s[i]=='"'; t.kind=text?TK_TEXT:TK_BYTES; i+=text?1:2;
            while(i<n && s[i]!='"') {
                if(s[i]=='\n'||s[i]=='\r') vx_fail(p,t,text?"unterminated text literal":"unterminated byte literal");
                if(s[i]=='\\') ++i;
                if(i<n) ++i;
            }
            if(i==n) vx_fail(p,t,text?"unterminated text literal":"unterminated byte literal");
            ++i; t.length=i-t.start; t.number=(uint64_t)(text?text_string(p,t,NULL):byte_string(p,t,NULL));
        } else if (letter(s[i]) || s[i] == '_') {
            t.kind = TK_WORD;
            do { ++i; } while (i < n && (letter(s[i]) || digit(s[i]) || s[i] == '_'));
        } else if (digit(s[i])) {
            bool hex = i + 1 < n && s[i] == '0' && s[i+1] == 'x';
            size_t start = i;
            do { ++i; } while (i < n && (letter(s[i]) || digit(s[i]) || s[i] == '_' ||
                (!hex && s[i] == '.' && i + 1 < n && digit(s[i+1])) ||
                (!hex && (s[i] == '+' || s[i] == '-') && (s[i-1] == 'e' || s[i-1] == 'E'))));
            t.length = i - start; t.kind = TK_NUMBER;
            bool real = false;
            if (!hex) for (size_t j=start;j<i;++j) if (s[j]=='.'||s[j]=='e'||s[j]=='E') real = true;
            if (real) {
                char buffer[128]; size_t k=0;
                for (size_t j=start;j<i;++j) {
                    if (s[j]=='_') {
                        if (j==start || j+1==i || !digit(s[j-1]) || !digit(s[j+1])) vx_fail(p,t,"malformed floating literal");
                    } else {
                        if (k+1==sizeof buffer) vx_fail(p,t,"floating literal is too long");
                        buffer[k++]=(char)s[j];
                    }
                }
                uint64_t cells[128],parsed[3];
                for(size_t j=0;j<k;++j) cells[j]=(unsigned char)buffer[j];
                vxn_ext_parse(cells,k,128,parsed);
                if(parsed[0]&&parsed[1]!=2) vx_fail(p,t,"floating literal must be well formed");
                t.kind=TK_REAL;
            } else {
                unsigned base=hex?16:10; if (hex) start+=2;
                bool prior=false; uint64_t value=0;
                for (size_t j=start;j<i;++j) {
                    unsigned c=s[j];
                    if(c=='_') {
                        if(!prior || j+1==i) vx_fail(p,t,"malformed integer literal");
                        prior=false; continue;
                    }
                    unsigned d=digit((unsigned char)c)?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:99;
                    if(d>=base || value>(UINT64_MAX-d)/base) vx_fail(p,t,"integer literal must fit uint64");
                    value=value*base+d; prior=true;
                }
                if(!prior) vx_fail(p,t,"malformed integer literal");
                t.number=value;
            }
        } else {
            static const struct { const char *text; int kind; } ops[] = {
                {"..=",TK_RANGE},{"<<=",TK_SHL_EQ},{">>=",TK_SHR_EQ},{"==",TK_EQ},{"!=",TK_NE},{"<=",TK_LE},{">=",TK_GE},
                {"&&",TK_AND},{"||",TK_OR},{"<<",TK_SHL},{">>",TK_SHR},{"++",TK_INC},{"--",TK_DEC},
                {"+=",TK_ADD_EQ},{"-=",TK_SUB_EQ},{"*=",TK_MUL_EQ},{"/=",TK_DIV_EQ},{"%=",TK_MOD_EQ},
                {"&=",TK_AND_EQ},{"|=",TK_OR_EQ},{"^=",TK_XOR_EQ},{"->",TK_ARROW},{"::",TK_SCOPE},{"|>",TK_PIPE},{".*",TK_DEREF}};
            for(size_t j=0;j<sizeof ops/sizeof ops[0];++j) {
                size_t length=strlen(ops[j].text);
                if(i+length<=n && !memcmp(s+i,ops[j].text,length)) { t.kind=ops[j].kind; i+=length; break; }
            }
            if(!t.kind) {
                if(strchr("(){}[];,=+!-*/%~&|^<>.:?",s[i])) t.kind=s[i++];
                else vx_fail(p,(Token){.start=i,.length=1},"unsupported character (byte 0x%02x)",(unsigned)s[i]);
            }
        }
        t.length=i-t.start; p->tokens[p->token_count++]=t;
    }
    p->tokens[p->token_count++]=(Token){.kind=TK_EOF,.start=n,.module=p->current_module};
}
static Token peek(VxnProgram *p) { return p->tokens[p->at]; }
static Token next(VxnProgram *p) { Token t=peek(p); if(t.kind!=TK_EOF) ++p->at; return t; }
static bool take(VxnProgram *p,int k) { if(peek(p).kind!=k) return false; next(p); return true; }
static bool word(VxnProgram *p,const char *s) { return vx_word(p,peek(p),s); }
static bool take_word(VxnProgram *p,const char *s) { if(!word(p,s)) return false; next(p); return true; }
static void expect(VxnProgram *p,int k) {
    if(!take(p,k)) {
        if(k<256) vx_fail(p,peek(p),"expected '%c'",k);
        vx_fail(p,peek(p),"expected %s",k==TK_ARROW?"->":k==TK_SCOPE?"::":"operator");
    }
}
static void enter(VxnProgram *p) { if(++p->depth>VX_MAX_DEPTH) vx_fail(p,peek(p),"syntax nesting exceeds limit (%d)",VX_MAX_DEPTH); }
static void *node(VxnProgram *p,size_t n) { if(++p->nodes>VX_MAX_NODES) vx_fail(p,peek(p),"syntax node limit exceeded"); return vx_alloc(p,n); }
static Token name(VxnProgram *p,bool upper) {
    static const char *reserved[]={"pub","safe","unsafe","opaque","once","const","mut","let","wrap","checked","saturating","bool","void",
        "uint8","uint16","uint32","uint64","sint8","sint16","sint32","sint64","float32","float64","float80","float128",
        "true","false","return","if","else","pure","io","hot","noalloc","nopanic","noblock","lazy","eager","rec",
        "ref","uniq","raw","from","noescape","for","while","break","continue","struct","choice","match",
        "comptime","loadtime","dyntime","emit","cast","narrow","bitcast","ptrcast","fn","import","as","Slice","UniqSlice","FnRef","FnUniq","DynFn","Box","Vec","String","str","constraint","impl","operator"};
    Token t=next(p);
    if(t.kind!=TK_WORD) vx_fail(p,t,"expected a name");
    for(size_t i=0;i<sizeof reserved/sizeof reserved[0];++i)
        if(vx_word(p,t,reserved[i])) vx_fail(p,t,"reserved keyword cannot be used as a name");
    const char *s=p->source+t.start;
    if(t.length>80) vx_fail(p,t,"identifier exceeds limit (80 characters)");
    bool valid=upper?(s[0]>='A'&&s[0]<='Z'):(s[0]>='a'&&s[0]<='z'&&s[t.length-1]!='_');
    for(size_t i=0;i<t.length;++i) {
        if(upper ? !(letter((unsigned char)s[i])||digit((unsigned char)s[i])) :
           !((s[i]>='a'&&s[i]<='z')||digit((unsigned char)s[i])||s[i]=='_')) valid=false;
        if(i && s[i]=='_'&&s[i-1]=='_') valid=false;
    }
    if(!valid) vx_fail(p,t,upper?"type and constructor names must be UpperCamel":"function and value names must be lower_snake_case");
    return t;
}
/* Module qualifiers and choice qualifiers are distinct: vec::Value::Some. */
static Token symbol(VxnProgram *p,int upper) {
    Token first=peek(p); int target=0;
    if(first.kind==TK_WORD && p->source[first.start]>='a' && p->source[first.start]<='z' && p->tokens[p->at+1].kind==TK_SCOPE) {
        first=name(p,false); expect(p,TK_SCOPE);
        for(Import *i=p->modules[p->current_module].imports;i;i=i->next)
            if(!i->selective && vx_same(p,first,i->name)) { target=i->module; break; }
        if(!target) vx_fail(p,first,"unknown module alias; add an explicit import");
    }
    Token t=peek(p);
    bool capital=t.kind==TK_WORD && p->source[t.start]>='A' && p->source[t.start]<='Z';
    t=name(p,upper<0?capital:upper!=0); t.qualifier=target; return t;
}
static Type named_type(VxnProgram *p,Token name) {
    if(!name.qualifier) for(int i=0;i<p->parse_type_count;++i)
        if(vx_same(p,name,p->types[p->parse_types[i]]->name)) return p->parse_types[i];
    if(vx_word(p,name,"IoFile")) return TY_FILE;
    if(vx_word(p,name,"Canvas")) return TY_CANVAS;
    if(vx_word(p,name,"Window")) return TY_WINDOW;
    int owner=vx_symbol_module(p,name);
    for(int i=TY_AGGREGATE;i<p->type_count;++i) {
        TypeDef *d=p->types[i];
        if(d->kind!=TD_FN_VIEW && d->kind!=TD_UNIQ_FN_VIEW && d->kind!=TD_ARRAY && d->kind!=TD_SLICE && d->kind!=TD_UNIQ_SLICE && d->kind!=TD_CALLABLE && d->kind!=TD_PARAMETER && !d->template_type && vx_same(p,name,d->name) &&
           (d->name.module==owner || (!name.qualifier && !d->name.module))) {
            vx_visible(p,name,d->name,d->public_api); return i;
        }
    }
    if(owner!=name.module) vx_fail(p,name,"unknown imported type");
    if(p->type_count==VX_MAX_TYPES) vx_fail(p,name,"type limit exceeded");
    TypeDef *d=vx_alloc(p,sizeof *d); d->name=name; p->types[p->type_count]=d;
    return p->type_count++;
}
static Expr *expression(VxnProgram *p);
static Type type(VxnProgram *p);
static Stmt *block(VxnProgram *p);
static unsigned effect_caps(VxnProgram *p) {
    unsigned caps=0;
    for(;;) {
        unsigned cap=word(p,"pure")?CP_PURE:word(p,"io")?CP_IO:word(p,"hot")?CP_HOT:word(p,"noalloc")?CP_NOALLOC:word(p,"nopanic")?CP_NOPANIC:word(p,"noblock")?CP_NOBLOCK:0;
        if(!cap) break;
        if(caps&cap) vx_fail(p,peek(p),"duplicate effect annotation");
        caps|=cap; next(p);
    }
    if((caps&(CP_PURE|CP_IO))==(CP_PURE|CP_IO)) vx_fail(p,peek(p),"pure and io are conflicting effect annotations");
    return caps;
}
static Function *callable_signature(VxnProgram *p,bool literal) {
    Function *f=node(p,sizeof *f); f->name=f->cap_token=peek(p); f->operator_code=-1;
    bool safe=take_word(p,"safe");
    if(safe) f->caps=effect_caps(p);
    if(!literal&&(!safe||!f->caps)) vx_fail(p,peek(p),"callable types require safe and an explicit effect contract");
    if(!take_word(p,"fn")) vx_fail(p,peek(p),"expected fn after callable contract");
    if(take_word(p,"mut")) f->call_mode=1;
    else if(take_word(p,"once")) f->call_mode=2;
    if(literal&&f->call_mode) vx_fail(p,f->name,"closure literal invocation mode is inferred from its body");
    expect(p,'(');
    if(peek(p).kind!=')') do {
        if(f->param_count==VX_MAX_PARAMS) vx_fail(p,peek(p),"callable parameter limit exceeded");
        Param *a=&f->params[f->param_count++];
        if(take_word(p,"mut")) a->mutable_binding=true;
        else if(!take_word(p,"const")) vx_fail(p,peek(p),"parameters require const or mut");
        enter(p); a->type=type(p); --p->depth;
        if(literal || (peek(p).kind==TK_WORD&&!word(p,"noescape"))) a->name=name(p,false);
        a->noescape=take_word(p,"noescape");
        if(peek(p).kind=='=') vx_fail(p,peek(p),"callable parameters do not have defaults; use a named wrapper");
    } while(take(p,','));
    expect(p,')'); expect(p,TK_ARROW); enter(p); f->result=type(p); --p->depth;
    if(take_word(p,"from")) {
        f->from_explicit=true; expect(p,'(');
        if(peek(p).kind!=')') do {
            if(f->from_count==VX_MAX_PARAMS) vx_fail(p,peek(p),"too many callable from origins");
            f->from_names[f->from_count++]=name(p,false);
        } while(take(p,','));
        expect(p,')');
    }
    return f;
}
static bool starts_type(const VxnProgram *p,size_t at) {
    Token t=p->tokens[at]; if(t.kind!=TK_WORD) return false;
    if(p->source[t.start]>='A'&&p->source[t.start]<='Z') return true;
    if(at+2<p->token_count && p->tokens[at+1].kind==TK_SCOPE)
        return p->source[p->tokens[at+2].start]>='A'&&p->source[p->tokens[at+2].start]<='Z';
    static const char *words[]={"uint8","uint16","uint32","uint64","sint8","sint16","sint32","sint64","bool","void","float32","float64","float80","float128","str","wrap","checked","saturating","ref","safe"};
    for(size_t i=0;i<sizeof words/sizeof words[0];++i) if(vx_word(p,t,words[i])) return true;
    return false;
}
static Type applied_type(VxnProgram *p,Type base,Token at) {
    if(peek(p).kind!='[' || !starts_type(p,p->at+1)) return base;
    next(p); Type args[VX_MAX_GENERICS]; int count=0;
    do {
        if(count==VX_MAX_GENERICS) vx_fail(p,peek(p),"type argument limit exceeded (8)");
        enter(p); args[count++]=type(p); --p->depth;
    } while(take(p,','));
    expect(p,']'); return vx_apply_type(p,base,args,count,at);
}
static Type type(VxnProgram *p) {
    Token at=peek(p); Type t=TY_INFER;
    if(take_word(p,"eager")) { enter(p); t=type(p); --p->depth; return t; }
    if(word(p,"lazy")) vx_fail(p,peek(p),"lazy T currently requires an initialized local; use Lazy[T, F] for an explicit suspension type");
    if(take_word(p,"opaque")) {
        bool lazy=take_word(p,"Lazy"); Type value=TY_INFER;
        if(lazy) { expect(p,'['); value=type(p); expect(p,','); }
        Function *contract=callable_signature(p,false);
        if(lazy) {
            expect(p,']');
            if(contract->param_count||contract->result!=value) vx_fail(p,at,"opaque Lazy requires a zero-argument callable returning its value type");
        }
        if(p->type_count==VX_MAX_TYPES) vx_fail(p,at,"opaque callable type limit exceeded");
        TypeDef *d=node(p,sizeof *d); d->kind=TD_OPAQUE; d->name=at; d->signature=contract;
        Type hidden=p->type_count++; p->types[hidden]=d;
        if(lazy) { Type args[]={value,hidden}; return vx_apply_type(p,p->lazy_type,args,2,at); }
        return hidden;
    }
    if(word(p,"safe")||word(p,"fn")) {
        Function *f=callable_signature(p,false); return vx_callable_type(p,f,at);
    }
    if(take_word(p,"uniq")) {
        bool writable=!take_word(p,"const"); if(writable) take_word(p,"mut");
        enter(p); t=type(p); --p->depth;
        return vx_exclusive_type(p,t,writable,at);
    }
    if(take_word(p,"ref")) {
        enter(p); t=type(p); --p->depth;
        return vx_reference_type(p,t,at);
    }
    if(take_word(p,"Box")) {
        if(peek(p).kind!='[') return vx_builtin_constructor(p,TD_BOX,at);
        expect(p,'['); enter(p); Type element=type(p); --p->depth; expect(p,']'); t=vx_box_type(p,element,at);
    }
    else if(take_word(p,"String")) t=vx_string_type(p,at);
    else if(take_word(p,"str")) t=vx_str_type(p,at);
    else if(take_word(p,"Vec")) {
        if(peek(p).kind!='[') return vx_builtin_constructor(p,TD_VEC,at);
        expect(p,'['); enter(p); Type element=type(p); --p->depth; expect(p,']'); t=vx_vec_type(p,element,at);
    }
    else if(take_word(p,"Slice")) {
        expect(p,'['); enter(p); Type element=type(p); --p->depth; expect(p,']'); t=vx_slice_type(p,element,at);
    }
    else if(take_word(p,"UniqSlice")) {
        expect(p,'['); enter(p); Type element=type(p); --p->depth; expect(p,']'); t=vx_uniq_slice_type(p,element,at);
    }
    else if(take_word(p,"DynFn")) {
        expect(p,'['); enter(p); Type contract=type(p); --p->depth; expect(p,']');
        t=vx_dyn_fn_type(p,contract,at);
    }
    else if(word(p,"FnRef")||word(p,"FnUniq")) {
        bool exclusive=take_word(p,"FnUniq"); if(!exclusive) next(p);
        expect(p,'['); enter(p); Type contract=type(p); --p->depth; expect(p,']');
        t=vx_fn_view_type(p,contract,exclusive,at);
    }
    else if(take_word(p,"bool")) t=TY_BOOL;
    else if(take_word(p,"void")) t=TY_VOID;
    else if(take_word(p,"float32")) t=TY_F32;
    else if(take_word(p,"float64")) t=TY_F64;
    else if(take_word(p,"float80")) t=TY_F80;
    else if(take_word(p,"float128")) t=TY_F128;
    else {
        int mode=take_word(p,"wrap")?0:take_word(p,"checked")?1:take_word(p,"saturating")?2:-1;
        static const char *names[]={"uint8","uint16","uint32","uint64","sint8","sint16","sint32","sint64"};
        for(int i=0;i<8;++i) if(word(p,names[i])) {
            if(mode<0) mode=1;
            next(p); t=TY_INT_BASE+mode*8+i; break;
        }
        if(!t) {
            if(mode>=0) vx_fail(p,peek(p),"an arithmetic domain requires an integer type");
            Token nt=peek(p);
            if(nt.kind==TK_WORD && ((p->source[nt.start]>='A' && p->source[nt.start]<='Z') || p->tokens[p->at+1].kind==TK_SCOPE)) { Token n=symbol(p,true); t=applied_type(p,named_type(p,n),n); }
            else vx_fail(p,nt,"expected a supported type; raw pointers are not implemented yet");
        }
    }
    Expr *dimensions[VX_MAX_DEPTH]; int count=0;
    while(take(p,'[')) {
        if(count==VX_MAX_DEPTH) vx_fail(p,peek(p),"array dimension limit exceeded");
        dimensions[count++]=expression(p); expect(p,']');
    }
    while(count) {
        Expr *length=dimensions[--count];
        if(length->kind==EX_WORD) {
            if(length->value>1024) vx_fail(p,length->token,"array length must be in 0..1024");
            t=vx_array_type(p,t,(int)length->value,at);
        } else {
            if(p->type_count==VX_MAX_TYPES) vx_fail(p,at,"type limit exceeded");
            TypeDef *d=vx_alloc(p,sizeof *d); d->kind=TD_ARRAY; d->element=t;
            d->count=-1; d->length=length; d->name=at; p->types[p->type_count]=d; t=p->type_count++;
        }
    }
    return t;
}
static Expr *expression(VxnProgram *p);
static Stmt *block(VxnProgram *p);
static void text_literal(VxnProgram *p,Expr *e,Token t) {
    e->kind=EX_TEXT; e->argc=(int)t.number; e->type=vx_str_type(p,t);
    e->cells=vx_alloc(p,((size_t)e->argc+1)*sizeof *e->cells); text_string(p,t,e->cells);
    uint64_t checked[3]; vx_utf8_exec(0,e->cells,(uint64_t)e->argc,0,checked);
    if(checked[0]) vx_fail(p,t,"text literal is not valid UTF-8 at decoded byte %u",(unsigned)checked[2]);
    TextLiteral *literal=vx_alloc(p,sizeof *literal);
    literal->cells=e->cells; literal->count=e->argc; literal->id=e->id=p->text_count++;
    literal->next=p->text_literals; p->text_literals=literal;
}
static Expr *pattern_literal(VxnProgram *p) {
    Expr *root=node(p,sizeof *root),*e=root; e->token=peek(p);
    if(take(p,'-')) {
        e->kind=EX_UNARY; e->op=VX_NEG; e->left=node(p,sizeof *e);
        e=e->left; e->token=peek(p);
        if(peek(p).kind!=TK_NUMBER) vx_fail(p,peek(p),"negative pattern requires an integer literal");
    }
    if(peek(p).kind==TK_NUMBER) { e->kind=EX_WORD; e->value=next(p).number; }
    else if(word(p,"true")||word(p,"false")) { e->kind=EX_BOOL; e->value=word(p,"true"); next(p); }
    else vx_fail(p,peek(p),"range endpoint requires an integer literal");
    return root;
}
static Pattern *pattern(VxnProgram *p,Arm *arm);
static Pattern *pattern_single(VxnProgram *p,Arm *arm) {
    enter(p); Pattern *n=node(p,sizeof *n); n->token=peek(p);
    if(take_word(p,"_")) n->kind=PAT_WILD;
    else if(word(p,"const")||word(p,"mut")) {
        n->kind=PAT_BIND;
        if(arm->count==VX_MAX_PARAMS) vx_fail(p,peek(p),"pattern binding limit exceeded");
        n->binding=arm->count++; Param *b=&arm->bindings[n->binding];
        b->mutable_binding=take_word(p,"mut"); if(!b->mutable_binding) next(p);
        b->name=name(p,false);
        for(int i=0;i<n->binding;++i) if(vx_same(p,b->name,arm->bindings[i].name)) vx_fail(p,b->name,"duplicate binding in this pattern");
    } else if(peek(p).kind==TK_TEXT) {
        n->kind=PAT_TEXT; n->literal=node(p,sizeof *n->literal);
        n->literal->token=peek(p); text_literal(p,n->literal,next(p));
    } else if(take(p,'{')) {
        n->kind=PAT_ARRAY; Pattern **tail=&n->children;
        if(peek(p).kind!='}') do {
            if(n->count==VX_MAX_FIELDS) vx_fail(p,peek(p),"array pattern element limit exceeded");
            *tail=pattern(p,arm); tail=&(*tail)->next; ++n->count;
            if(peek(p).kind=='}') break;
        } while(take(p,',' )&&peek(p).kind!='}');
        expect(p,'}');
    } else if(peek(p).kind==TK_NUMBER||peek(p).kind=='-'||word(p,"true")||word(p,"false")) {
        n->kind=PAT_LITERAL; n->literal=pattern_literal(p);
        if(take(p,TK_RANGE)) { n->kind=PAT_RANGE; n->limit=pattern_literal(p); }
    } else {
        if(peek(p).kind==TK_WORD&&p->source[peek(p).start]>='a'&&p->source[peek(p).start]<='z'&&p->tokens[p->at+1].kind!=TK_SCOPE)
            vx_fail(p,peek(p),"pattern bindings require const or mut; use _ to ignore a value");
        n->kind=PAT_CONSTRUCTOR; Token type_name=symbol(p,true);
        n->type=applied_type(p,named_type(p,type_name),type_name);
        if(take(p,'{')) {
            n->kind=PAT_STRUCT; Pattern **tail=&n->children;
            if(peek(p).kind!='}') do {
                if(n->count==VX_MAX_FIELDS) vx_fail(p,peek(p),"struct pattern field limit exceeded");
                Token field=name(p,false); expect(p,'=');
                *tail=pattern(p,arm); (*tail)->field=field; tail=&(*tail)->next; ++n->count;
                if(peek(p).kind=='}') break;
            } while(take(p,',')&&peek(p).kind!='}');
            expect(p,'}');
        } else {
          expect(p,TK_SCOPE); n->constructor=name(p,true);
          if(take(p,'(')) {
            Pattern **tail=&n->children;
            if(peek(p).kind!=')') do {
                if(n->count==VX_MAX_FIELDS) vx_fail(p,peek(p),"pattern payload limit exceeded");
                *tail=pattern(p,arm); tail=&(*tail)->next; ++n->count;
            } while(take(p,','));
            expect(p,')');
          }
        }
    }
    --p->depth; return n;
}
static void remap_pattern(Pattern *n,const int *ids) {
    if(n->kind==PAT_BIND) n->binding=ids[n->binding];
    for(Pattern *child=n->children;child;child=child->next) remap_pattern(child,ids);
}
static Pattern *pattern(VxnProgram *p,Arm *arm) {
    int base=arm->count; Pattern *first=pattern_single(p,arm);
    if(peek(p).kind!='|') return first;
    Pattern *n=node(p,sizeof *n); n->kind=PAT_OR; n->token=first->token; n->children=first; n->count=1;
    int count=arm->count; Param *bindings=vx_alloc(p,(size_t)(count?count:1)*sizeof *bindings);
    memcpy(bindings,arm->bindings,(size_t)count*sizeof *bindings); Pattern **tail=&first->next;
    while(take(p,'|')) {
        if(n->count==VX_MAX_FIELDS) vx_fail(p,peek(p),"alternative pattern limit exceeded");
        arm->count=base; Pattern *branch=pattern_single(p,arm); int ids[VX_MAX_PARAMS];
        if(arm->count!=count) vx_fail(p,branch->token,"pattern alternatives must bind the same names");
        for(int i=0;i<base;++i) ids[i]=i;
        for(int i=base;i<count;++i) {
            int j=base; while(j<count&&!vx_same(p,arm->bindings[i].name,bindings[j].name)) ++j;
            if(j==count) vx_fail(p,arm->bindings[i].name,"pattern alternatives must bind the same names");
            if(arm->bindings[i].mutable_binding!=bindings[j].mutable_binding)
                vx_fail(p,arm->bindings[i].name,"pattern alternative binding mutability must agree");
            ids[i]=j;
        }
        remap_pattern(branch,ids); memcpy(arm->bindings,bindings,(size_t)count*sizeof *bindings);
        *tail=branch; tail=&branch->next; ++n->count;
    }
    return n;
}
static Arm *arms(VxnProgram *p) {
    expect(p,'{'); Arm *head=NULL,**tail=&head; int count=0;
    while(!take(p,'}')) {
        if(++count>VX_MAX_FIELDS) vx_fail(p,peek(p),"match arm limit exceeded");
        Arm *a=node(p,sizeof *a); a->token=peek(p); a->pattern=pattern(p,a);
        if(take_word(p,"if")) a->guard=expression(p);
        expect(p,TK_ARROW);
        if(peek(p).kind=='{') a->body=block(p);
        else a->expr=expression(p);
        a->end=p->tokens[p->at-1].start+p->tokens[p->at-1].length;
        *tail=a; tail=&a->next;
        if(take(p,'}')) break;
        expect(p,',');
    }
    return head;
}
static void arguments(VxnProgram *p,Expr *e,int close,bool fields) {
    Arg **tail=&e->args;
    if(take(p,close)) return;
    do {
        if(e->argc==(fields?VX_MAX_FIELDS:1024)) vx_fail(p,peek(p),"initializer/argument limit exceeded");
        Arg *a=node(p,sizeof *a);
        if(fields) { a->name=name(p,false); expect(p,'='); }
        else if((e->kind==EX_CALL||e->kind==EX_METHOD||e->kind==EX_INVOKE) && peek(p).kind==TK_WORD && p->tokens[p->at+1].kind==':') {
            a->name=name(p,false); expect(p,':');
        }
        if(!fields&&(e->kind==EX_CALL||e->kind==EX_INVOKE||e->kind==EX_METHOD)&&word(p,"_")) {
            a->expr=node(p,sizeof *a->expr); a->expr->kind=EX_HOLE; a->expr->token=next(p);
        } else a->expr=expression(p);
        *tail=a; tail=&a->next; ++e->argc;
        if(take(p,close)) return;
        expect(p,',');
    } while(peek(p).kind!=close);
    expect(p,close);
}
static size_t bracket_end(const VxnProgram *p,size_t at) {
    int depth=0;
    for(size_t i=at;i<p->token_count;++i) {
        if(p->tokens[i].kind=='[') ++depth;
        else if(p->tokens[i].kind==']' && !--depth) return i+1;
        else if(p->tokens[i].kind==TK_EOF) break;
    }
    return 0;
}
static void type_arguments(VxnProgram *p,Expr *e) {
    expect(p,'[');
    do {
        if(e->type_arg_count==VX_MAX_GENERICS) vx_fail(p,peek(p),"type argument limit exceeded (8)");
        e->type_args[e->type_arg_count++]=type(p);
    } while(take(p,','));
    expect(p,']');
}
static Expr *atom(VxnProgram *p) {
    Token t=peek(p); Expr *e=NULL;
    if(take(p,'(')) { e=expression(p); expect(p,')'); }
    else {
        e=node(p,sizeof *e); e->token=t;
        if(word(p,"ref")||word(p,"uniq")) {
            bool exclusive=take_word(p,"uniq");
            if(exclusive) { e->op=take_word(p,"const")?2:1; if(e->op==1) take_word(p,"mut"); }
            else next(p);
            enter(p); e->kind=EX_REF; e->left=atom(p); --p->depth; return e;
        }
        if(t.kind=='!' || t.kind=='-' || t.kind=='+' || t.kind=='~') {
            next(p); enter(p); e->kind=EX_UNARY;
            e->op=t.kind=='!'?VX_NOT:t.kind=='-'?VX_NEG:t.kind=='~'?VX_INV:VX_POS;
            e->left=atom(p); --p->depth; return e;
        }
        if(word(p,"fn")||word(p,"safe")) {
            Function *f=callable_signature(p,true); f->anonymous=true;
            f->generic_count=p->parse_type_count;
            memcpy(f->generic_types,p->parse_types,(size_t)f->generic_count*sizeof(Type));
            if(p->function_count==VX_MAX_FUNCTIONS) vx_fail(p,t,"function limit exceeded");
            e->kind=EX_FUNCTION; e->id=p->function_count; p->functions[p->function_count++]=f;
            f->body=block(p); f->end=p->tokens[p->at-1].start+p->tokens[p->at-1].length;
        }
        else if(take(p,TK_NUMBER)) { e->kind=EX_WORD; e->value=t.number; }
        else if(take(p,TK_BYTES)) {
            e->kind=EX_BYTES; e->argc=(int)t.number;
            e->bytes=vx_alloc(p,(size_t)e->argc+1); byte_string(p,t,e->bytes);
            e->type=vx_array_type(p,TY_INT_BASE,e->argc,t);
        }
        else if(take(p,TK_TEXT)) {
            text_literal(p,e,t);
        }
        else if(take(p,TK_REAL)) { e->kind=EX_REAL; }
        else if(word(p,"true")||word(p,"false")) { e->kind=EX_BOOL; e->value=word(p,"true"); next(p); }
        else if(take_word(p,"match")) {
            enter(p); e->kind=EX_MATCH; e->left=expression(p); e->arms=arms(p); --p->depth;
        } else if(word(p,"cast")||word(p,"narrow")) {
            e->kind=EX_CAST; e->op=word(p,"narrow"); next(p);
            expect(p,'['); e->type=type(p); expect(p,']'); expect(p,'(');
            e->left=expression(p); expect(p,')');
        } else if(take(p,'{')) {
            e->kind=EX_ARRAY; arguments(p,e,'}',false);
        } else {
            e->token=symbol(p,-1);
            if(p->source[e->token.start]>='A'&&p->source[e->token.start]<='Z') {
                size_t end=peek(p).kind=='['?bracket_end(p,p->at):0;
                if(end && end+1<p->token_count && p->tokens[end].kind==TK_SCOPE &&
                   p->tokens[end+1].kind==TK_WORD && p->source[p->tokens[end+1].start]>='a' && p->source[p->tokens[end+1].start]<='z') {
                    e->kind=EX_METHOD; type_arguments(p,e); expect(p,TK_SCOPE);
                    e->member=name(p,false); expect(p,'('); arguments(p,e,')',false);
                } else {
                e->type=applied_type(p,named_type(p,e->token),e->token);
                if(take(p,TK_SCOPE)) {
                    e->kind=EX_CHOICE; e->member=name(p,true);
                    if(take(p,'(')) arguments(p,e,')',false);
                } else {
                    e->kind=EX_RECORD; expect(p,'{'); arguments(p,e,'}',true);
                }
                }
            } else {
                e->kind=EX_VAR;
                if(peek(p).kind=='[') {
                    size_t end=bracket_end(p,p->at);
                    if(end && end<p->token_count && p->tokens[end].kind=='(') type_arguments(p,e);
                }
                if(take(p,'(')) { e->kind=EX_CALL; arguments(p,e,')',false); }
            }
        }
    }
    for(int count=0;;++count) {
        if(count>VX_MAX_DEPTH) vx_fail(p,peek(p),"postfix expression limit exceeded");
        if(take(p,'(')) {
            Expr *call=node(p,sizeof *call); call->kind=EX_INVOKE; call->token=e->token; call->left=e;
            arguments(p,call,')',false); e=call;
        } else if(take(p,TK_DEREF)) {
            Expr *deref=node(p,sizeof *deref); deref->kind=EX_DEREF; deref->token=e->token;
            deref->left=e; e=deref;
        } else if(take(p,'.')) {
            Expr *field=node(p,sizeof *field); field->kind=EX_FIELD; field->token=e->token;
            field->left=e;
            if(take(p,'*')) field->kind=EX_DEREF;
            else field->member=name(p,false);
            e=field;
        } else if(take(p,'?')) {
            Expr *propagate=node(p,sizeof *propagate); propagate->kind=EX_TRY;
            propagate->token=p->tokens[p->at-1]; propagate->left=e; e=propagate;
        } else if(take(p,'[')) {
            Expr *index=node(p,sizeof *index); index->kind=EX_INDEX; index->token=e->token;
            index->left=e; index->right=expression(p); expect(p,']'); e=index;
        } else break;
    }
    return e;
}
static int precedence(int token,int *op) {
    switch(token) {
    case TK_OR:*op=VX_LOR;return 1; case TK_AND:*op=VX_LAND;return 2;
    case '|':*op=VX_OR;return 3; case '^':*op=VX_XOR;return 4; case '&':*op=VX_AND;return 5;
    case TK_EQ:*op=VX_EQ;return 6; case TK_NE:*op=VX_NE;return 6;
    case '<':*op=VX_LT;return 7; case TK_LE:*op=VX_LE;return 7; case '>':*op=VX_GT;return 7; case TK_GE:*op=VX_GE;return 7;
    case TK_SHL:*op=VX_SHL;return 8; case TK_SHR:*op=VX_SHR;return 8;
    case '+':*op=VX_ADD;return 9; case '-':*op=VX_SUB;return 9;
    case '*':*op=VX_MUL;return 10; case '/':*op=VX_DIV;return 10; case '%':*op=VX_MOD;return 10;
    default:return 0;
    }
}
static Expr *binary(VxnProgram *p,int minimum) {
    enter(p); Expr *left=atom(p); int op=0, prec=precedence(peek(p).kind,&op), count=0;
    while(prec>=minimum) {
        if(++count>VX_MAX_DEPTH) vx_fail(p,peek(p),"expression chain exceeds prototype limit");
        Token t=next(p); Expr *e=node(p,sizeof *e); e->kind=EX_BINARY; e->op=op; e->token=t;
        e->left=left; e->right=binary(p,prec+1); left=e; prec=precedence(peek(p).kind,&op);
    }
    --p->depth; return left;
}
static Expr *expression(VxnProgram *p) {
    Expr *e=binary(p,1);
    int count=0;
    while(take(p,TK_PIPE)) {
        if(++count>VX_MAX_DEPTH) vx_fail(p,peek(p),"pipe chain limit exceeded");
        Expr *call=node(p,sizeof *call); call->kind=EX_CALL; call->token=symbol(p,false);
        if(peek(p).kind=='[') type_arguments(p,call);
        expect(p,'('); arguments(p,call,')',false);
        Arg *first=node(p,sizeof *first); first->expr=e; first->next=call->args; call->args=first; ++call->argc; e=call;
    }
    return e;
}
static int assignment_op(int token) {
    switch(token) {
    case '=':return -1; case TK_ADD_EQ:case TK_INC:return VX_ADD; case TK_SUB_EQ:case TK_DEC:return VX_SUB;
    case TK_MUL_EQ:return VX_MUL; case TK_DIV_EQ:return VX_DIV; case TK_MOD_EQ:return VX_MOD;
    case TK_AND_EQ:return VX_AND; case TK_OR_EQ:return VX_OR; case TK_XOR_EQ:return VX_XOR;
    case TK_SHL_EQ:return VX_SHL; case TK_SHR_EQ:return VX_SHR; default:return -2;
    }
}
static Stmt *simple(VxnProgram *p,bool semicolon) {
    Stmt *s=node(p,sizeof *s); s->token=peek(p);
    if(word(p,"const")||word(p,"mut")||word(p,"let")||word(p,"comptime")) {
        s->kind=ST_BIND; s->comptime=take_word(p,"comptime"); bool inferred=take_word(p,"let");
        if(s->comptime && word(p,"mut")) vx_fail(p,peek(p),"comptime implies const; a comptime binding cannot be mut");
        if(take_word(p,"mut")) s->mutable_binding=true;
        else if(!take_word(p,"const")&&!s->comptime) vx_fail(p,peek(p),"bindings require const or mut");
        s->lazy=take_word(p,"lazy");
        if(s->lazy&&(inferred||s->comptime)) vx_fail(p,s->token,"lazy bindings require an explicit runtime result type");
        s->type=inferred?TY_INFER:type(p); s->name=name(p,false);
        if(take(p,'=')) s->expr=expression(p);
        else if(inferred||!s->mutable_binding||s->lazy) vx_fail(p,s->name,"const, inferred and lazy bindings require an initializer");
        if(s->lazy) {
            if(p->function_count==VX_MAX_FUNCTIONS) vx_fail(p,s->name,"function limit exceeded");
            Function *f=node(p,sizeof *f); f->name=f->cap_token=s->name; f->anonymous=true; f->result=s->type;
            f->generic_count=p->parse_type_count; memcpy(f->generic_types,p->parse_types,(size_t)f->generic_count*sizeof(Type));
            Stmt *ret=node(p,sizeof *ret); ret->kind=ST_RETURN; ret->token=s->name; ret->expr=s->expr;
            f->body=ret; f->end=peek(p).start; ret->end=ret->scope_end=f->end;
            Expr *closure=node(p,sizeof *closure); closure->kind=EX_FUNCTION; closure->token=s->name; closure->id=p->function_count;
            p->functions[p->function_count++]=f; s->expr=closure;
        }
    } else {
        s->expr=expression(p); int k=peek(p).kind,op=assignment_op(k);
        if(op!=-2) {
            next(p); s->kind=ST_ASSIGN; s->target=s->expr; s->op=op;
            if(k==TK_INC||k==TK_DEC) {
                s->expr=node(p,sizeof *s->expr); s->expr->kind=EX_WORD; s->expr->token=s->token; s->expr->value=1;
            } else s->expr=expression(p);
        } else s->kind=ST_EXPR;
    }
    if(semicolon) expect(p,';');
    s->end=p->tokens[p->at-1].start+p->tokens[p->at-1].length;
    return s;
}
static Stmt *statement(VxnProgram *p) {
    Token at=peek(p);
    if(word(p,"const")||word(p,"mut")||word(p,"let")||word(p,"comptime")) return simple(p,true);
    Stmt *s=node(p,sizeof *s); s->token=at;
    if(take_word(p,"rec")) { s->kind=ST_BLOCK; s->recursive=true; s->body=block(p); }
    else if(take_word(p,"return")) {
        s->kind=ST_RETURN;
        /* Contextual keyword: preserve calls/values named tail. */
        s->tail=word(p,"tail")&&p->tokens[p->at+1].kind==TK_WORD;
        if(s->tail) next(p);
        if(peek(p).kind!=';') s->expr=expression(p);
        expect(p,';');
    } else if(take_word(p,"if")) {
        s->kind=ST_IF; expect(p,'('); s->expr=expression(p); expect(p,')'); s->body=block(p);
        if(take_word(p,"else")) {
            if(word(p,"if")) { enter(p); s->otherwise=statement(p); --p->depth; }
            else s->otherwise=block(p);
        }
    } else if(word(p,"while")||word(p,"for")) {
        bool is_for=take_word(p,"for"); if(!is_for) next(p);
        s->kind=ST_LOOP; expect(p,'(');
        if(is_for) {
            if(!take(p,';')) s->initial=simple(p,true);
            if(!take(p,';')) { s->expr=expression(p); expect(p,';'); }
            if(peek(p).kind!=')') s->step=simple(p,false);
            if(s->step&&s->step->kind==ST_BIND) vx_fail(p,s->step->token,"for update cannot declare a binding");
        } else s->expr=expression(p);
        expect(p,')');
        if(!s->expr) { s->expr=node(p,sizeof *s->expr); s->expr->kind=EX_BOOL; s->expr->value=1; s->expr->token=at; }
        s->body=block(p);
    } else if(take_word(p,"break")) { s->kind=ST_BREAK; expect(p,';'); }
    else if(take_word(p,"continue")) { s->kind=ST_CONTINUE; expect(p,';'); }
    else if(take_word(p,"match")) { s->kind=ST_MATCH; s->expr=expression(p); s->arms=arms(p); }
    else if(peek(p).kind=='{') { s->kind=ST_BLOCK; s->body=block(p); }
    else {
        static const char *deferred[]={"loadtime","dyntime","unsafe","emit"};
        for(size_t i=0;i<sizeof deferred/sizeof deferred[0];++i)
            if(word(p,deferred[i])) vx_fail(p,peek(p),"%s is not implemented; see the blocker ledger",deferred[i]);
        return simple(p,true);
    }
    s->end=p->tokens[p->at-1].start+p->tokens[p->at-1].length;
    return s;
}
static Stmt *block(VxnProgram *p) {
    enter(p); expect(p,'{'); Stmt *head=NULL,**tail=&head;
    while(!take(p,'}')) {
        if(peek(p).kind==TK_EOF) vx_fail(p,peek(p),"expected '}' before end of file");
        *tail=statement(p); tail=&(*tail)->next;
    }
    size_t end=p->tokens[p->at-1].start+p->tokens[p->at-1].length;
    for(Stmt *s=head;s;s=s->next) s->scope_end=end;
    --p->depth; return head;
}
static Type type_parameter(VxnProgram *p,bool callable) {
    Token n=name(p,true); Type t=vx_parameter_type(p,n,(Token){0}); TypeDef *d=p->types[t];
    if(take(p,'[')) {
        if(!callable) vx_fail(p,n,"constructor parameters currently require generic functions");
        do {
            if(!take_word(p,"_")) vx_fail(p,peek(p),"constructor kind requires _ slots");
            if(++d->constructor_arity>VX_MAX_GENERICS) vx_fail(p,n,"constructor arity limit exceeded");
        } while(take(p,','));
        expect(p,']');
    }
    if(take(p,':')) do {
        if(word(p,"safe")||word(p,"fn")) {
            if(!callable) vx_fail(p,peek(p),"callable bounds currently require function type parameters");
            if(d->callable_bound) vx_fail(p,peek(p),"duplicate callable bound");
            d->callable_bound=type(p); d->signature=p->types[d->callable_bound]->signature;
            continue;
        }
        Token b=symbol(p,true);
        int ownership=!b.qualifier?(vx_word(p,b,"Copy")?1:vx_word(p,b,"Move")?2:0):0;
        if(ownership) {
            if(d->ownership) vx_fail(p,b,"duplicate or conflicting Copy/Move bound");
            d->ownership=ownership;
        } else {
            if(d->bound_count==VX_MAX_GENERICS) vx_fail(p,b,"operation bound limit exceeded (8)");
            d->bound_names[d->bound_count++]=b;
        }
    } while(take(p,'&'));
    if(d->constructor_arity&&!d->ownership) d->ownership=2;
    if(d->constructor_arity&&(d->ownership==1||d->bound_count||d->callable_bound))
        vx_fail(p,n,"constructor parameters accept owned arguments; operation/callable constructor bounds are not implemented");
    return t;
}
static int type_parameters(VxnProgram *p,Type *types,bool callable) {
    int count=0; expect(p,'['); p->parse_type_count=0;
    do {
        if(count==VX_MAX_GENERICS) vx_fail(p,peek(p),"generic parameter limit exceeded (8)");
        Type t=type_parameter(p,callable); Token n=p->types[t]->name;
        for(int j=0;j<count;++j) if(vx_same(p,n,p->types[types[j]]->name)) vx_fail(p,n,"duplicate type parameter");
        types[count++]=t;
        p->parse_type_count=count; memcpy(p->parse_types,types,(size_t)count*sizeof(Type));
    } while(take(p,','));
    expect(p,']'); p->parse_type_count=count; memcpy(p->parse_types,types,(size_t)count*sizeof(Type)); return count;
}
static void aggregate(VxnProgram *p,bool choice,bool public_api) {
    Token name_token=name(p,true);
    if(vx_word(p,name_token,"Copy")||vx_word(p,name_token,"Move")) vx_fail(p,name_token,"Copy and Move are reserved ownership bounds");
    if(vx_symbol_module(p,name_token)!=name_token.module) vx_fail(p,name_token,"type declaration conflicts with a selective import");
    Type id=named_type(p,name_token);
    if(id<TY_AGGREGATE) vx_fail(p,name_token,"cannot redefine opaque %s",vx_type(id));
    TypeDef *d=p->types[id];
    if(d->kind!=TD_UNRESOLVED) vx_fail(p,name_token,"duplicate type");
    d->kind=choice?TD_CHOICE:TD_STRUCT; d->public_api=public_api;
    if(peek(p).kind=='[') d->generic_count=type_parameters(p,d->generic_types,false);
    expect(p,'{');
    while(!take(p,'}')) {
        if(d->count==VX_MAX_FIELDS) vx_fail(p,peek(p),"field/constructor limit exceeded");
        if(choice) {
            Variant *v=&d->variants[d->count++]; v->name=name(p,true);
            if(take(p,'(')) {
                if(peek(p).kind!=')') do {
                    if(v->count==VX_MAX_FIELDS) vx_fail(p,peek(p),"payload limit exceeded");
                    v->fields[v->count++]=type(p);
                } while(take(p,','));
                expect(p,')');
            }
            if(take(p,'}')) break;
            expect(p,',');
        } else {
            Field *f=&d->fields[d->count++]; f->type=type(p); f->name=name(p,false); expect(p,';');
        }
    }
    if(choice&&!d->count) vx_fail(p,name_token,"choice must have a constructor");
    p->parse_type_count=0;
}
/* Expose header type parameters to the return type. */
static size_t generic_header(VxnProgram *p,Function *f) {
    size_t saved=p->at,end=0;
    for(size_t i=saved;i+1<p->token_count;++i) {
        int k=p->tokens[i].kind;
        if(k=='{'||k==TK_EOF) break;
        if(k=='(') {
            bool callable_group=i>saved&&(vx_word(p,p->tokens[i-1],"fn")||vx_word(p,p->tokens[i-1],"from")||
                (i>saved+1&&vx_word(p,p->tokens[i-2],"fn")&&
                 (vx_word(p,p->tokens[i-1],"mut")||vx_word(p,p->tokens[i-1],"once"))));
            if(!callable_group) break;
            int nesting=1;
            while(nesting&&++i<p->token_count) {
                if(p->tokens[i].kind=='(') ++nesting;
                else if(p->tokens[i].kind==')') --nesting;
                else if(p->tokens[i].kind==TK_EOF) break;
            }
            if(nesting) break;
        }
        if(k=='[') {
            size_t after=bracket_end(p,i); if(!after) break;
            if(after<p->token_count && p->tokens[after].kind=='(') {
                p->at=i; f->generic_count=type_parameters(p,f->generic_types,true); end=p->at; break;
            }
            i=after-1;
        }
    }
    p->at=saved; p->parse_type_count=f->generic_count;
    memcpy(p->parse_types,f->generic_types,(size_t)f->generic_count*sizeof(Type));
    return end;
}
/* One definition per equation group. Pack parameters into a private product
   to reuse match coverage, guards, ownership and specialization. */
static Stmt *equations(VxnProgram *p,Function *f) {
    if(p->type_count==VX_MAX_TYPES) vx_fail(p,f->name,"equation product exceeds type budget");
    Type base=p->type_count++; TypeDef *d=node(p,sizeof *d); p->types[base]=d;
    char spelling[48]; snprintf(spelling,sizeof spelling,"$equations_%d",base);
    char *stored=vx_alloc(p,strlen(spelling)+1); strcpy(stored,spelling);
    d->kind=TD_STRUCT; d->name=vx_synthetic(f->name,stored); d->count=f->param_count;
    d->generic_count=f->generic_count;
    memcpy(d->generic_types,f->generic_types,(size_t)f->generic_count*sizeof(Type));
    for(int i=0;i<f->param_count;++i) d->fields[i]=(Field){.name=f->params[i].name,.type=f->params[i].type};
    Type product=f->generic_count?vx_apply_type(p,base,f->generic_types,f->generic_count,f->name):base;
    Stmt *s=node(p,sizeof *s); s->kind=ST_MATCH; s->token=f->name;
    s->expr=node(p,sizeof *s->expr); s->expr->kind=EX_RECORD; s->expr->token=f->name; s->expr->type=product;
    Arg **argument=&s->expr->args;
    for(int i=0;i<f->param_count;++i) {
        Arg *a=node(p,sizeof *a); a->name=f->params[i].name;
        a->expr=node(p,sizeof *a->expr); a->expr->kind=EX_VAR; a->expr->token=a->name;
        *argument=a; argument=&a->next; ++s->expr->argc;
    }
    Arm **tail=&s->arms; int count=0;
    do {
        if(!vx_same(p,peek(p),f->name)) vx_fail(p,peek(p),"function signature must be followed immediately by equations for the same function");
        if(++count>VX_MAX_FIELDS) vx_fail(p,peek(p),"function equation limit exceeded (32)");
        Arm *a=node(p,sizeof *a); a->token=next(p);
        Pattern *n=a->pattern=node(p,sizeof *n); n->kind=PAT_STRUCT; n->token=a->token; n->type=product;
        expect(p,'('); Pattern **child=&n->children;
        if(peek(p).kind!=')') do {
            if(n->count>=f->param_count) vx_fail(p,peek(p),"equation has too many parameter patterns");
            *child=pattern(p,a); (*child)->field=f->params[n->count++].name; child=&(*child)->next;
        } while(take(p,','));
        expect(p,')');
        if(n->count!=f->param_count) vx_fail(p,a->token,"equation must match every parameter");
        if(take_word(p,"if")) a->guard=expression(p);
        expect(p,'=');
        if(peek(p).kind=='{') a->body=block(p);
        else {
            Stmt *ret=a->body=node(p,sizeof *ret); ret->kind=ST_RETURN; ret->token=peek(p);
            ret->tail=word(p,"tail")&&p->tokens[p->at+1].kind==TK_WORD;
            if(ret->tail) next(p);
            ret->expr=expression(p);
        }
        expect(p,';'); a->end=p->tokens[p->at-1].start+p->tokens[p->at-1].length;
        if(a->body->kind==ST_RETURN) a->body->end=a->body->scope_end=a->end;
        *tail=a; tail=&a->next;
    } while(vx_same(p,peek(p),f->name));
    s->end=s->scope_end=p->tokens[p->at-1].start+p->tokens[p->at-1].length;
    return s;
}
static Function *function(VxnProgram *p,bool method,bool prototype) {
    if(p->function_count==VX_MAX_FUNCTIONS) vx_fail(p,peek(p),"function limit exceeded");
    Function *f=node(p,sizeof *f); f->operator_code=-1; f->public_api=take_word(p,"pub");
    f->comptime=take_word(p,"comptime");
    if(method && (f->public_api||f->comptime)) vx_fail(p,peek(p),"operators require ordinary safe declarations without pub/comptime");
    if(f->comptime&&!word(p,"safe")) {
        if(p->global_count==VX_MAX_FUNCTIONS) vx_fail(p,peek(p),"comptime constant limit exceeded");
        if(word(p,"mut")) vx_fail(p,peek(p),"comptime implies const; a comptime binding cannot be mut");
        take_word(p,"const"); Global *g=&p->globals[p->global_count++];
        g->type=type(p); g->name=name(p,false); expect(p,'='); g->expr=expression(p); expect(p,';');
        g->context=f; g->public_api=f->public_api; f->name=g->name; f->result=g->type;
        return NULL;
    }
    if(!take_word(p,"safe")) vx_fail(p,peek(p),"functions must start with safe; unsafe and staging are not implemented yet");
    f->cap_token=peek(p);
    for(;;) {
        unsigned cap=word(p,"pure")?CP_PURE:word(p,"io")?CP_IO:word(p,"hot")?CP_HOT:word(p,"noalloc")?CP_NOALLOC:word(p,"nopanic")?CP_NOPANIC:word(p,"noblock")?CP_NOBLOCK:0;
        if(!cap) break;
        if(f->caps&cap) vx_fail(p,peek(p),"duplicate effect annotation");
        f->caps|=cap; next(p);
    }
    size_t generic_end=method?0:generic_header(p,f);
    if(method&&!prototype) {
        f->generic_count=p->parse_type_count;
        memcpy(f->generic_types,p->parse_types,(size_t)f->generic_count*sizeof(Type));
    }
    f->result=type(p);
    if(vx_opaque_result(p,f->result)) {
        if(method) vx_fail(p,f->name,"opaque callable returns in constraint operations require associated result contracts");
        TypeDef *d=p->types[vx_opaque_result(p,f->result)]; d->definition=f; d->generic_count=f->generic_count;
        memcpy(d->generic_types,f->generic_types,(size_t)f->generic_count*sizeof(Type));
    }
    if(method) f->operator_impl=!prototype;
    if(method && take_word(p,"operator")) {
        f->name=next(p);
        int op=0;
        if(f->name.kind=='!') op=VX_NOT;
        else if(f->name.kind=='~') op=VX_INV;
        else if(!precedence(f->name.kind,&op) || op==VX_LAND || op==VX_LOR)
            vx_fail(p,f->name,"unsupported overload; short-circuit operators retain builtin semantics");
        f->operator_code=op;
    } else {
        f->name=name(p,false);
        if(generic_end) { expect(p,'['); p->at=generic_end; }
    }
    expect(p,'(');
    if(peek(p).kind!=')') do {
        if(f->param_count==VX_MAX_PARAMS) vx_fail(p,peek(p),"parameter limit exceeded");
        Param *a=&f->params[f->param_count++];
        if(take_word(p,"mut")) a->mutable_binding=true;
        else if(!take_word(p,"const")) vx_fail(p,peek(p),"parameters require const or mut");
        a->type=type(p); a->name=name(p,false);
        a->noescape=take_word(p,"noescape");
        if(take(p,'=')) a->default_value=expression(p);
    } while(take(p,','));
    expect(p,')');
    if(method && f->param_count==1) {
        if(f->operator_code==VX_ADD) f->operator_code=VX_POS;
        if(f->operator_code==VX_SUB) f->operator_code=VX_NEG;
    }
    if(take_word(p,"from")) {
        f->from_explicit=true; expect(p,'(');
        if(peek(p).kind!=')') do {
            if(f->from_count==VX_MAX_PARAMS) vx_fail(p,peek(p),"too many from origins");
            f->from_names[f->from_count++]=name(p,false);
        } while(take(p,','));
        expect(p,')');
    }
    if(prototype) expect(p,';');
    else {
        p->functions[p->function_count++]=f;
        if(take(p,'=')) {
            Stmt *s=f->body=node(p,sizeof *s); s->kind=ST_RETURN; s->token=peek(p);
            s->tail=word(p,"tail")&&p->tokens[p->at+1].kind==TK_WORD;
            if(s->tail) next(p);
            s->expr=expression(p); expect(p,';');
            s->end=s->scope_end=p->tokens[p->at-1].start+p->tokens[p->at-1].length;
        } else if(take(p,';')) {
            if(method) vx_fail(p,f->name,"constraint implementations use an ordinary body, not equation heads");
            f->body=equations(p,f);
        } else f->body=block(p);
    }
    f->end=p->tokens[p->at-1].start+p->tokens[p->at-1].length;
    if(!method) p->parse_type_count=0;
    return f;
}
static void constraint_declaration(VxnProgram *p,bool public_api) {
    if(p->constraint_count==VX_MAX_CONSTRAINTS) vx_fail(p,peek(p),"constraint limit exceeded (64)");
    Constraint *c=&p->constraints[p->constraint_count++]; c->name=name(p,true); c->public_api=public_api;
    if(vx_word(p,c->name,"Copy")||vx_word(p,c->name,"Move")) vx_fail(p,c->name,"Copy and Move are reserved ownership bounds");
    expect(p,'['); c->self=type_parameter(p,false); expect(p,']');
    if(p->types[c->self]->bound_count) vx_fail(p,c->name,"constraint self parameter supports only Copy or Move");
    if(!p->types[c->self]->ownership) p->types[c->self]->ownership=1;
    p->parse_type_count=1; p->parse_types[0]=c->self;
    expect(p,'{');
    while(!take(p,'}')) {
        if(c->method_count==VX_MAX_FIELDS) vx_fail(p,peek(p),"constraint operation limit exceeded");
        c->methods[c->method_count++]=function(p,true,true);
    }
    if(!c->method_count) vx_fail(p,c->name,"constraint requires an operation");
    p->parse_type_count=0;
}
static void implementation_declaration(VxnProgram *p) {
    if(p->implementation_count==VX_MAX_IMPLS) vx_fail(p,peek(p),"implementation limit exceeded (128)");
    Implementation *i=&p->implementations[p->implementation_count++];
    if(peek(p).kind=='[') i->generic_count=type_parameters(p,i->generic_types,false);
    i->name=symbol(p,true);
    expect(p,'['); i->type=type(p); expect(p,']'); expect(p,'{');
    while(!take(p,'}')) {
        if(i->method_count==VX_MAX_FIELDS) vx_fail(p,peek(p),"implementation operation limit exceeded");
        i->methods[i->method_count++]=p->function_count;
        (void)function(p,true,false);
    }
    p->parse_type_count=0;
}
static void declarations(VxnProgram *p) {
    while(peek(p).kind!=TK_EOF) {
        bool pub=word(p,"pub");
        if(pub && (vx_word(p,p->tokens[p->at+1],"struct") || vx_word(p,p->tokens[p->at+1],"choice") || vx_word(p,p->tokens[p->at+1],"constraint"))) next(p);
        if(take_word(p,"struct")) aggregate(p,false,pub);
        else if(take_word(p,"choice")) aggregate(p,true,pub);
        else if(take_word(p,"constraint")) constraint_declaration(p,pub);
        else if(take_word(p,"impl")) implementation_declaration(p);
        else if(word(p,"import")) vx_fail(p,peek(p),"imports must precede declarations");
        else (void)function(p,false,false);
    }
}
static void add_import(VxnProgram *p,Token name_token,int target,bool selective) {
    Module *m=&p->modules[p->current_module];
    for(Import *i=m->imports;i;i=i->next)
        if(vx_same(p,i->name,name_token)) vx_fail(p,name_token,"duplicate import binding or alias");
    if(++p->import_count>256) vx_fail(p,name_token,"project import binding limit exceeded (256)");
    Import *i=node(p,sizeof *i); i->name=name_token; i->module=target; i->selective=selective;
    i->next=m->imports; m->imports=i;
}
static void imports(VxnProgram *p) {
    while(take_word(p,"import")) {
        char path[512]; size_t n=0; Token last={0};
        do {
            last=name(p,false);
            if(n+last.length+2>sizeof path) vx_fail(p,last,"logical module path is too long");
            if(n) path[n++]='.';
            memcpy(path+n,p->source+last.start,last.length); n+=last.length;
        } while(take(p,'.'));
        path[n]=0; int target=0;
        int package=p->modules[p->current_module].package;
        const char *local_path=path;
        for(const Dependency *d=p->packages[package].dependencies;d;d=d->next) {
            size_t length=strlen(d->alias);
            if(!strncmp(path,d->alias,length)&&path[length]=='.') {
                package=d->package; local_path=path+length+1; break;
            }
        }
        for(int i=1;i<p->module_count;++i)
            if(p->modules[i].package==package&&!strcmp(local_path,p->modules[i].name)) { target=i; break; }
        if(!target) vx_fail(p,last,"module '%s' is not mapped; supply a project manifest or source bundle",path);
        if(take(p,TK_SCOPE)) {
            if(peek(p).kind=='*') vx_fail(p,peek(p),"wildcard imports are not supported");
            expect(p,'{');
            if(peek(p).kind=='}') vx_fail(p,peek(p),"selective import must name at least one declaration");
            do {
                Token t=peek(p); bool upper=t.kind==TK_WORD && p->source[t.start]>='A' && p->source[t.start]<='Z';
                add_import(p,name(p,upper),target,true);
            } while(take(p,','));
            expect(p,'}');
        } else {
            if(take_word(p,"as")) last=name(p,false);
            add_import(p,last,target,false);
        }
        expect(p,';');
    }
}
static void parse_module(VxnProgram *p,int id,Token via) {
    Module *m=&p->modules[id];
    if(m->state==2) return;
    if(m->state==1) vx_fail(p,via,"circular module import involving '%s'",m->name);
    m->state=1;
    for(Import *i=m->imports;i;i=i->next) parse_module(p,i->module,i->name);
    p->current_module=id; p->tokens=m->tokens; p->token_count=m->token_count; p->at=m->body_start;
    declarations(p); m->state=2;
}
static const char *copy_text(VxnProgram *p,const char *s) {
    if(!s) return NULL;
    size_t n=strlen(s)+1; char *copy=vx_alloc(p,n); memcpy(copy,s,n); return copy;
}
void vx_compile_sources(VxnProgram *p,const VxnSource *sources,size_t count) {
    p->main_id=-1; p->type_count=TY_AGGREGATE;
    if(!sources || !count || count>VX_MAX_MODULES) vx_fail(p,(Token){0},"source bundle requires 1..64 modules");
    size_t total=0;
    for(size_t i=0;i<count;++i) {
        if(!sources[i].source) vx_fail(p,(Token){0},"source must not be null");
        if(!sources[i].name || !vx_module_name(sources[i].name)) vx_fail(p,(Token){0},"module names require dotted lower_snake_case segments");
        for(size_t j=0;j<i;++j) if(p->modules[i+1].package==p->modules[j+1].package&&!strcmp(sources[i].name,sources[j].name)) vx_fail(p,(Token){0},"duplicate logical module name");
        size_t n=0; while(n<=VX_MAX_SOURCE && sources[i].source[n]) ++n;
        if(n>VX_MAX_SOURCE-total) vx_fail(p,(Token){0},"project source exceeds prototype limit (1 MiB)");
        total+=n;
        Module *m=&p->modules[i+1]; m->name=copy_text(p,sources[i].name); m->path=copy_text(p,sources[i].path); m->length=n;
    }
    p->module_count=(int)count+1; if(!p->package) p->package="local";
    if(!p->package_count) { p->package_count=1; p->packages[0]=(Package){.name=p->package,.manifest=p->manifest,.state=2}; }
    const char *preludes[]={vx_io_declarations,vx_graphics_declarations,vx_window_declarations,vx_time_declarations,vx_memory_declarations,vx_parse_declarations,vx_text_declarations,vx_lazy_declarations};
    size_t prelude_size=0;
    for(size_t i=0;i<sizeof preludes/sizeof preludes[0];++i) prelude_size+=strlen(preludes[i]);
    p->source=vx_alloc(p,total+count+1+prelude_size); size_t offset=0;
    for(size_t i=0;i<count;++i) {
        Module *m=&p->modules[i+1]; m->start=offset;
        memcpy(p->source+offset,sources[i].source,m->length+1); offset+=m->length+1;
    }
    p->modules[0]=(Module){.name="std.prelude",.path="<standard prelude>",.start=offset,.length=prelude_size,.state=2};
    for(size_t i=0;i<sizeof preludes/sizeof preludes[0];++i) {
        size_t size=strlen(preludes[i]); memcpy(p->source+offset,preludes[i],size); offset+=size;
    }
    p->source[offset]=0;
    p->current_module=0; lex(p,p->modules[0].start,prelude_size); declarations(p);
    for(int i=TY_AGGREGATE;i<p->type_count;++i) {
        Token name=p->types[i]->name;
        if(vx_word(p,name,"Lazy")) { p->lazy_type=i; p->types[i]->lazy=true; }
        if(vx_word(p,name,"IoOpen")) p->io_open=i;
        if(vx_word(p,name,"IoRead")) p->io_read=i;
        if(vx_word(p,name,"IoReadBuffer")) p->io_read_buffer=i;
        if(vx_word(p,name,"IoLine")) p->io_line=i;
        if(vx_word(p,name,"IoResult")) p->io_result=i;
        for(int j=0;j<LT_COUNT;++j) if(vx_word(p,name,vx_library_type_names[j])) p->library_types[j]=i;
    }
    int builtin_types=p->type_count; p->builtin_functions=p->function_count;
    for(int i=1;i<p->module_count;++i) {
        Module *m=&p->modules[i]; p->current_module=i;
        lex(p,m->start,m->length); imports(p);
        m->tokens=p->tokens; m->token_count=p->token_count; m->body_start=p->at;
    }
    for(int i=1;i<p->module_count;++i) parse_module(p,i,(Token){.start=p->modules[i].start});
    if(p->function_count==p->builtin_functions && p->type_count==builtin_types && !p->global_count && !p->constraint_count) vx_fail(p,peek(p),"expected at least one function or type");
    vx_check(p);
}
VxnProgram *vxn_compile_sources(const VxnSource *sources,size_t count) {
    VxnProgram *p=calloc(1,sizeof *p); if(!p) return NULL;
    if(setjmp(p->failure)) return p;
    vx_compile_sources(p,sources,count); return p;
}
VxnProgram *vxn_compile(const char *source) {
    const VxnSource input={"entry",NULL,source}; return vxn_compile_sources(&input,1);
}
