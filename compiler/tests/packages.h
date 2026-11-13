static void package_file(const Files *f,const char *name,const char *source) {
    char path[256]; snprintf(path,sizeof path,"./%s/%s",f->dir,name);
    FILE *out=fopen(path,"wb"); expect(out!=NULL,"create package fixture");
    if(out) { fputs(source,out); expect(!fclose(out),"write package fixture"); }
}
static void package_tests(void) {
    Files f; if(!setup(&f)) return;
    static const char root[]="package app\nmodule main = \"app.vxn\"\ndependency left = \"left.vxnp\"\ndependency right = \"right.vxnp\"\n";
    static const char left[]="package left_package\nmodule api = \"left.vxn\"\ndependency base = \"base.vxnp\"\n";
    static const char right[]="package right_package\nmodule api = \"right.vxn\"\ndependency base = \"base.vxnp\"\n";
    static const char base[]="package common\nmodule values = \"base.vxn\"\n";
    static const char app[]="import left.api as a;import right.api as b;safe io void main(){output(b::read(a::make()));}";
    package_file(&f,"root.vxnp",root); package_file(&f,"left.vxnp",left);
    package_file(&f,"right.vxnp",right); package_file(&f,"base.vxnp",base);
    package_file(&f,"app.vxn",app);
    package_file(&f,"left.vxn","import base.values;pub safe pure values::Value make(){return values::Value{number=42};}");
    package_file(&f,"right.vxn","import base.values;pub safe pure uint32 read(const values::Value value){return values::Read[values::Value]::read(ref value);}");
    package_file(&f,"base.vxn","pub struct Value{uint32 number;}pub constraint Read[T]{safe pure uint32 read(const ref T value);}impl Read[Value]{safe pure uint32 read(const ref Value value){return value.number;}}");
    char manifest[256],source[256]; snprintf(manifest,sizeof manifest,"./%s/root.vxnp",f.dir); snprintf(source,sizeof source,"./%s/app.vxn",f.dir);
    VxnProgram *p=vxn_compile_file(source,manifest);
    expect(p&&vxn_ok(p),"diamond package graph compiles");
    if(p&&vxn_ok(p)) {
        expect(p->package_count==4&&p->module_count==5,"diamond dependency loaded exactly once");
        FILE *out=tmpfile(),*err=tmpfile();
        if(out&&err) {
            char result[32768]; expect(vxn_run(p,out,err)==0,"diamond package program runs");
            contents(out,result,sizeof result); expect(!strcmp(result,"42\n"),"diamond preserves type and witness identity");
            rewind(out); expect(!vxn_emit_interface(p,out),"package interface emission");
            contents(out,result,sizeof result);
            expect(strstr(result,"vxn-interface-4")&&strstr(result,"common::values::Value")&&strstr(result,"common::values::Read")&&strstr(result,"\"packages\""),"interface names package identities and dependencies");
            expect(vxn_emit_llvm_unit(p,out,0,-1)==2&&vxn_emit_llvm_unit(p,out,0,p->module_count)==2,"LLVM unit emitter rejects invalid unit indices");
            for(int unit=0;unit<p->module_count;++unit) {
                FILE *part=tmpfile(); expect(part!=NULL,"unit IR stream");
                if(part) { expect(!vxn_emit_llvm_unit(p,part,0,unit),"emit checked package unit"); fclose(part); }
            }
        } else expect(false,"package result streams");
        if(out) fclose(out);
        if(err) fclose(err);
    } else if(p) vxn_diagnostic(p,"packages",stderr);
    vxn_free(p);
    static const struct { const char *file,*replacement,*error; } bad[]={
        {"app.vxn","import base.values;safe pure void main(){}","not mapped"},
        {"root.vxnp","package app\nmodule main=\"app.vxn\"\ndependency left=\"root.vxnp\"\n","circular package"},
        {"left.vxnp","package left_package\nmodule api=\"left.vxn\"\ndependency base=\"root.vxnp\"\n","circular package"},
        {"right.vxnp","package left_package\nmodule api=\"right.vxn\"\n","different manifests"},
        {"root.vxnp","package app\nmodule main=\"app.vxn\"\ndependency left=\"left.vxnp\"\ndependency left=\"right.vxnp\"\n","duplicate dependency alias"},
        {"root.vxnp","package app\nmodule left.main=\"app.vxn\"\ndependency left=\"left.vxnp\"\n","local module namespace"},
        {"root.vxnp","package app\nmodule main=\"app.vxn\"\ndependency left.sub=\"left.vxnp\"\n","one lower_snake_case"},
        {"right.vxnp","package right_package\nmodule api=\"left.vxn\"\n","multiple module identities"},
        {"right.vxnp","package right_package\nmodule api=\"base.vxnp\"\n","manifest cannot also"},
        {"left.vxn","import base.values;impl values::Read[uint32]{safe pure uint32 read(const ref uint32 value){return value.*;}}pub safe pure values::Value make(){return values::Value{number=42};}","package owning"},
        {"left.vxn","import base.values;impl values::Read[values::Value]{safe pure uint32 read(const ref values::Value value){return value.number;}}pub safe pure values::Value make(){return values::Value{number=42};}","overlapping implementations"},
        {"base.vxn","struct Value{uint32 number;}pub constraint Read[T]{safe pure uint32 read(const ref T value);}","private"},
    };
    static const char *const names[]={"root.vxnp","left.vxnp","right.vxnp","base.vxnp","app.vxn","left.vxn","right.vxn","base.vxn"};
    const char *const originals[]={root,left,right,base,app,
        "import base.values;pub safe pure values::Value make(){return values::Value{number=42};}",
        "import base.values;pub safe pure uint32 read(const values::Value value){return values::Read[values::Value]::read(ref value);}",
        "pub struct Value{uint32 number;}pub constraint Read[T]{safe pure uint32 read(const ref T value);}impl Read[Value]{safe pure uint32 read(const ref Value value){return value.number;}}"};
    for(size_t i=0;i<sizeof bad/sizeof bad[0];++i) {
        package_file(&f,bad[i].file,bad[i].replacement);
        p=vxn_compile_file(source,manifest);
        bool ok=p&&!vxn_ok(p)&&strstr(p->error,bad[i].error); expect(ok,bad[i].error);
        if(!ok&&p) vxn_diagnostic(p,"package rejection",stderr);
        vxn_free(p);
        for(size_t j=0;j<sizeof names/sizeof names[0];++j) if(!strcmp(names[j],bad[i].file)) package_file(&f,names[j],originals[j]);
    }
    /* A type owner can implement an imported constraint, even though it does
       not own the constraint. The dependency itself remains unchanged. */
    package_file(&f,"left.vxn","import base.values;struct Local{uint32 value;}impl values::Read[Local]{safe pure uint32 read(const ref Local value){return value.value;}}pub safe pure values::Value make(){const Local local=Local{value=42};return values::Value{number=values::Read[Local]::read(ref local)};}");
    p=vxn_compile_file(source,manifest); expect(p&&vxn_ok(p),"local type owner may implement dependency constraint");
    if(p&&!vxn_ok(p)) vxn_diagnostic(p,"local type implementation",stderr);
    vxn_free(p);
    package_file(&f,"root.vxnp","package app\nmodule main=\"app.vxn\"\ndependency left=\"left.vxnp\"\ndependency right=\"right.vxnp\"\ndependency common=\"./base.vxnp\"\n");
    p=vxn_compile_file(source,manifest); expect(p&&vxn_ok(p)&&p->package_count==4,"additional alias preserves physical package identity"); vxn_free(p);
    package_file(&f,"root.vxnp",root);
    package_file(&f,"left.vxn","pub struct Value{uint32 number;}pub safe pure Value make(){return Value{number=42};}");
    package_file(&f,"right.vxn","pub struct Value{uint32 number;}pub safe pure uint32 read(const Value value){return value.number;}");
    p=vxn_compile_file(source,manifest);
    expect(p&&!vxn_ok(p)&&strstr(p->error,"expected"),"same module and type spelling in different packages remains distinct"); vxn_free(p);
    for(size_t j=0;j<sizeof names/sizeof names[0];++j) { char path[256]; snprintf(path,sizeof path,"./%s/%s",f.dir,names[j]); remove_fixture(path); }
    cleanup(&f);
}
