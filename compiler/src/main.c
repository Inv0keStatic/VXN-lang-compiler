#include "internal.h"
#include <stdlib.h>
#include <string.h>
#include "vxn_native_config.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#if defined(__linux__)
#include <unistd.h>
#endif
#endif
#include "driver_paths.h"

static void usage(FILE *out) {
    fputs("vxnc " VXN_VERSION " - Vxn v1 compiler (C11)\n"
        "  vxnc check file.vxn\n"
        "  vxnc run file.vxn\n"
        "  vxnc emit-c file.vxn [-o file.c] [--no-opt]\n"
        "  vxnc emit-llvm file.vxn [-o file.ll] [--no-opt]\n"
        "  vxnc interface file.vxn [-o interface.json]\n"
        "  vxnc build file.vxn -o executable [--llvm clang] [--cc linker-driver]\n"
        "             [--runtime library] [--backend llvm|c] [--no-opt]\n"
        "All commands accept --manifest project.vxnp for explicit module mappings.\n"
        "Native builds emit LLVM IR directly and retain executable.ll and .obj.\n"
        "--cc links the LLVM object; --backend c explicitly selects C emission.\n"
        "See README.md for compilation instructions and LANGUAGE.txt for examples.\n",out);
}
static bool input_alias(const VxnProgram *p,const char *path) {
    if(vx_same_file(p->manifest,path)) return true;
    for(int i=0;i<p->package_count;++i) if(vx_same_file(p->packages[i].manifest,path)) return true;
    for(int i=1;i<p->module_count;++i) if(vx_same_file(p->modules[i].path,path)) return true;
    return false;
}
static int emit(const VxnProgram *p, const char *path, int optimize, int kind) {
    FILE *out = path ? fopen(path,"w") : stdout;
    if (!out) { perror(path); return 2; }
    int status = kind==2 ? vxn_emit_interface(p,out) : kind==1 ? vxn_emit_llvm(p,out,optimize) : vxn_emit_c(p,out,optimize);
    if (path) { if (fclose(out)) status = 2; } else if (fflush(out)) status = 2;
    if (status) fputs("failed to write generated source\n",stderr);
    return status;
}
static const char *setting(const char *arg,const char *env,const char *fallback) {
    if(arg) return arg;
    const char *value=getenv(env);
    return value&&*value?value:fallback;
}
static char *suffix(const char *path,const char *ending) {
    size_t n=strlen(path),m=strlen(ending);
    char *result=malloc(n+m+1);
    if(result) { memcpy(result,path,n); memcpy(result+n,ending,m+1); }
    return result;
}
static int native_build_modules(const VxnProgram *p,const char *output,const char *cc,
                                const char *llvm,const char *runtime,int optimize) {
    char *generated[VX_MAX_MODULES+1]={0},*objects[VX_MAX_MODULES+1]={0};
    const char *paths[2*(VX_MAX_MODULES+1)+2]={output,runtime}; int count=2,status=0;
    /* Reject all output aliases before opening files. */
    for(int unit=0;unit<p->module_count;++unit) {
        char ending[32]; snprintf(ending,sizeof ending,".unit%d.ll",unit); generated[unit]=suffix(output,ending);
        snprintf(ending,sizeof ending,".unit%d.obj",unit); objects[unit]=suffix(output,ending);
        if(!generated[unit]||!objects[unit]) { status=2; break; }
        paths[count++]=generated[unit]; paths[count++]=objects[unit];
    }
    if(status) fputs("out of memory\n",stderr);
    else {
        for(int i=0;i<count;++i) {
            if(input_alias(p,paths[i])) status=2;
            for(int j=0;j<i;++j) if(vx_same_file(paths[i],paths[j])) status=2;
        }
        if(status) fputs("refusing to overwrite source, executable, runtime, or an aliased build artifact\n",stderr);
    }
    for(int unit=0;unit<p->module_count&&!status;++unit) {
        FILE *out=fopen(generated[unit],"w");
        if(!out) { perror(generated[unit]); status=2; break; }
        status=vxn_emit_llvm_unit(p,out,optimize,unit); if(fclose(out)) status=2;
        if(status) { fputs("failed to write LLVM module\n",stderr); break; }
        const char *compile[]={llvm,"-target",VXN_LLVM_TARGET,"-x","ir","-Wno-override-module",
            optimize?"-O2":"-O0","-c",generated[unit],"-o",objects[unit],NULL};
        int child=vxn_process(compile);
        if(child) { fprintf(stderr,"LLVM module compilation failed (%d); IR retained at %s\n",child,generated[unit]); status=2; }
    }
    if(!status) {
        const char *link[VX_MAX_MODULES+sizeof vxn_link_options/sizeof vxn_link_options[0]+sizeof vxn_platform_links/sizeof vxn_platform_links[0]+6]; size_t n=0;
        link[n++]=cc;
        for(size_t i=0;vxn_link_options[i];++i) link[n++]=vxn_link_options[i];
        for(int unit=0;unit<p->module_count;++unit) link[n++]=objects[unit];
        link[n++]=runtime;
        for(size_t i=0;vxn_platform_links[i];++i) link[n++]=vxn_platform_links[i];
        link[n++]="-o"; link[n++]=output; link[n]=NULL;
        int child=vxn_process(link);
        if(child) { fprintf(stderr,"native module link failed (%d); unit objects retained\n",child); status=2; }
    }
    for(int unit=0;unit<p->module_count;++unit) { free(generated[unit]); free(objects[unit]); }
    return status;
}
static int native_build(const VxnProgram *p,const char *source,const char *output,
                        const char *cc,const char *llvm,const char *runtime,const char *extended,int optimize,bool use_llvm) {
    if(use_llvm&&p->package_count>1) return native_build_modules(p,output,cc,llvm,runtime,optimize);
    char *generated=suffix(output,use_llvm?".ll":".c"),*object=suffix(output,".obj");
    if(!generated||!object) { free(generated); free(object); fputs("out of memory\n",stderr); return 2; }
    const char *paths[]={source,output,generated,use_llvm?object:extended,use_llvm?runtime:NULL};
    int count=use_llvm?5:4,status=0;
    for(int i=0;i<count;++i) for(int j=i+1;j<count;++j)
        if(vx_same_file(paths[i],paths[j])) status=2;
    for(int i=1;i<count;++i) if(input_alias(p,paths[i])) status=2;
    if(status) fputs("refusing to overwrite source, executable, runtime, or an aliased build artifact\n",stderr);
    else if(!(status=emit(p,generated,optimize,use_llvm))) {
        int child;
        if(use_llvm) {
            const char *compile[]={llvm,"-target",VXN_LLVM_TARGET,"-x","ir",
                "-Wno-override-module",optimize?"-O2":"-O0","-c",generated,"-o",object,NULL};
            child=vxn_process(compile);
            if(child) { fprintf(stderr,"LLVM compilation failed (%d); IR retained at %s\n",child,generated); status=2; }
            else {
                const char *link[sizeof vxn_link_options/sizeof vxn_link_options[0]+sizeof vxn_platform_links/sizeof vxn_platform_links[0]+5]; size_t n=0;
                link[n++]=cc;
                for(size_t i=0;vxn_link_options[i];++i) link[n++]=vxn_link_options[i];
                link[n++]=object; link[n++]=runtime;
                for(size_t i=0;vxn_platform_links[i];++i) link[n++]=vxn_platform_links[i];
                link[n++]="-o"; link[n++]=output; link[n]=NULL;
                child=vxn_process(link);
                if(child) { fprintf(stderr,"native link failed (%d); object retained at %s\n",child,object); status=2; }
            }
        } else {
            const char *compile[sizeof vxn_platform_links/sizeof vxn_platform_links[0]+11];
            size_t n=0;
            compile[n++]=cc; compile[n++]="-std=c11"; compile[n++]=optimize?"-O2":"-O0";
            compile[n++]="-Wall"; compile[n++]="-Wextra"; compile[n++]="-Wpedantic"; compile[n++]=generated; compile[n++]=extended;
            for(size_t i=0;vxn_platform_links[i];++i) compile[n++]=vxn_platform_links[i];
            compile[n++]="-o"; compile[n++]=output; compile[n]=NULL;
            child=vxn_process(compile);
            if(child) { fprintf(stderr,"C compiler failed (%d); C retained at %s\n",child,generated); status=2; }
        }
    }
    free(generated); free(object); return status;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1],"--version")) { puts("vxnc " VXN_VERSION); return 0; }
    if (argc == 2 && !strcmp(argv[1],"--help")) { usage(stdout); return 0; }
    if (argc < 3) { usage(stderr); return 2; }
    bool check = !strcmp(argv[1],"check"), run = !strcmp(argv[1],"run"),
        emit_c = !strcmp(argv[1],"emit-c"), emit_llvm = !strcmp(argv[1],"emit-llvm"), build = !strcmp(argv[1],"build"), interface = !strcmp(argv[1],"interface");
    if (!check && !run && !emit_c && !emit_llvm && !build && !interface) { usage(stderr); return 2; }
    const char *path = argv[2], *output = NULL, *cc = NULL, *llvm = NULL, *runtime = NULL, *backend = NULL, *manifest = NULL;
    int optimize = 1;
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i],"-o") && !output && i + 1 < argc && (emit_c || emit_llvm || build || interface)) output = argv[++i];
        else if (!strcmp(argv[i],"--manifest") && !manifest && i + 1 < argc) manifest = argv[++i];
        else if (!strcmp(argv[i],"--cc") && !cc && i + 1 < argc && build) cc = argv[++i];
        else if (!strcmp(argv[i],"--llvm") && !llvm && i + 1 < argc && build) llvm = argv[++i];
        else if (!strcmp(argv[i],"--runtime") && !runtime && i + 1 < argc && build) runtime = argv[++i];
        else if (!strcmp(argv[i],"--backend") && !backend && i + 1 < argc && build) backend = argv[++i];
        else if (!strcmp(argv[i],"--no-opt") && (emit_c || emit_llvm || build)) optimize = 0;
        else { fprintf(stderr,"unknown or misplaced argument: %s\n",argv[i]); return 2; }
    }
    if(backend && strcmp(backend,"llvm") && strcmp(backend,"c")) { fputs("backend must be llvm or c\n",stderr); return 2; }
    if(backend && !strcmp(backend,"c") && (llvm||runtime)) { fputs("--llvm and --runtime require the LLVM backend\n",stderr); return 2; }
    if (build && !output) { fputs("build requires -o executable\n",stderr); return 2; }
    if (output && vx_same_file(path,output)) { fputs("refusing to overwrite the source file\n",stderr); return 2; }
    VxnProgram *p = vxn_compile_file(path,manifest);
    if (!vxn_ok(p)) { int status=p&&p->input_error?2:1; vxn_diagnostic(p,path,stderr); vxn_free(p); return status; }
    if(output && input_alias(p,output)) { fputs("refusing to overwrite a module source or project manifest\n",stderr); vxn_free(p); return 2; }
    int status = 0;
    if (check) vxn_report(p,stdout);
    else if (run) status = vxn_run(p,stdout,stderr);
    else if (emit_c || emit_llvm) status = emit(p,output,optimize,emit_llvm);
    else if (interface) status = emit(p,output,optimize,2);
    else if (p->main_id < 0) { fputs("native build requires a main function\n",stderr); status = 2; }
    else {
        char *runtime_path=driver_library(VXN_INSTALLED_RUNTIME,VXN_RUNTIME_PATH);
        char *extended_path=driver_library(VXN_INSTALLED_EXTENDED,VXN_EXTENDED_PATH);
        const char *selected_runtime=setting(runtime,"VXN_RUNTIME",runtime_path);
        const char *selected_extended=setting(NULL,"VXN_EXTENDED",extended_path);
        bool use_llvm=!backend||!strcmp(backend,"llvm");
        const char *archive=use_llvm?selected_runtime:selected_extended;
        if(!archive||!driver_readable(archive)) {
            fprintf(stderr,"cannot read %s archive: %s; reinstall vxn or set %s\n",
                use_llvm?"runtime":"extended",archive?archive:"(path unavailable)",
                use_llvm?"VXN_RUNTIME / --runtime":"VXN_EXTENDED");
            status=2;
        } else status=native_build(p,path,output,
            setting(cc,"VXN_CC",driver_tool(VXN_DEFAULT_CC,VXN_CC_NAME)),
            setting(llvm,"VXN_LLVM",driver_tool(VXN_DEFAULT_LLVM,VXN_LLVM_NAME)),
            selected_runtime,selected_extended,optimize,use_llvm);
        free(runtime_path); free(extended_path);
    }
    vxn_free(p); return status;
}
