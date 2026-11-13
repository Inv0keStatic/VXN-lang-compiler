#ifndef VXN_INTERNAL_H
#define VXN_INTERNAL_H
#include "vxn.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include "../runtime/scalar.h"
#include "../runtime/extended.h"
#include "../runtime/io_api.h"
#include "../runtime/heap_api.h"
#include "../../stdlib/runtime/library_api.h"
#define VX_MAX_SOURCE (1024u * 1024u)
#define VX_MAX_FUNCTIONS 256
#define VX_MAX_PARAMS 32
#define VX_MAX_LOCALS 1024
#define VX_MAX_DEPTH 96
#define VX_MAX_NODES 32768
#define VX_MAX_MEMORY (64u * 1024u * 1024u)
#define VX_MAX_TYPES 512
#define VX_MAX_FIELDS 32
#define VX_MAX_SLOTS 4096
#define VX_MAX_MODULES 64
#define VX_MAX_GENERICS 8
#define VX_MAX_CONSTRAINTS 64
#define VX_MAX_IMPLS 128
typedef enum { TK_EOF = 256, TK_WORD, TK_NUMBER, TK_REAL, TK_EQ, TK_NE, TK_LE,
    TK_GE, TK_AND, TK_OR, TK_SHL, TK_SHR, TK_INC, TK_DEC, TK_ADD_EQ, TK_SUB_EQ,
    TK_MUL_EQ, TK_DIV_EQ, TK_MOD_EQ, TK_AND_EQ, TK_OR_EQ, TK_XOR_EQ, TK_SHL_EQ,
    TK_SHR_EQ, TK_ARROW, TK_SCOPE, TK_PIPE, TK_BYTES, TK_DEREF, TK_TEXT, TK_RANGE } TokenKind;
/* Keep literal spelling until contextual conversion. Compact tokens bound
   recursive parser/checker stack use. */
typedef struct { int kind; size_t start, length; uint64_t number; const char *synthetic; int module, qualifier; } Token;
typedef int Type;
enum { TY_INFER, TY_BOOL, TY_VOID, TY_F32, TY_F64, TY_FILE, TY_CANVAS, TY_WINDOW, TY_INT_BASE = 8,
    /* Separate representation words from source literal defaults. */
    TY_WORD = TY_INT_BASE + 2,
    TY_DEFAULT_UINT = TY_INT_BASE + 8 + 2, TY_DEFAULT_SINT = TY_INT_BASE + 8 + 6,
    TY_INT_END = 32, TY_F80 = 32, TY_F128 = 33, TY_AGGREGATE = 34 };
enum { EF_IO = 1, EF_ALLOC = 2, EF_PANIC = 4, EF_BLOCK = 8, EF_ABORT = 15, EF_WRITE = 16, EF_ALL = 31 };
enum { CP_PURE = 1, CP_IO = 2, CP_NOALLOC = 4, CP_NOPANIC = 8, CP_NOBLOCK = 16, CP_HOT = 32 };
typedef struct Expr Expr;
typedef struct Arg Arg;
typedef struct Stmt Stmt;
typedef struct Arm Arm;
typedef struct Pattern Pattern;
typedef struct Function Function;
struct Arg { Expr *expr; Token name; int field; Arg *next; };
typedef enum { EX_WORD, EX_REAL, EX_BOOL, EX_VAR, EX_UNARY, EX_BINARY, EX_CALL,
    EX_OUTPUT, EX_PANIC, EX_CAST, EX_RECORD, EX_ARRAY, EX_FIELD, EX_INDEX,
    EX_CHOICE, EX_MATCH, EX_LENGTH, EX_BYTES, EX_TEXT, EX_IO, EX_PARSE_NUMBER, EX_UTF8, EX_STRING_FROM, EX_STRING_INTO, EX_REF, EX_DEREF, EX_LIBRARY, EX_STATIC,
    EX_SLICE, EX_SLICE_BORROW, EX_SLICE_LENGTH, EX_GENERIC_CALL, EX_OPERATOR, EX_METHOD, EX_TRY, EX_BOX_NEW, EX_BOX_REF, EX_BOX_TAKE, EX_VEC, EX_STRING,
    EX_FUNCTION, EX_INVOKE, EX_FN_COERCE, EX_CLOSURE, EX_CLOSURE_CALL, EX_FN_VIEW, EX_VIEW_CALL, EX_VIEW_BORROW, EX_BOX_FN, EX_BOX_CALL, EX_SLICE_SPLIT, EX_MEMORY, EX_HOLE } ExprKind;
struct Expr {
    ExprKind kind; Token token, member; Type type, operand_type, call_contract;
    uint64_t value; double real; int id, op, argc;
    Expr *left, *right; Arg *args; Arm *arms;
    unsigned char *bytes;
    uint64_t *cells;
    Type type_args[VX_MAX_GENERICS]; int type_arg_count;
    bool move, bounds_proven, arithmetic_proven;
    int loan_id;
};
typedef enum { ST_BIND, ST_ASSIGN, ST_EXPR, ST_RETURN, ST_IF, ST_BLOCK,
    ST_LOOP, ST_BREAK, ST_CONTINUE, ST_MATCH } StmtKind;
struct Stmt {
    StmtKind kind; Token token, name; Type type; int local, op;
    bool mutable_binding, comptime, tail, arithmetic_proven, lazy, recursive; Expr *expr, *target;
    Stmt *body, *otherwise, *next, *initial, *step; Arm *arms;
    size_t end, scope_end;
};
typedef struct { Token name; Type type; bool mutable_binding, noescape; Expr *default_value; int default_state; } Param;
struct Arm {
    Token token;
    Pattern *pattern;
    Param bindings[VX_MAX_PARAMS]; int locals[VX_MAX_PARAMS], offsets[VX_MAX_PARAMS], count;
    Expr *expr, *guard; Stmt *body; Arm *next;
    size_t end;
};
typedef enum { PAT_WILD, PAT_BIND, PAT_LITERAL, PAT_CONSTRUCTOR, PAT_STRUCT, PAT_ARRAY, PAT_TEXT, PAT_RANGE, PAT_OR } PatternKind;
struct Pattern {
    PatternKind kind; Token token, constructor, field; Type type;
    Pattern *children, *next; Expr *literal, *limit;
    int count, binding, variant, offset; uint64_t value, upper;
};
typedef struct { Token name; Type type; int offset; } Field;
typedef struct { Token name; Type fields[VX_MAX_FIELDS]; int count, slots; } Variant;
typedef enum { TD_UNRESOLVED, TD_STRUCT, TD_ARRAY, TD_CHOICE, TD_SLICE, TD_UNIQ_SLICE, TD_PARAMETER, TD_APPLICATION, TD_BOX, TD_VEC, TD_STRING, TD_STR, TD_CALLABLE, TD_CLOSURE, TD_OPAQUE, TD_FUNCTION_ITEM, TD_FN_VIEW, TD_UNIQ_FN_VIEW, TD_DYN_FN } TypeKind;
typedef struct {
    TypeKind kind; Token name; Type element;
    Field fields[VX_MAX_FIELDS]; Variant variants[VX_MAX_FIELDS];
    int count, slots, state;
    Expr *length; Type resolved;
    bool public_api, borrowed_fields, split_result, lazy, recursive_cell;
    int lazy_take, lazy_force;
    Token bound_names[VX_MAX_GENERICS]; int bounds[VX_MAX_GENERICS],bound_count;
    /* 0: unspecified, 1: structural Copy, 2: affine Move (may own storage). */
    int ownership, constructor_arity, constructor_builtin;
    Type generic_types[VX_MAX_GENERICS], type_args[VX_MAX_GENERICS];
    int generic_count, template_type, type_arg_count;
    uint64_t *owner_map; size_t owner_map_count;
    Function *signature;
    Type callable_bound;
    bool invoked, borrowed_captures, exclusive_captures, boxed_payload;
    int closure_target;
    Function *definition;
} TypeDef;
typedef struct { Token name; Type type; int local, source, offset; bool mutable_binding; } Capture;
typedef struct Use Use;
struct Use { int callee; unsigned effects; Token token; Use *next; Expr *bounds_check,*arithmetic_check; Stmt *arithmetic_assignment; bool comptime,cleanup,indirect,guard; };
typedef struct {
    Type type, values[VX_MAX_FIELDS]; Token storage, names[VX_MAX_FIELDS];
    int count, helpers[VX_MAX_FIELDS];
} RecursiveGroup;
struct Function {
    Token name, cap_token; bool public_api; unsigned caps, effects;
    Type result; Param params[VX_MAX_PARAMS]; int param_count, local_count, local_slots, stack_cost;
    int offsets[VX_MAX_LOCALS]; Type local_types[VX_MAX_LOCALS];
    Token local_names[VX_MAX_LOCALS], from_names[VX_MAX_PARAMS];
    int local_scopes[VX_MAX_LOCALS], from_count;
    size_t local_starts[VX_MAX_LOCALS], local_ends[VX_MAX_LOCALS], end;
    uint32_t from_mask;
    bool from_explicit, from_inferred, uses_references;
    void *borrow_state;
    bool comptime; int check_state, stage_state;
    Stmt *body; Use *uses, *last_use;
    Type generic_types[VX_MAX_GENERICS], instance_types[VX_MAX_GENERICS];
    int generic_count, instance_of;
    bool instance, operator_impl;
    int operator_code;
    bool anonymous, dynamic_calls, tail_member, lazy_helper;
    Capture captures[VX_MAX_FIELDS]; int capture_count, call_mode;
    Type closure_type, item_type;
    void *capture_context; RecursiveGroup *recursive_group;
};

typedef struct {
    Token name; Type self; bool public_api;
    Function *methods[VX_MAX_FIELDS]; int method_count;
} Constraint;
typedef struct {
    Token name; Type type; int constraint;
    int methods[VX_MAX_FIELDS], method_count;
    Type generic_types[VX_MAX_GENERICS]; int generic_count;
} Implementation;
typedef struct { Token name; Type type; Expr *expr; Function *context; int state; bool public_api; } Global;
typedef struct Import Import;
struct Import { Token name; int module; bool selective; Import *next; };
typedef struct {
    const char *name, *path; size_t start, length, token_count, body_start;
    Token *tokens; Import *imports; int state, package;
} Module;
typedef struct Dependency Dependency;
struct Dependency { const char *alias; int package; Dependency *next; };
typedef struct {
    const char *name, *manifest; Dependency *dependencies; int state;
} Package;
typedef struct Allocation Allocation;
struct Allocation { void *memory; Allocation *next; };
typedef struct TextLiteral TextLiteral;
struct TextLiteral { uint64_t *cells; int count, id; TextLiteral *next; };
enum {
    LT_COLOR, LT_FRAME, LT_GFX_RESULT, LT_CANVAS_OPEN, LT_WINDOW_OPEN,
    LT_FRAME_READ, LT_PIXEL_READ, LT_TIME_READ, LT_TIME_RESULT, LT_WINDOW_MODE, LT_COUNT,
    LA_U32, LA_I32, LA_CANVAS, LA_WINDOW, LA_BYTES
};
typedef struct {
    const char *name; int count,arguments[6],result; unsigned effects;
} LibraryCall;
struct VxnProgram {
    char *source; Token *tokens; size_t token_count, at, nodes, memory;
    int depth, function_count, main_id, type_count, tail_slots, tail_environment_slots;
    Type io_open, io_read, io_read_buffer, io_result;
    Type library_types[LT_COUNT]; int builtin_functions;
    Function *functions[VX_MAX_FUNCTIONS]; TypeDef *types[VX_MAX_TYPES];
    Global globals[VX_MAX_FUNCTIONS]; int global_count, check_depth, stage_depth;
    unsigned stage_steps;
    Module modules[VX_MAX_MODULES+1]; int module_count, current_module, import_count;
    const char *package, *manifest; bool input_error;
    Package packages[VX_MAX_MODULES]; int package_count, dependency_count;
    Constraint constraints[VX_MAX_CONSTRAINTS]; int constraint_count;
    Implementation implementations[VX_MAX_IMPLS]; int implementation_count;
    unsigned char implementation_cache[VX_MAX_TYPES][VX_MAX_CONSTRAINTS];
    int implementation_depth;
    Type parse_types[VX_MAX_GENERICS]; int parse_type_count;
    int specialize_depth;
    bool generics_ready, resolving_types;
    Type box_result,vec_result,vec_pop,alloc_result,parse_result,utf8_result,utf8_read,string_type,string_result,str_type,string_alloc,string_edit;
    Type text_search,text_order,string_index,vec_insert,io_line,text_slice,str_result,split_result,lazy_type,lazy_error,lazy_result,recursive_cell;
    TextLiteral *text_literals; int text_count;
    void *pattern_workspace;
    Allocation *allocations; jmp_buf failure;
    char error[1024]; size_t error_pos, error_length;
};
void *vx_alloc(VxnProgram *p, size_t bytes);
_Noreturn void vx_fail(VxnProgram *p, Token at, const char *format, ...);
bool vx_word(const VxnProgram *p, Token t, const char *word);
bool vx_same(const VxnProgram *p, Token a, Token b);
Token vx_synthetic(Token at,const char *name);
Type vx_builtin_constructor(VxnProgram *p,TypeKind kind,Token at);
int vx_lazy_helper(VxnProgram *p,Type type,bool borrow,const Function *context,Token at);
const char *vx_type(Type type);
void vx_describe_type(const VxnProgram *p,Type type,char *out,size_t capacity);
void vx_name(const VxnProgram *p, Token t, char *out, size_t cap);
void vx_compile_sources(VxnProgram *p,const VxnSource *sources,size_t count);
int vx_symbol_module(const VxnProgram *p,Token name);
bool vx_visible(VxnProgram *p,Token use,Token declaration,bool public_api);
bool vx_module_name(const char *name);
bool vx_same_file(const char *a,const char *b);
void vx_check_interfaces(VxnProgram *p);
void vx_check(VxnProgram *p);
void vx_prepare_tails(VxnProgram *p);
int vx_tail_environment_mode(const VxnProgram *p,const Function *f,const Expr *e);
void vx_prove_ranges(VxnProgram *p);
int vx_parameter_slots(const VxnProgram *p,const Function *f);
void vx_check_patterns(VxnProgram *p,Type t,const Arm *arms,Token at);
void vx_pattern_layout(VxnProgram *p,Arm *arm,Pattern *pattern,int offset);
void vx_check_borrows(VxnProgram *p);
void vx_check_exclusive(VxnProgram *p,Function *f);
void vx_check_ownership(VxnProgram *p);
void vx_check_function_ownership(VxnProgram *p,Function *f);
bool vx_owned(const VxnProgram *p,Type t);
unsigned vx_cleanup_effects(const VxnProgram *p,Type t);
void vx_borrow_signature(VxnProgram *p,Function *f);
const Function *vx_call_contract(const VxnProgram *p,const Expr *e);
Function *vx_method_contract(VxnProgram *p,Expr *e);
int vx_method_implementation(VxnProgram *p,int constraint,Type t,int method,Token at);
Type vx_box_type(VxnProgram *p,Type t,Token at);
Type vx_vec_type(VxnProgram *p,Type t,Token at);
int vx_vec_builtin(const VxnProgram *p,Token name);
int vx_string_builtin(const VxnProgram *p,Token name);
Type vx_string_type(VxnProgram *p,Token at);
Type vx_str_type(VxnProgram *p,Token at);
Type vx_resolve_type(VxnProgram *p,Type t);
void vx_check_function(VxnProgram *p,Function *f);
void vx_frame_layout(VxnProgram *p,Function *f);
unsigned vx_forbidden(unsigned caps);
Type vx_parameter_type(VxnProgram *p,Token name,Token bound);
bool vx_abstract(const VxnProgram *p,Type t);
Type vx_substitute_type(VxnProgram *p,Type t,const Type *from,const Type *to,int count,Token at);
int vx_constraint_id(VxnProgram *p,Token name);
void vx_prepare_generics(VxnProgram *p);
void vx_check_implementations(VxnProgram *p);
void vx_check_type_arguments(VxnProgram *p,const Function *f,const Type *types,int count,Token at);
void vx_check_callable_arguments(VxnProgram *p,const Function *f,const Type *types,Token at,bool complete);
void vx_infer_type_arguments(VxnProgram *p,const Function *f,Type pattern,Type actual,Type *types,Token at);
Type vx_argument_hint(VxnProgram *p,const Function *f,Type pattern,const Type *types,Token at);
Function *vx_operator_contract(VxnProgram *p,Type t,int op,Token at,int *method,int *constraint);
int vx_operator_implementation(VxnProgram *p,Type t,int op,int constraint,Token at);
void vx_specialize(VxnProgram *p);
void vx_specialize_call(VxnProgram *p,Expr *e);
Type vx_callable_type(VxnProgram *p,const Function *signature,Token at);
Type vx_fn_view_type(VxnProgram *p,Type contract,bool exclusive,Token at);
void vx_check_view_contract(VxnProgram *p,Type source,Type view,Token at,bool complete);
Type vx_closure_type(VxnProgram *p,int id,Token at);
Type vx_function_item_type(VxnProgram *p,int id,Token at);
bool vx_callable_shape(const VxnProgram *p,const Function *source,const Function *target);
bool vx_callable_target(const VxnProgram *p,const Function *source,const Function *target);
bool vx_callable_contract(const VxnProgram *p,const Function *source,const Function *target);
void vx_check_callables(VxnProgram *p);
Type vx_apply_type(VxnProgram *p,Type base,const Type *args,int count,Token at);
void vx_expand_type(VxnProgram *p,Type t);
void vx_check_type_application(VxnProgram *p,Type t);
void vx_check_global(VxnProgram *p,int id);
uint64_t vx_check_length(VxnProgram *p,Expr *e);
void vx_stage_value(VxnProgram *p,Function *f,Expr *e);
void vx_check_stages(VxnProgram *p);
bool vx_stage_type(const VxnProgram *p,Type t);
bool vx_stage_template_type(const VxnProgram *p,Type t);
int vx_eval_static(VxnProgram *p,Function *f,const Expr *e,uint64_t *out);
static inline bool vx_ref(Type t) { return t < 0; }
static inline bool vx_uniq(Type t) { return t < -VX_MAX_TYPES; }
static inline bool vx_writable(Type t) { return t < -VX_MAX_TYPES && t >= -2*VX_MAX_TYPES; }
static inline Type vx_referent(Type t) { return (-t-1)%VX_MAX_TYPES; }
static inline Type vx_retarget(Type reference,Type element) { return -element-1-((-reference-1)/VX_MAX_TYPES)*VX_MAX_TYPES; }
Type vx_reference_type(VxnProgram *p, Type t, Token at);
Type vx_exclusive_type(VxnProgram *p,Type t,bool writable,Token at);
unsigned vx_builtin_effects(ExprKind kind);
bool vx_constant(const Expr *e, Type *type, uint64_t *value);
int vx_slots(const VxnProgram *p, Type type);
Type vx_array_type(VxnProgram *p, Type element, int count, Token at);
Type vx_slice_type(VxnProgram *p,Type element,Token at);
Type vx_uniq_slice_type(VxnProgram *p,Type element,Token at);
static inline bool vx_extended(Type t) { return t==TY_F80||t==TY_F128; }
static inline bool vx_indirect(Type t) { return vx_extended(t)||t>=TY_AGGREGATE; }
static inline bool vx_uniq_slice(const VxnProgram *p,Type t) { return t>=TY_AGGREGATE && p->types[t]->kind==TD_UNIQ_SLICE; }
static inline bool vx_slice(const VxnProgram *p,Type t) { return t>=TY_AGGREGATE && (p->types[t]->kind==TD_SLICE||p->types[t]->kind==TD_UNIQ_SLICE); }
static inline bool vx_shared_slice(const VxnProgram *p,Type t) { return t>=TY_AGGREGATE && p->types[t]->kind==TD_SLICE; }
static inline bool vx_str(const VxnProgram *p,Type t) { return t>=TY_AGGREGATE && p->types[t]->kind==TD_STR; }
static inline bool vx_shared_aggregate(const VxnProgram *p,Type t) { return t>=TY_AGGREGATE&&p->types[t]->borrowed_fields; }
static inline bool vx_shared_view(const VxnProgram *p,Type t) { return vx_shared_slice(p,t)||vx_str(p,t); }
static inline bool vx_shared_result(const VxnProgram *p,Type t) { return (vx_ref(t)&&!vx_uniq(t))||vx_shared_view(p,t)||vx_shared_aggregate(p,t); }
static inline bool vx_split_result(const VxnProgram *p,Type t) { return t>=TY_AGGREGATE&&p->types[t]->split_result; }
static inline bool vx_returned_borrow(const VxnProgram *p,Type t) { return vx_shared_result(p,t)||vx_uniq(t)||vx_uniq_slice(p,t)||vx_split_result(p,t); }
void vx_borrow_parameter(VxnProgram *p,Function *f,Param *a);
static inline bool vx_uniq_fn_view(const VxnProgram *p,Type t) { return t>=TY_AGGREGATE&&p->types[t]->kind==TD_UNIQ_FN_VIEW; }
static inline bool vx_fn_view(const VxnProgram *p,Type t) { return t>=TY_AGGREGATE&&(p->types[t]->kind==TD_FN_VIEW||p->types[t]->kind==TD_UNIQ_FN_VIEW); }
static inline bool vx_exclusive(const VxnProgram *p,Type t) { return vx_uniq(t)||vx_uniq_slice(p,t)||vx_uniq_fn_view(p,t)||vx_split_result(p,t); }
static inline bool vx_vec(const VxnProgram *p,Type t) { return t>=TY_AGGREGATE&&p->types[t]->kind==TD_VEC; }
static inline bool vx_callable(const VxnProgram *p,Type t) { return t>=TY_AGGREGATE&&p->types[t]->kind==TD_CALLABLE; }
static inline int vx_kind_arity(const VxnProgram *p,Type t) {
    if(t<TY_AGGREGATE) return 0;
    const TypeDef *d=p->types[t];
    return d->constructor_arity?d->constructor_arity:
        ((d->kind==TD_STRUCT||d->kind==TD_CHOICE)&&!d->template_type?d->generic_count:0);
}
static inline bool vx_opaque(const VxnProgram *p,Type t) { return t>=TY_AGGREGATE&&p->types[t]->kind==TD_OPAQUE; }
static inline Type vx_opaque_result(const VxnProgram *p,Type t) {
    if(vx_opaque(p,t)) return t;
    if(t>=TY_AGGREGATE) {
        const TypeDef *d=p->types[t];
        if(d->template_type==p->lazy_type+1&&d->type_arg_count==2&&vx_opaque(p,d->type_args[1])) return d->type_args[1];
    }
    return TY_INFER;
}
static inline bool vx_closure(const VxnProgram *p,Type t) { return t>=TY_AGGREGATE&&(p->types[t]->kind==TD_CLOSURE||p->types[t]->kind==TD_OPAQUE); }
static inline bool vx_function_item(const VxnProgram *p,Type t) { return t>=TY_AGGREGATE&&p->types[t]->kind==TD_FUNCTION_ITEM; }
static inline bool vx_dyn_fn(const VxnProgram *p,Type t) { return t>=TY_AGGREGATE&&p->types[t]->kind==TD_DYN_FN; }
static inline bool vx_box_fn(const VxnProgram *p,Type t) { return t>=TY_AGGREGATE&&p->types[t]->kind==TD_BOX&&vx_dyn_fn(p,p->types[t]->element); }
Type vx_dyn_fn_type(VxnProgram *p,Type contract,Token at);
Type vx_box_fn_payload(VxnProgram *p,Type source,Token at);
static inline bool vx_invocable(const VxnProgram *p,Type t) { return vx_callable(p,t)||vx_closure(p,t)||vx_function_item(p,t)||vx_fn_view(p,t)||vx_box_fn(p,t)||(t>=TY_AGGREGATE&&p->types[t]->kind==TD_PARAMETER&&p->types[t]->callable_bound); }
static inline int vx_capture(const Function *f,int local) {
    for(int i=0;i<f->capture_count;++i) if(f->captures[i].local==local) return i;
    return -1;
}
static inline bool vx_heap_type(const VxnProgram *p,Type t) { return t>=TY_AGGREGATE&&(p->types[t]->kind==TD_BOX||p->types[t]->kind==TD_VEC||p->types[t]->kind==TD_STRING); }
static inline bool vx_borrowed(const VxnProgram *p,Type t) { return vx_ref(t)||vx_slice(p,t)||vx_str(p,t)||vx_fn_view(p,t); }
static inline bool vx_borrowed_closure(const VxnProgram *p,Type t) { return vx_closure(p,t)&&p->types[t]->borrowed_captures; }
static inline bool vx_exclusive_captures(const VxnProgram *p,Type t) { return vx_closure(p,t)&&p->types[t]->exclusive_captures; }
/* Dependent values carry loans, including through aggregates. */
static inline bool vx_dependent(const VxnProgram *p,Type t) { return vx_borrowed(p,t)||vx_borrowed_closure(p,t)||vx_shared_aggregate(p,t)||vx_split_result(p,t); }
static inline bool vx_stable_place(const VxnProgram *p,const Expr *e) {
    if(e->kind==EX_VAR||e->kind==EX_DEREF) return true;
    if(e->kind==EX_INDEX&&vx_slice(p,e->left->type)) return true;
    return (e->kind==EX_FIELD||e->kind==EX_INDEX)&&vx_stable_place(p,e->left);
}
int vx_integer(Type type);
int vx_float(Type type);
unsigned vx_width(Type type);
bool vx_signed_type(Type type);
unsigned vx_mode(Type type);
const char *vx_op_name(int op);
int vx_io_builtin(const VxnProgram *p, Token name);
Type vx_io_result(const VxnProgram *p, int op);
extern const char *const vx_library_type_names[LT_COUNT];
extern const LibraryCall vx_library_calls[VX_LIBRARY_COUNT];
extern const char *const vx_library_parameter_names[VX_LIBRARY_COUNT][6];
const Arg *vx_argument(const Expr *e,int parameter);
int vx_argument_offset(const VxnProgram *p,const Expr *e,int parameter);
int vx_library_builtin(const VxnProgram *p,Token name);
Type vx_library_type(const VxnProgram *p,int type);
#endif
