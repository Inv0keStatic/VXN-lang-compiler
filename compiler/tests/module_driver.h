static void write_fixture(const char *path,const char *text) {
    FILE *file=fopen(path,"wb"); expect(file!=NULL,"create module fixture");
    if(file) { expect(fputs(text,file)>=0,"write module fixture"); expect(fclose(file)==0,"close module fixture"); }
}
static void module_driver_tests(const char *driver,const char *cc,Files *f) {
    char root[256],library[256],manifest[256],summary[256],executable[256],generated[264],object[264],body[4096],buffer[32768];
    snprintf(root,sizeof root,"./%s/module entry.vxn",f->dir);
    snprintf(library,sizeof library,"./%s/module library%s",f->dir,llvm_driver?".ll":".c");
    snprintf(manifest,sizeof manifest,"./%s/project.vxnp",f->dir);
    snprintf(summary,sizeof summary,"./%s/interface.json",f->dir);
    const char *entry="import math.values as v;import math.values::{Item,count,Cell,Result};safe io void main(){let const later=v::later(_);output(force_take(later(60)));output(force_take(v::generic_later(Item{value=61})).value);output(v::same[v::OwnedCell,uint32](v::OwnedCell[uint32]{value=62}).value);mut Item item=Item{value=0};v::set(value:v::square(n:3),item:uniq mut item);output(v::get(item:ref item).*);output(v::defaulted());output(v::frozen(Item{value=48}).value);match v::boxed(46){BoxResult[DynFn[safe pure fn()->uint32]]::Ok(const b)->output(v::read_boxed(ref b)),BoxResult[DynFn[safe pure fn()->uint32]]::Err(const e)->output(999)}match v::generic_boxed(Item{value=47}){BoxResult[DynFn[safe pure fn()->Item]]::Ok(const b)->output(b().value),BoxResult[DynFn[safe pure fn()->Item]]::Err(const e)->output(999)}output(v::Read[Item]::read(ref item).*);output(v::inspect[Item](ref item).*);output(force_take(v::defer(fn()->uint32{return v::transfer(7);})));output(v::invoke(v::callback()));output(v::closed(43)());let const generic_closure=v::generic_closed(Item{value=45});output(generic_closure().value);let const closure=v::closed(44);output(v::apply(ref closure));output(v::borrowed(fn_ref[safe pure fn()->uint32](ref closure)));output(v::combined[Item](item,item));output(count);mut Item[2] items={item,item};v::fill(uniq_slice(uniq mut items));output(v::size(slice(ref items)));output(v::shared(slice(ref items))[0].value);const Cell[Item] cell=Cell[Item]{item=v::combine[Item](item,item)};output(cell.item.value);output(v::inspect(ref cell).*);const wrap uint8[2] text=b\"49\";match v::number(slice(ref text)){ParseResult[uint32]::Ok(const value)->output(value),ParseResult[uint32]::Err(const e)->output(false)}match v::text(slice(ref text)){Utf8Result::Ok(const count)->output(count),Utf8Result::Err(const e)->output(false)}match v::forward[uint32,bool](Result[uint32,bool]::Err(true)){Result[uint32,bool]::Ok(const n)->output(n),Result[uint32,bool]::Err(const e)->output(e)}match vec_new[wrap uint8](){VecResult[wrap uint8]::Ok(mut data)->{vec_push(uniq mut data,65);match v::own(data){StringResult::Ok(const s)->output(v::bytes(ref s)[0]),StringResult::Err(const e)->output(999)}},VecResult[wrap uint8]::Err(const e)->output(999)}output(str_byte_len(v::echo_text(v::invoke_text(v::greeting))));output(v::classify(v::Flag::On(true)));match v::copy_text(\"a\"){StringAlloc::Ok(mut s)->{v::append_text(uniq mut s,\"b\");output(str_equal(string_view(ref s),\"ab\"));},_->panic()}match v::tree(){BoxResult[v::Node]::Ok(const box)->{let const node=box_into_inner(box);output(node.value);},BoxResult[v::Node]::Err(const e)->output(999)}}";
    write_fixture(root,entry); write_fixture(library,module_values());
    snprintf(body,sizeof body,"# Paths are relative to the manifest, not the process cwd.\npackage demo\nmodule math.values = \"module library%s\"\nmodule app = \"module entry.vxn\"\n",llvm_driver?".ll":".c");
    write_fixture(manifest,body);
    const char *check[]={driver,"check",root,"--manifest",manifest,NULL};
    expect(vxn_process_redirect(check,f->out,f->err)==0,"check mapped project");
    const char *run[]={driver,"run",root,"--manifest",manifest,NULL};
    expect(vxn_process_redirect(run,f->out,f->err)==0,"interpret mapped project");
    read_file(f->out,buffer,sizeof buffer); expect(!strcmp(buffer,"60\n61\n62\n9\n42\n48\n46\n47\n9\n9\n7\n42\n43\n45\n44\n44\n9\n2\n2\n17\n9\n9\n49\n2\ntrue\n65\n2\n57\ntrue\n42\n"),"module interpreter output");
    const char *interface[]={driver,"interface",root,"--manifest",manifest,"-o",summary,NULL};
    expect(vxn_process_redirect(interface,f->out,f->err)==0,"emit project interface");
    read_file(summary,buffer,sizeof buffer);
    expect(strstr(buffer,"opaque(demo::math.values::generic_closed[T]) safe pure fn() -> T")!=NULL,"interface preserves generic opaque factory family");
    expect(strstr(buffer,"\"opaque_result\":{\"parameterized\":true")!=NULL,"interface marks abstract opaque layout");
    expect(strstr(buffer,"\"kind_arity\":1")!=NULL,"interface preserves constructor arity");
    expect(strstr(buffer,"opaque(demo::math.values::later)")!=NULL,"interface preserves opaque lazy result");
    expect(strstr(buffer,"$equations_")==NULL,"equation products stay private in interfaces");
    expect(strstr(buffer,"\"reifiable_data\":true")!=NULL,"interface preserves generic comptime data requirement");
    expect(strstr(buffer,"ParseResult[checked uint32]")!=NULL,"interface preserves integer parser result type");
    expect(strstr(buffer,"std.prelude::Utf8Result")!=NULL,"interface preserves UTF-8 validator result type");
    expect(strstr(buffer,"\"type\":\"ref String\"")!=NULL,"interface preserves opaque String type");
    expect(strstr(buffer,"\"type\":\"str\"")!=NULL,"interface preserves borrowed str type");
    expect(strstr(buffer,"\"from\":[\"text\"]")!=NULL,"interface preserves str origin");
    expect(strstr(buffer,"std.prelude::StringAlloc")!=NULL,"interface preserves fallible String constructor result");
    expect(strstr(buffer,"safe pure fn() -> str from()")!=NULL,"interface preserves static callable result contract");
    expect(strstr(buffer,"\"type\":\"demo::math.values::Cell[T]\",\"type_parameters\":[{")!=NULL,"interface preserves generic implementation parameters");
    expect(strstr(buffer,"Box[DynFn[safe pure fn() -> checked uint32]]")!=NULL,"interface preserves owned erased callable contract");
    expect(strstr(buffer,"FnRef[safe pure fn() -> checked uint32]")!=NULL,"interface preserves borrowed callable view contract");
    expect(strstr(buffer,"\"callable_bound\":\"safe pure fn() -> checked uint32\"")!=NULL,"interface preserves generic callable contract");
    expect(strstr(buffer,"\"default\":{\"type\":\"checked uint32\",\"cells_hex\":[\"000000000000002a\"]}")!=NULL,"interface serializes evaluated default");
    expect(strstr(buffer,"\"from\":[\"data\"]")!=NULL,"interface serializes shared slice result origins");
    expect(strstr(buffer,"\"from\":[\"item\"]")!=NULL,"interface serializes borrowed return");
    expect(strstr(buffer,"\"format\":\"vxn-interface-4\"")!=NULL,"interface uses the package-aware contract format");
    expect(strstr(buffer,"\"name\":\"item\",\"type\":\"uniq mut demo::math.values::Item\",\"mutable_binding\":false,\"noescape\":true")!=NULL,"interface serializes exclusive permission and noescape");
    expect(strstr(buffer,"\"effects\":[\"write\"]")!=NULL,"interface serializes external memory writes");
    expect(strstr(buffer,"\"name\":\"data\",\"type\":\"Slice[demo::math.values::Item]\",\"mutable_binding\":false,\"noescape\":false")!=NULL,"interface serializes shared slice escape permission");
    expect(strstr(buffer,"Vec[demo::math.values::Node]")!=NULL,"interface serializes a recursive owned field");
    expect(strstr(buffer,"\"constraints\":[\"demo::math.values::Read\",\"demo::math.values::Add\"]")!=NULL,"interface serializes every operation bound");
    expect(strstr(buffer,"\"ownership\":\"move\"")!=NULL,"interface serializes Move capability");
    expect(strstr(buffer,"\"methods\":[{\"name\":\"read\"")!=NULL,"interface serializes named operations");
    expect(strstr(buffer,"\"from\":[\"value\"]")!=NULL,"interface serializes method borrow origins");
    expect(strstr(buffer,"safe pure fn() -> checked uint32")!=NULL,"interface serializes callable contracts");
    expect(strstr(buffer,"\"dynamic_calls\":true")!=NULL,"interface distinguishes dynamic call targets");
    expect(strstr(buffer,"opaque(demo::math.values::closed) safe pure fn() -> checked uint32")!=NULL,"interface preserves opaque result identity and callable contract");
    expect(strstr(buffer,"\"opaque_result\":{\"copyable\":true,\"cells\":1,\"cleanup_effects\":[]}")!=NULL,"interface preserves opaque layout and cleanup promises");
    for(int optimize=0;optimize<2;++optimize) {
        snprintf(executable,sizeof executable,"./%s/module%d.exe",f->dir,optimize);
        const char *build[]={driver,"build",root,"--manifest",manifest,"-o",executable,"--backend",llvm_driver?"llvm":"c",llvm_driver?"--llvm":"--cc",cc,optimize?NULL:"--no-opt",NULL};
        int status=vxn_process_redirect(build,f->out,f->err); expect(status==0,"build module project");
        if(status) { read_file(f->err,buffer,sizeof buffer); fputs(buffer,stderr); }
        else {
            const char *native[]={executable,NULL}; expect(vxn_process_redirect(native,f->out,f->err)==0,"execute module project");
            read_file(f->out,buffer,sizeof buffer); expect(!strcmp(buffer,"60\n61\n62\n9\n42\n48\n46\n47\n9\n9\n7\n42\n43\n45\n44\n44\n9\n2\n2\n17\n9\n9\n49\n2\ntrue\n65\n2\n57\ntrue\n42\n"),"module native output");
        }
        remove_fixture(executable);
        snprintf(generated,sizeof generated,"%s%s",executable,llvm_driver?".ll":".c"); remove(generated);
        snprintf(object,sizeof object,"%s.obj",executable); remove(object);
        snprintf(object,sizeof object,"./%s/module%d.pdb",f->dir,optimize); remove(object);
    }
    const char *protect[]={driver,"interface",root,"--manifest",manifest,"-o",library,NULL};
    expect(vxn_process_redirect(protect,f->out,f->err)==2,"interface cannot overwrite imported source");
    read_file(library,buffer,sizeof buffer); expect(!strcmp(buffer,module_values()),"import source preserved");
    const char *protect_manifest[]={driver,"emit-llvm",root,"--manifest",manifest,"-o",manifest,NULL};
    expect(vxn_process_redirect(protect_manifest,f->out,f->err)==2,"emission cannot overwrite manifest");
    read_file(manifest,buffer,sizeof buffer); expect(!strcmp(buffer,body),"manifest preserved");
    snprintf(executable,sizeof executable,"./%s/module library",f->dir);
    const char *collision[]={driver,"build",root,"--manifest",manifest,"-o",executable,"--backend",llvm_driver?"llvm":"c",NULL};
    expect(vxn_process_redirect(collision,f->out,f->err)==2,"generated artifact cannot overwrite dependency");
    read_file(library,buffer,sizeof buffer); expect(!strcmp(buffer,module_values()),"dependency preserved after artifact collision");
    snprintf(buffer,sizeof buffer,"%s\npub safe pure void broken(){missing;}\n",module_values());
    write_fixture(library,buffer);
    expect(vxn_process_redirect(check,f->out,f->err)==1,"invalid dependency rejected");
    read_file(f->err,buffer,sizeof buffer); expect(strstr(buffer,llvm_driver?"module library.ll:2:":"module library.c:2:")!=NULL,"CLI reports dependency path and line");
    write_fixture(library,module_values());
    snprintf(body,sizeof body,"package demo\nmodule app = \"module entry.vxn\"\nmodule alias = \"./module entry.vxn\"\n");
    write_fixture(manifest,body); expect(vxn_process_redirect(check,f->out,f->err)==2,"duplicate physical source aliases rejected");
    write_fixture(manifest,"module app = \"module entry.vxn\"\n"); expect(vxn_process_redirect(check,f->out,f->err)==2,"manifest requires package identity");
    write_fixture(manifest,"package demo\nmodule app = \"missing.vxn\"\n"); expect(vxn_process_redirect(check,f->out,f->err)==2,"unmapped entry rejected");
    remove(root); remove(library); remove(manifest); remove(summary);
}
