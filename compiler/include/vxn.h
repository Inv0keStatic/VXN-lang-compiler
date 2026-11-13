#ifndef VXN_H
#define VXN_H

#include <stdio.h>

typedef struct VxnProgram VxnProgram;
typedef struct { const char *name, *path, *source; } VxnSource;
VxnProgram *vxn_compile(const char *source);
/* sources[0] is the entry module; names are logical dotted module paths. */
VxnProgram *vxn_compile_sources(const VxnSource *sources, size_t count);
VxnProgram *vxn_compile_file(const char *path, const char *manifest);
int vxn_emit_interface(const VxnProgram *program, FILE *out);
void vxn_free(VxnProgram *program);
int vxn_ok(const VxnProgram *program);
void vxn_diagnostic(const VxnProgram *program, const char *path, FILE *out);
void vxn_report(const VxnProgram *program, FILE *out);

int vxn_run(const VxnProgram *program, FILE *out, FILE *err);
int vxn_run_io(const VxnProgram *program, FILE *in, FILE *out, FILE *err);

int vxn_emit_c(const VxnProgram *program, FILE *out, int optimize);

// Direct checked-AST -> textual LLVM IR (opaque pointers, LLVM 15+)
int vxn_emit_llvm(const VxnProgram *program, FILE *out, int optimize);
/* Shared checked image: unit 0 holds dispatch/entry; others hold module functions.
   No independent ABI. */
int vxn_emit_llvm_unit(const VxnProgram *program, FILE *out, int optimize, int unit);

int vxn_process(const char *const argv[]);
int vxn_process_redirect(const char *const argv[], const char *out, const char *err);
int vxn_process_redirect_io(const char *const argv[], const char *in, const char *out, const char *err);

#endif
