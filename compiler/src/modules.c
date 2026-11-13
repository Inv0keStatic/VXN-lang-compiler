#include "internal.h"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

bool vx_module_name(const char *name) {
    if(!name || !*name || strlen(name)>511) return false;
    bool start=true,underscore=false; size_t segment=0;
    for(const char *s=name;*s;++s) {
        if(*s=='.') { if(start||underscore) return false; start=true; segment=0; continue; }
        if(start && (*s<'a'||*s>'z')) return false;
        if(!((*s>='a'&&*s<='z')||(*s>='0'&&*s<='9')||*s=='_')) return false;
        if(*s=='_'&&underscore) return false;
        underscore=*s=='_'; start=false; if(++segment>80) return false;
    }
    return !start&&!underscore;
}
int vx_symbol_module(const VxnProgram *p,Token name) {
    if(name.qualifier) return name.qualifier;
    for(const Import *i=p->modules[name.module].imports;i;i=i->next)
        if(i->selective&&vx_same(p,i->name,name)) return i->module;
    return name.module;
}
bool vx_visible(VxnProgram *p,Token use,Token declaration,bool public_api) {
    if(declaration.module && declaration.module!=use.module && !public_api)
        vx_fail(p,use,"declaration is private to module '%s'",p->modules[declaration.module].name);
    return true;
}
static void exported_type(VxnProgram *p,Type t,Token at) {
    if(vx_ref(t)) t=vx_referent(t);
    while(t>=TY_AGGREGATE && (p->types[t]->kind==TD_ARRAY||vx_heap_type(p,t)||vx_slice(p,t)||vx_str(p,t)||vx_fn_view(p,t)||vx_dyn_fn(p,t))) t=p->types[t]->element;
    if(vx_callable(p,t)||vx_opaque(p,t)) {
        Function *f=p->types[t]->signature; exported_type(p,f->result,at);
        for(int i=0;i<f->param_count;++i) exported_type(p,f->params[i].type,at);
        return;
    }
    if(t>=TY_AGGREGATE && p->types[t]->template_type)
        for(int i=0;i<p->types[t]->type_arg_count;++i) exported_type(p,p->types[t]->type_args[i],at);
    if(t>=TY_AGGREGATE && p->types[t]->kind!=TD_PARAMETER && p->types[t]->name.module && !p->types[t]->public_api)
        vx_fail(p,at,"public interface exposes a private type; mark the type pub");
}
void vx_check_interfaces(VxnProgram *p) {
    for(int m=1;m<p->module_count;++m) for(Import *i=p->modules[m].imports;i;i=i->next) {
        bool found=false;
        for(int f=0;f<p->function_count;++f) {
            Function *d=p->functions[f]; if(d->anonymous) continue;
            if(!vx_same(p,i->name,d->name)) continue;
            if(d->name.module==m || !d->name.module) vx_fail(p,i->name,"import binding conflicts with a declaration or builtin");
            if(i->selective&&d->name.module==i->module) { vx_visible(p,i->name,d->name,d->public_api); found=true; }
        }
        for(int g=0;g<p->global_count;++g) {
            Global *d=&p->globals[g]; if(!vx_same(p,i->name,d->name)) continue;
            if(d->name.module==m) vx_fail(p,i->name,"import binding conflicts with a declaration");
            if(i->selective&&d->name.module==i->module) { vx_visible(p,i->name,d->name,d->public_api); found=true; }
        }
        for(int t=TY_AGGREGATE;t<p->type_count;++t) {
            TypeDef *d=p->types[t]; if(d->kind==TD_FN_VIEW||d->kind==TD_UNIQ_FN_VIEW||d->kind==TD_ARRAY||d->kind==TD_SLICE||d->kind==TD_UNIQ_SLICE||d->kind==TD_CALLABLE||d->kind==TD_PARAMETER||d->template_type||!vx_same(p,i->name,d->name)) continue;
            if(d->name.module==m || !d->name.module) vx_fail(p,i->name,"import binding conflicts with a type declaration");
            if(i->selective&&d->name.module==i->module) { vx_visible(p,i->name,d->name,d->public_api); found=true; }
        }
        for(int c=0;c<p->constraint_count;++c) {
            Constraint *d=&p->constraints[c]; if(!vx_same(p,i->name,d->name)) continue;
            if(d->name.module==m) vx_fail(p,i->name,"import binding conflicts with a constraint");
            if(i->selective&&d->name.module==i->module) { vx_visible(p,i->name,d->name,d->public_api); found=true; }
        }
        if(vx_word(p,i->name,"fn_ref")||vx_word(p,i->name,"fn_uniq")||vx_word(p,i->name,"output")||vx_word(p,i->name,"panic")||vx_word(p,i->name,"len")||vx_word(p,i->name,"slice")||vx_io_builtin(p,i->name)>=0||vx_library_builtin(p,i->name)>=0)
            vx_fail(p,i->name,"import binding conflicts with a builtin");
        if(i->selective&&!found) vx_fail(p,i->name,"unknown declaration in selective import");
    }
    for(int i=0;i<p->function_count;++i) {
        Function *f=p->functions[i]; if(!f->public_api) continue;
        for(int j=0;j<f->generic_count;++j) {
            const TypeDef *d=p->types[f->generic_types[j]];
            if(d->callable_bound) exported_type(p,d->callable_bound,f->name);
        }
        exported_type(p,f->result,f->name);
        for(int j=0;j<f->param_count;++j) exported_type(p,f->params[j].type,f->params[j].name);
    }
    for(int i=0;i<p->constraint_count;++i) if(p->constraints[i].public_api) {
        Constraint *c=&p->constraints[i];
        for(int j=0;j<c->method_count;++j) {
            Function *f=c->methods[j]; exported_type(p,f->result,f->name);
            for(int k=0;k<f->param_count;++k) exported_type(p,f->params[k].type,f->params[k].name);
        }
    }
    for(int i=0;i<p->global_count;++i) if(p->globals[i].public_api) exported_type(p,p->globals[i].type,p->globals[i].name);
    for(int t=TY_AGGREGATE;t<p->type_count;++t) {
        TypeDef *d=p->types[t]; if(!d->public_api||d->template_type) continue;
        for(int i=0;i<d->generic_count;++i) {
            const TypeDef *param=p->types[d->generic_types[i]];
            for(int j=0;j<param->bound_count;++j) if(!p->constraints[param->bounds[j]].public_api)
                vx_fail(p,d->name,"public generic type exposes a private constraint");
        }
        for(int i=0;i<d->count;++i) {
            if(d->kind==TD_STRUCT) exported_type(p,d->fields[i].type,d->fields[i].name);
            else for(int j=0;j<d->variants[i].count;++j) exported_type(p,d->variants[i].fields[j],d->variants[i].name);
        }
    }
}
bool vx_same_file(const char *a,const char *b) {
    if(!a||!b) return false;
#ifdef _WIN32
    char aa[32768],bb[32768];
    DWORD na=GetFullPathNameA(a,sizeof aa,aa,NULL),nb=GetFullPathNameA(b,sizeof bb,bb,NULL);
    if(!na||!nb||na>=sizeof aa||nb>=sizeof bb) return true;
    if(_stricmp(aa,bb)==0) return true;
    HANDLE ha=CreateFileA(a,0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    HANDLE hb=CreateFileA(b,0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    BY_HANDLE_FILE_INFORMATION ia,ib;
    bool equal=ha!=INVALID_HANDLE_VALUE&&hb!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(ha,&ia)&&GetFileInformationByHandle(hb,&ib)&&
        ia.dwVolumeSerialNumber==ib.dwVolumeSerialNumber&&ia.nFileIndexHigh==ib.nFileIndexHigh&&ia.nFileIndexLow==ib.nFileIndexLow;
    if(ha!=INVALID_HANDLE_VALUE) CloseHandle(ha);
    if(hb!=INVALID_HANDLE_VALUE) CloseHandle(hb);
    return equal;
#else
    struct stat aa,bb;
    return !strcmp(a,b)||(stat(a,&aa)==0&&stat(b,&bb)==0&&aa.st_dev==bb.st_dev&&aa.st_ino==bb.st_ino);
#endif
}
static char *copy(VxnProgram *p,const char *s,size_t n) {
    char *result=vx_alloc(p,n+1); memcpy(result,s,n); result[n]=0; return result;
}
static char *read_file(VxnProgram *p,const char *path) {
    FILE *file=fopen(path,"rb");
    if(!file) vx_fail(p,(Token){0},"cannot open source or manifest: %s",path);
    bool bad=fseek(file,0,SEEK_END)!=0; long length=bad?-1:ftell(file); fclose(file);
    if(length<0 || (unsigned long)length>VX_MAX_SOURCE) vx_fail(p,(Token){0},"%s: source size unavailable or exceeds 1 MiB",path);
    char *text=vx_alloc(p,(size_t)length+2);
    file=fopen(path,"rb"); if(!file) vx_fail(p,(Token){0},"cannot reopen source or manifest: %s",path);
    size_t n=fread(text,1,(size_t)length+1,file); bad=ferror(file)!=0; fclose(file);
    if(bad || n!=(size_t)length || memchr(text,0,n)) vx_fail(p,(Token){0},"%s: %s",path,bad?"read failed":n!=(size_t)length?"source changed while being read":"embedded NUL byte in source");
    text[n]=0; return text;
}
static char *skip_space(char *s) { while(*s==' '||*s=='\t'||*s=='\r') ++s; return s; }
static char *manifest_word(VxnProgram *p,char **cursor) {
    char *s=skip_space(*cursor),*end=s;
    while((*end>='a'&&*end<='z')||(*end>='0'&&*end<='9')||*end=='_'||*end=='.') ++end;
    char *word=copy(p,s,(size_t)(end-s));
    if(!vx_module_name(word)) vx_fail(p,(Token){0},"manifest requires a dotted lower_snake_case name");
    *cursor=end; return word;
}
static const char *relative_path(VxnProgram *p,const char *manifest,const char *path,size_t n) {
    if(!n || n>4095) vx_fail(p,(Token){0},"manifest path must contain 1..4095 bytes");
    bool absolute=path[0]=='/'||path[0]=='\\'||(n>1&&path[1]==':');
    size_t prefix=0;
    if(!absolute) for(size_t i=0;manifest[i];++i) if(manifest[i]=='/'||manifest[i]=='\\') prefix=i+1;
    char *result=vx_alloc(p,prefix+n+1); memcpy(result,manifest,prefix); memcpy(result+prefix,path,n); result[prefix+n]=0; return result;
}
typedef struct { VxnSource sources[VX_MAX_MODULES]; int owners[VX_MAX_MODULES]; size_t count; } SourceGraph;
static int load_package(VxnProgram *p,SourceGraph *graph,const char *manifest) {
    for(int i=0;i<p->package_count;++i) if(vx_same_file(p->packages[i].manifest,manifest)) {
        if(p->packages[i].state==1) vx_fail(p,(Token){0},"circular package dependency: %s",manifest);
        return i;
    }
    if(p->package_count==VX_MAX_MODULES) vx_fail(p,(Token){0},"package limit exceeded (64)");
    int id=p->package_count++; Package *package=&p->packages[id];
    package->manifest=copy(p,manifest,strlen(manifest)); package->state=1;
    char *text=read_file(p,manifest);
    for(char *line=text;*line;) {
        char *next=strchr(line,'\n'); if(next) *next++=0;
        char *s=skip_space(line);
        if(*s&&*s!='#') {
            char *keyword=manifest_word(p,&s);
            if(!strcmp(keyword,"package")) {
                if(package->name) vx_fail(p,(Token){0},"duplicate manifest package");
                package->name=manifest_word(p,&s);
            } else if(!strcmp(keyword,"module")||!strcmp(keyword,"dependency")) {
                bool dependency=!strcmp(keyword,"dependency");
                const char *name=manifest_word(p,&s); s=skip_space(s);
                if(dependency&&strchr(name,'.')) vx_fail(p,(Token){0},"dependency alias must be one lower_snake_case segment");
                if(*s++!='=') vx_fail(p,(Token){0},"manifest mapping requires '='");
                s=skip_space(s); if(*s++!='\"') vx_fail(p,(Token){0},"manifest mapping path must be quoted");
                char *end=strchr(s,'\"'); if(!end) vx_fail(p,(Token){0},"unterminated manifest path");
                const char *path=relative_path(p,manifest,s,(size_t)(end-s)); s=end+1;
                if(dependency) {
                    if(++p->dependency_count>256) vx_fail(p,(Token){0},"package dependency limit exceeded (256)");
                    for(const Dependency *d=package->dependencies;d;d=d->next)
                        if(!strcmp(d->alias,name)) vx_fail(p,(Token){0},"duplicate dependency alias");
                    Dependency *d=vx_alloc(p,sizeof *d); d->alias=name;
                    d->package=load_package(p,graph,path); d->next=package->dependencies; package->dependencies=d;
                } else {
                    if(graph->count==VX_MAX_MODULES) vx_fail(p,(Token){0},"project module limit exceeded (64)");
                    graph->owners[graph->count]=id;
                    graph->sources[graph->count++]=(VxnSource){name,path,NULL};
                }
            } else vx_fail(p,(Token){0},"manifest accepts only package, module and dependency entries");
            s=skip_space(s); if(*s&&*s!='#') vx_fail(p,(Token){0},"unexpected text after manifest entry");
        }
        if(!next) break;
        line=next;
    }
    if(!package->name) vx_fail(p,(Token){0},"project manifest requires a package name");
    for(int i=0;i<p->package_count;++i) if(i!=id&&p->packages[i].name&&!strcmp(package->name,p->packages[i].name))
        vx_fail(p,(Token){0},"package identity '%s' is mapped by different manifests",package->name);
    for(size_t i=0;i<graph->count;++i) if(graph->owners[i]==id) {
        const char *module=graph->sources[i].name;
        for(const Dependency *d=package->dependencies;d;d=d->next) {
            size_t n=strlen(d->alias);
            if(!strncmp(module,d->alias,n)&&(!module[n]||module[n]=='.'))
                vx_fail(p,(Token){0},"dependency alias conflicts with a local module namespace");
        }
    }
    package->state=2; return id;
}
VxnProgram *vxn_compile_file(const char *path,const char *manifest) {
    VxnProgram *p=calloc(1,sizeof *p); if(!p) return NULL;
    p->input_error=true;
    if(setjmp(p->failure)) return p;
    if(!path) vx_fail(p,(Token){0},"source path must not be null");
    SourceGraph *graph=vx_alloc(p,sizeof *graph);
    if(manifest) {
        p->manifest=copy(p,manifest,strlen(manifest)); (void)load_package(p,graph,manifest);
        p->package=p->packages[0].name;
        size_t entry=graph->count;
        for(size_t i=0;i<graph->count;++i) {
            const char *source=graph->sources[i].path;
            if(!graph->owners[i]&&vx_same_file(path,source)) entry=i;
            for(int j=0;j<p->package_count;++j) if(vx_same_file(p->packages[j].manifest,source))
                vx_fail(p,(Token){0},"manifest cannot also be a module source");
            for(size_t j=0;j<i;++j) if(vx_same_file(source,graph->sources[j].path))
                vx_fail(p,(Token){0},"a physical source file cannot have multiple module identities");
        }
        if(entry==graph->count) vx_fail(p,(Token){0},"entry source is not mapped by the root project manifest");
        VxnSource first=graph->sources[0]; graph->sources[0]=graph->sources[entry]; graph->sources[entry]=first;
        int owner=graph->owners[0]; graph->owners[0]=graph->owners[entry]; graph->owners[entry]=owner;
    } else graph->sources[graph->count++]=(VxnSource){"entry",path,NULL};
    for(size_t i=0;i<graph->count;++i) {
        graph->sources[i].source=read_file(p,graph->sources[i].path);
        p->modules[i+1].package=graph->owners[i];
    }
    p->input_error=false; vx_compile_sources(p,graph->sources,graph->count); return p;
}
static const char *package_name(const VxnProgram *p,int module) {
    return module?p->packages[p->modules[module].package].name:"vxn";
}

static void json_string(FILE *out,const char *s) {
    fputc('"',out);
    for(const unsigned char *c=(const unsigned char *)s;*c;++c) {
        if(*c=='"'||*c=='\\') fprintf(out,"\\%c",*c);
        else if(*c<32) fprintf(out,"\\u%04x",(unsigned)*c);
        else fputc(*c,out);
    }
    fputc('"',out);
}
static void json_name(const VxnProgram *p,FILE *out,Token name) {
    char text[81]; vx_name(p,name,text,sizeof text); json_string(out,text);
}
static void type_text(const VxnProgram *p,FILE *out,Type t) {
    if(vx_ref(t)) { fputs(vx_uniq(t)?(vx_writable(t)?"uniq mut ":"uniq const "):"ref ",out); type_text(p,out,vx_referent(t)); }
    else if(t<TY_AGGREGATE) fputs(vx_type(t),out);
    else {
        const TypeDef *d=p->types[t];
        if(vx_callable(p,t)||vx_opaque(p,t)) {
            const Function *f=d->signature; static const char *const caps[]={"pure","io","noalloc","nopanic","noblock","hot"};
            if(vx_opaque(p,t)) {
                Token name=d->definition->name;
                fprintf(out,"opaque(%s::%s::%.*s",package_name(p,name.module),p->modules[name.module].name,(int)name.length,p->source+name.start);
                int count=d->template_type?d->type_arg_count:d->generic_count;
                const Type *args=d->template_type?d->type_args:d->generic_types;
                if(count) {
                    fputc('[',out);
                    for(int i=0;i<count;++i) { if(i) fputc(',',out); type_text(p,out,args[i]); }
                    fputc(']',out);
                }
                fputs(") ",out);
            }
            fputs("safe ",out);
            for(int i=0;i<6;++i) if(f->caps&(1u<<i)) fprintf(out,"%s ",caps[i]);
            fputs(f->call_mode==2?"fn once(":f->call_mode==1?"fn mut(":"fn(",out);
            for(int i=0;i<f->param_count;++i) {
                const Param *a=&f->params[i]; fputs(i?", const ":"const ",out); type_text(p,out,a->type);
                if(a->name.length) fprintf(out," %.*s",(int)a->name.length,p->source+a->name.start);
                if(a->noescape) fputs(" noescape",out);
            }
            fputs(") -> ",out); type_text(p,out,f->result);
            if(f->from_mask||f->from_explicit) {
                fputs(" from(",out); bool comma=false;
                for(int i=0;i<f->param_count;++i) if(f->from_mask&(UINT32_C(1)<<i)) {
                    fprintf(out,"%s%d",comma?",":"",i); comma=true;
                }
                fputc(')',out);
            }
        }
        else if(d->kind==TD_PARAMETER&&!d->template_type) { char name[81]; vx_name(p,d->name,name,sizeof name); fputs(name,out); }
        else if(d->template_type) {
            type_text(p,out,d->template_type-1); fputc('[',out);
            for(int i=0;i<d->type_arg_count;++i) { if(i) fputc(',',out); type_text(p,out,d->type_args[i]); }
            fputc(']',out);
        }
        else if(d->kind==TD_STRING) fputs("String",out);
        else if(vx_str(p,t)) fputs("str",out);
        else if(vx_fn_view(p,t)||vx_dyn_fn(p,t)) { fputs(vx_dyn_fn(p,t)?"DynFn[":vx_uniq_fn_view(p,t)?"FnUniq[":"FnRef[",out); type_text(p,out,d->element); fputc(']',out); }
        else if(d->kind==TD_SLICE||d->kind==TD_UNIQ_SLICE||d->kind==TD_BOX||d->kind==TD_VEC) { fputs(d->kind==TD_BOX?"Box[":d->kind==TD_VEC?"Vec[":d->kind==TD_UNIQ_SLICE?"UniqSlice[":"Slice[",out); type_text(p,out,d->element); fputc(']',out); }
        else if(d->kind==TD_ARRAY) {
            Type base=t;
            while(base>=TY_AGGREGATE && p->types[base]->kind==TD_ARRAY) base=p->types[base]->element;
            type_text(p,out,base);
            while(t>=TY_AGGREGATE && p->types[t]->kind==TD_ARRAY) { fprintf(out,"[%d]",p->types[t]->count); t=p->types[t]->element; }
        }
        else {
            char name[81]; vx_name(p,d->name,name,sizeof name);
            fprintf(out,"%s::%s::%s",package_name(p,d->name.module),p->modules[d->name.module].name,name);
        }
    }
}
static void json_type(const VxnProgram *p,FILE *out,Type t) {
    fputc('"',out); type_text(p,out,t); fputc('"',out);
}
static void json_flags(FILE *out,unsigned bits,const char *const *names,int count) {
    fputc('[',out); bool comma=false;
    for(int i=0;i<count;++i) if(bits&(1u<<i)) { if(comma) fputc(',',out); json_string(out,names[i]); comma=true; }
    fputc(']',out);
}
static void json_value(const VxnProgram *p,FILE *out,const Expr *value) {
    fputs("{\"type\":",out); json_type(p,out,value->type); fputs(",\"cells_hex\":[",out);
    for(int i=0;i<vx_slots(p,value->type);++i)
        fprintf(out,"%s\"%016" PRIx64 "\"",i?",":"",vx_indirect(value->type)?value->cells[i]:value->value);
    fputs("]}",out);
}
static void json_constraint(const VxnProgram *p,FILE *out,int index) {
    const Constraint *c=&p->constraints[index]; char name[81]; vx_name(p,c->name,name,sizeof name);
    fprintf(out,"\"%s::%s::%s\"",package_name(p,c->name.module),p->modules[c->name.module].name,name);
}
static void json_bounds(const VxnProgram *p,FILE *out,const TypeDef *d) {
    fputc('[',out);
    for(int i=0;i<d->bound_count;++i) { if(i) fputc(',',out); json_constraint(p,out,d->bounds[i]); }
    fputc(']',out);
}
static void json_operator(const VxnProgram *p,FILE *out,const Function *f,bool contract) {
    static const char *const effects[]={"io","alloc","panic","block","write"};
    fputs("{\"operator\":",out); json_string(out,vx_op_name(f->operator_code));
    fprintf(out,",\"arity\":%d,\"result\":",f->param_count); json_type(p,out,f->result);
    fputs(",\"parameters\":[",out);
    for(int i=0;i<f->param_count;++i) { if(i) fputc(',',out); json_type(p,out,f->params[i].type); }
    fputs(contract?"],\"allowed_effects\":" : "],\"effects\":",out);
    json_flags(out,contract?(EF_ALL&~vx_forbidden(f->caps)):f->effects,effects,5); fputc('}',out);
}
static void json_ownership(FILE *out,const TypeDef *d) {
    if(d->constructor_arity) fprintf(out,",\"kind_arity\":%d",d->constructor_arity);
    fputs(",\"ownership\":",out);
    json_string(out,d->ownership==2?"move":d->ownership==1?"copy":"unspecified");
}
static void json_method(const VxnProgram *p,FILE *out,const Function *f,bool contract) {
    static const char *const effects[]={"io","alloc","panic","block","write"};
    fputs("{\"name\":",out); json_name(p,out,f->name);
    fputs(",\"result\":",out); json_type(p,out,f->result); fputs(",\"parameters\":[",out);
    for(int i=0;i<f->param_count;++i) {
        const Param *a=&f->params[i]; if(i) fputc(',',out);
        fputs("{\"name\":",out); json_name(p,out,a->name); fputs(",\"type\":",out); json_type(p,out,a->type);
        fprintf(out,",\"noescape\":%s}",a->noescape?"true":"false");
    }
    fputs("],\"from\":[",out); bool comma=false;
    for(int i=0;i<f->param_count;++i) if(f->from_mask&(UINT32_C(1)<<i)) {
        if(comma) fputc(',',out);
        json_name(p,out,f->params[i].name); comma=true;
    }
    fputs(contract?"],\"allowed_effects\":" : "],\"effects\":",out);
    json_flags(out,contract?(EF_ALL&~vx_forbidden(f->caps)):f->effects,effects,5); fputc('}',out);
}
static void json_operations(const VxnProgram *p,FILE *out,const Constraint *c,const Implementation *impl) {
    bool comma=false; fputs(",\"operators\":[",out);
    for(int i=0;i<c->method_count;++i) {
        const Function *f=impl?p->functions[impl->methods[i]]:c->methods[i];
        if(f->operator_code<0) continue;
        if(comma) fputc(',',out);
        json_operator(p,out,f,!impl); comma=true;
    }
    fputs("],\"methods\":[",out); comma=false;
    for(int i=0;i<c->method_count;++i) {
        const Function *f=impl?p->functions[impl->methods[i]]:c->methods[i];
        if(f->operator_code>=0) continue;
        if(comma) fputc(',',out);
        json_method(p,out,f,!impl); comma=true;
    }
    fputc(']',out);
}
int vxn_emit_interface(const VxnProgram *p,FILE *out) {
    if(!vxn_ok(p)||!out) return 2;
    static const char *const caps[]={"pure","io","noalloc","nopanic","noblock","hot"};
    static const char *const effects[]={"io","alloc","panic","block","write"};
    fputs("{\n\"format\":\"vxn-interface-4\",\"package\":",out); json_string(out,p->package);
    fputs(",\"validation\":\"frontend-checked; not formally verified\",\"value_encoding\":\"prototype-scalar-cells\",\n\"packages\":[",out);
    for(int k=0;k<p->package_count;++k) {
        if(k) fputc(',',out);
        fputs("{\"name\":",out); json_string(out,p->packages[k].name);
        fputs(",\"dependencies\":[",out); bool comma=false;
        for(const Dependency *d=p->packages[k].dependencies;d;d=d->next) {
            if(comma) fputc(',',out);
            fputs("{\"alias\":",out); json_string(out,d->alias);
            fputs(",\"package\":",out); json_string(out,p->packages[d->package].name); fputc('}',out); comma=true;
        }
        fputs("]}",out);
    }
    fputs("],\n\"modules\":[",out);
    for(int m=1;m<p->module_count;++m) {
        if(m>1) fputc(',',out);
        fputs("\n{\"name\":",out); json_string(out,p->modules[m].name);
        fputs(",\"package\":",out); json_string(out,package_name(p,m)); fputs(",\"imports\":[",out);
        bool comma=false;
        for(const Import *i=p->modules[m].imports;i;i=i->next) {
            if(comma) fputc(',',out);
            fputs("{\"module\":",out); json_string(out,p->modules[i->module].name);
            fputs(",\"package\":",out); json_string(out,package_name(p,i->module));
            fputs(i->selective?",\"declaration\":":",\"alias\":",out); json_name(p,out,i->name); fputc('}',out); comma=true;
        }
        fputs("],\"types\":[",out); comma=false;
        for(int t=TY_AGGREGATE;t<p->type_count;++t) {
            const TypeDef *d=p->types[t]; if(!d->public_api || d->name.module!=m||d->template_type) continue;
            if(comma) fputc(',',out);
            fputs("{\"name\":",out); json_name(p,out,d->name);
            fprintf(out,",\"kind\":\"%s\",\"type_parameters\":[",d->kind==TD_STRUCT?"struct":"choice");
            for(int j=0;j<d->generic_count;++j) {
                const TypeDef *param=p->types[d->generic_types[j]]; if(j) fputc(',',out);
                fputs("{\"name\":",out); json_name(p,out,param->name); fputs(",\"constraints\":",out);
                json_bounds(p,out,param); json_ownership(out,param); fputc('}',out);
            }
            fputs("],\"members\":[",out);
            for(int j=0;j<d->count;++j) {
                if(j) fputc(',',out);
                fputs("{\"name\":",out);
                if(d->kind==TD_STRUCT) {
                    json_name(p,out,d->fields[j].name); fputs(",\"type\":",out); json_type(p,out,d->fields[j].type);
                } else {
                    json_name(p,out,d->variants[j].name); fputs(",\"payload\":[",out);
                    for(int k=0;k<d->variants[j].count;++k) { if(k) fputc(',',out); json_type(p,out,d->variants[j].fields[k]); }
                    fputc(']',out);
                }
                fputc('}',out);
            }
            fputs("]}",out); comma=true;
        }
        fputs("],\"functions\":[",out); comma=false;
        for(int i=p->builtin_functions;i<p->function_count;++i) {
            const Function *f=p->functions[i]; if(!f->public_api||f->name.module!=m||f->instance||f->operator_impl) continue;
            if(comma) fputc(',',out);
            fputs("{\"name\":",out); json_name(p,out,f->name);
            fprintf(out,",\"safe\":true,\"stage\":\"%s\",\"caps\":",f->comptime?"comptime":"runtime"); json_flags(out,f->caps,caps,6);
            fputs(",\"effects\":",out); json_flags(out,f->effects,effects,5);
            fprintf(out,",\"dynamic_calls\":%s",f->dynamic_calls?"true":"false");
            fputs(",\"type_parameters\":[",out);
            for(int j=0;j<f->generic_count;++j) {
                const TypeDef *d=p->types[f->generic_types[j]]; if(j) fputc(',',out);
                fputs("{\"name\":",out); json_name(p,out,d->name); fputs(",\"constraints\":",out);
                json_bounds(p,out,d);
                if(d->callable_bound) { fputs(",\"callable_bound\":",out); json_type(p,out,d->callable_bound); }
                json_ownership(out,d);
                if(f->comptime) fputs(",\"reifiable_data\":true",out);
                fputc('}',out);
            }
            fputc(']',out);
            fputs(",\"result\":",out); json_type(p,out,f->result);
            if(vx_opaque_result(p,f->result)) {
                const TypeDef *d=p->types[f->result];
                if(vx_abstract(p,f->result)) fputs(",\"opaque_result\":{\"parameterized\":true,\"cleanup_effects\":",out);
                else fprintf(out,",\"opaque_result\":{\"copyable\":%s,\"cells\":%d,\"cleanup_effects\":",vx_owned(p,f->result)?"false":"true",d->slots);
                json_flags(out,vx_cleanup_effects(p,f->result),effects,5); fputc('}',out);
            }
            fputs(",\"parameters\":[",out);
            for(int j=0;j<f->param_count;++j) {
                const Param *a=&f->params[j]; if(j) fputc(',',out);
                fputs("{\"name\":",out); json_name(p,out,a->name); fputs(",\"type\":",out); json_type(p,out,a->type);
                fprintf(out,",\"mutable_binding\":%s,\"noescape\":%s",a->mutable_binding?"true":"false",a->noescape?"true":"false");
                if(a->default_value) { fputs(",\"default\":",out); json_value(p,out,a->default_value); }
                fputc('}',out);
            }
            fputs("],\"from\":[",out); bool origin=false;
            for(int j=0;j<f->param_count;++j) if(f->from_mask&(UINT32_C(1)<<j)) {
                if(origin) fputc(',',out);
                json_name(p,out,f->params[j].name); origin=true;
            }
            fputs("]}",out); comma=true;
        }
        fputs("],\"constraints\":[",out); comma=false;
        for(int i=0;i<p->constraint_count;++i) {
            const Constraint *c=&p->constraints[i]; if(c->name.module!=m||!c->public_api) continue;
            if(comma) fputc(',',out);
            fputs("{\"name\":",out); json_name(p,out,c->name); fputs(",\"parameter\":",out); json_type(p,out,c->self);
            json_ownership(out,p->types[c->self]); json_operations(p,out,c,NULL);
            fputc('}',out); comma=true;
        }
        fputs("],\"implementations\":[",out); comma=false;
        for(int i=0;i<p->implementation_count;++i) {
            const Implementation *impl=&p->implementations[i]; if(impl->name.module!=m) continue;
            if(comma) fputc(',',out);
            fputs("{\"constraint\":",out); json_constraint(p,out,impl->constraint);
            fputs(",\"type\":",out); json_type(p,out,impl->type);
            fputs(",\"type_parameters\":[",out);
            for(int j=0;j<impl->generic_count;++j) {
                const TypeDef *d=p->types[impl->generic_types[j]]; if(j) fputc(',',out);
                fputs("{\"name\":",out); json_name(p,out,d->name); fputs(",\"constraints\":",out);
                json_bounds(p,out,d); json_ownership(out,d); fputc('}',out);
            }
            fputc(']',out);
            json_operations(p,out,&p->constraints[impl->constraint],impl);
            fputc('}',out); comma=true;
        }
        fputs("],\"constants\":[",out); comma=false;
        for(int i=0;i<p->global_count;++i) {
            const Global *g=&p->globals[i]; if(!g->public_api||g->name.module!=m) continue;
            if(comma) fputc(',',out);
            fputs("{\"name\":",out); json_name(p,out,g->name); fputs(",\"value\":",out); json_value(p,out,g->expr); fputc('}',out); comma=true;
        }
        fputs("]}",out);
    }
    fputs("\n]}\n",out); return ferror(out)?2:0;
}
