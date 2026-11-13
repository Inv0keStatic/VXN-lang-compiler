static const Case reference_cases[] = {
    {"shared scalar and lexical loan release",
     "safe pure wrap uint32 read(const ref wrap uint32 x noescape) { return x.*; }"
     "safe io void main() { mut wrap uint32 x = 4; { let const r = ref x; output(read(r)); } x = 5; output(x); }",
     "4\n5\n","",0},
    {"borrowed struct field and explicit origin",
     "pub struct Pair { wrap uint32 x; wrap uint32 y; }"
     "pub safe pure ref wrap uint32 first(const ref Pair p) from(p) { return ref p.x; }"
     "safe io void main() { const Pair p = Pair { x = 7, y = 9 }; let const r = first(ref p); output(r.*); }",
     "7\n","",0},
    {"borrowed fixed array and dynamic projection",
     "safe ref wrap uint32 item(const ref wrap uint32[3] a, const wrap uint32 i) { return ref a[i]; }"
     "safe io void main() { const wrap uint32[3] a = {5,6,7}; let const r = item(ref a,1); output(r.*); output((ref a)[2]); }",
     "6\n7\n","",0},
    {"from keeps both possible inputs",
     "pub safe pure ref wrap uint32 choose(const ref wrap uint32 a, const ref wrap uint32 b, const bool first) from(a,b) { if(first) { return a; } return b; }"
     "safe io void main() { const wrap uint32 a = 10; const wrap uint32 b = 20; output(choose(ref a,ref b,false).*); }",
     "20\n","",0},
    {"private summary inference and forward calls",
     "safe pure ref wrap uint32 outer(const ref wrap uint32 a, const ref wrap uint32 b) { return inner(a,b); }"
     "safe pure ref wrap uint32 inner(const ref wrap uint32 a, const ref wrap uint32 b) { return b; }"
     "safe io void main() { mut wrap uint32 a = 1; const wrap uint32 b = 2; let const r = outer(ref a,ref b); a = 9; output(r.*); }",
     "2\n","",0},
    {"recursive origin fixed point",
     "safe pure ref wrap uint32 pick(const ref wrap uint32 a, const ref wrap uint32 b, const wrap uint32 n) { if(n == 0) { return a; } return other(b,a,n-1); }"
     "safe pure ref wrap uint32 other(const ref wrap uint32 a, const ref wrap uint32 b, const wrap uint32 n) { if(n == 0) { return b; } return pick(a,b,n-1); }"
     "safe io void main() { const wrap uint32 a = 3; const wrap uint32 b = 4; output(pick(ref a,ref b,3).*); }",
     "4\n","",0},
    {"read-only pointee with mutable reference binding",
     "safe io void main() { const wrap uint32 a = 1; const wrap uint32 b = 2; mut ref wrap uint32 r = ref a; output(r.*); r = ref b; output(r.*); }",
     "1\n2\n","",0},
    {"aggregate dereference is a value copy",
     "struct Pair { wrap uint32 x; wrap uint32 y; }"
     "safe io void main() { const Pair p = Pair { x = 3, y = 4 }; let const r = ref p; let mut copy = r.*; copy.x = 9; output(r.x); output(copy.x); }",
     "3\n9\n","",0},
    {"shared reborrow of returned reference",
     "struct Pair { wrap uint32 x; wrap uint32 y; }"
     "safe pure ref Pair identity(const ref Pair p) { return p; }"
     "safe io void main() { const Pair p = Pair { x = 5, y = 6 }; let const r = ref identity(ref p).y; let const again = ref r.*; output(again.*); }",
     "6\n","",0},
    {"noescape can use a returned borrow inside its own call",
     "safe pure ref wrap uint32 identity(const ref wrap uint32 a) { return a; }"
     "safe pure wrap uint32 read(const ref wrap uint32 a noescape) { return identity(a).*; }"
     "safe io void main() { mut wrap uint32 x = 8; output(read(ref x)); x = 9; output(x); }",
     "8\n9\n","",0},
    {"one eligible parameter excludes noescape input",
     "pub safe pure ref wrap uint32 keep(const ref wrap uint32 a, const ref wrap uint32 key noescape) { return a; }"
     "safe io void main() { const wrap uint32 a = 3; mut wrap uint32 key = 1; let const r = keep(ref a,ref key); key = 2; output(r.*); }",
     "3\n","",0},
    {"loan ends before for update",
     "safe io void main() { let mut x = 0; for (;x < 3;x++) { let const r = ref x; output(r.*); } x = 7; output(x); }",
     "0\n1\n2\n7\n","",0},
    {"match returning a reference",
     "choice Flag { First, Second }"
     "safe pure ref wrap uint32 pick(const Flag flag, const ref wrap uint32 a, const ref wrap uint32 b) from(a,b) { return match flag { Flag::First -> a, Flag::Second -> b }; }"
     "safe io void main() { const wrap uint32 a = 1; const wrap uint32 b = 2; output(pick(Flag::Second,ref a,ref b).*); }",
     "2\n","",0},
    {"borrow projection preserves bounds panic",
     "safe io void main() { const wrap uint32[2] a = {1,2}; let const i = 3; let const r = ref a[i]; output(r.*); }",
     "","panic\n",1},
    {"borrow argument order",
     "safe ref wrap uint32 item(const ref wrap uint32[2] a, const wrap uint32 i) { output(i); return ref a[i]; }"
     "safe pure wrap uint32 sum(const ref wrap uint32 a, const ref wrap uint32 b) { return a.* + b.*; }"
     "safe io void main() { const wrap uint32[2] a = {10,20}; output(sum(item(ref a,0),item(ref a,1))); }",
     "0\n1\n30\n","",0},
    {"assignment reads temporary borrow before store",
     "safe pure wrap uint32 read(const ref wrap uint32 x noescape) { return x.*; }"
     "safe io void main() { mut wrap uint32 x = 3; x = read(ref x) + 1; output(x); }",
     "4\n","",0},
    {"borrowed choice and payload consumed within match arm",
     "choice Value { Some(wrap uint32) }"
     "safe pure wrap uint32 read(const ref wrap uint32 n noescape) { return n.*; }"
     "safe io void main() { const Value v = Value::Some(12); let const r = ref v; output(match r.* { Value::Some(const n) -> read(ref n) }); }",
     "12\n","",0},
};
static const Bad reference_bad[] = {
    {"choice Value { Some(wrap uint32) } safe io void main() { output((match Value::Some(1) { Value::Some(const n) -> ref n }).*); }","escapes an arm-local"},
    {"choice Value { Some(wrap uint32) } safe pure wrap uint32 read(const ref wrap uint32 n noescape) { return n.*; } safe io void main() { output(read(match Value::Some(1) { Value::Some(const n) -> ref n })); }","escapes an arm-local"},
    {"choice Value { Some(wrap uint32) } safe io void main() { match Value::Some(1) { Value::Some(const outer) -> output((match Value::Some(outer) { Value::Some(const inner) -> ref inner }).*) } }","escapes an arm-local"},
    {"choice Value { Some(wrap uint32) } safe io void main() { mut wrap uint32[2] a = {1,2}; a[(match Value::Some(0) { Value::Some(const n) -> ref n }).*] = 3; }","escapes an arm-local"},
    {"safe pure ref wrap uint32 bad() { const wrap uint32 x = 1; return ref x; }","escapes a local"},
    {"safe pure ref wrap uint32 bad(const wrap uint32 x) { return ref x; }","escapes a local"},
    {"safe pure ref wrap uint32 bad(const ref wrap uint32 a, const ref wrap uint32 b) from(a) { return b; }","not permitted by from"},
    {"safe pure ref wrap uint32 bad(const ref wrap uint32 a noescape) from(a) { return a; }","noescape parameter cannot appear"},
    {"safe pure ref wrap uint32 bad(const ref wrap uint32 a noescape) { let const alias = a; return alias; }","escapes a noescape"},
    {"pub safe pure ref wrap uint32 bad(const ref wrap uint32 a, const ref wrap uint32 b) { return a; }","requires explicit from"},
    {"safe pure ref wrap uint32 bad(const ref wrap uint32 a) from(a,a) { return a; }","duplicate from"},
    {"safe pure ref wrap uint32 bad(const ref wrap uint32 a) from(missing) { return a; }","unknown from"},
    {"safe pure ref wrap uint32 bad(const wrap uint32 a) from(a) { return ref a; }","must be a reference"},
    {"safe pure bool bad(const bool a) from(a) { return a; }","requires a shared reference"},
    {"safe pure void bad(const wrap uint32 a noescape) {}","noescape requires a reference"},
    {"safe pure void main() { let mut x = 1; let const r = ref x; x = 2; r.*; }","cannot mutate"},
    {"safe pure void main() { let mut x = 1; let const r = ref x; r.* = 2; }","cannot mutate through a shared"},
    {"struct Pair { wrap uint32 x; wrap uint32 y; } safe pure void main() { mut Pair p = Pair {x=1,y=2}; let const r = ref p.x; p.x = 3; r.*; }","cannot mutate"},
    {"struct Pair { wrap uint32 x; } safe pure void f(mut ref Pair r) { r.x = 2; }","cannot mutate through a shared"},
    {"safe pure void main() { mut ref wrap uint32 r; { const wrap uint32 x = 1; r = ref x; } r.*; }","does not outlive"},
    {"safe pure wrap uint32 make() { return 1; } safe pure void main() { let const r = ref make(); }","cannot borrow a temporary"},
    {"safe pure void main() { const wrap uint32 x = 1; let const r = ref x; let const s = ref r; }","nested references"},
    {"safe pure void main() { let const r = ref 1; }","cannot borrow a temporary"},
    {"safe pure void main() { const wrap uint32 x = 1; let const r = {ref x}; }","references inside aggregates"},
    {"safe pure void main() { const wrap uint32 x = 1; cast[wrap uint64](ref x); }","require numeric"},
    {"safe pure void main() { const wrap uint32 x = 1; narrow[ref wrap uint32](x); }","require numeric"},
    {"safe pure void main() { const wrap uint32 x = 1; let const r = ref x; r + r; }","requires scalar"},
    {"safe pure void main() { let const x = 1; x.*; }","dereference requires"},
    {"safe pure void main() { mut wrap uint32 x; let const r = ref x; }","may be uninitialized"},
    {"safe pure void main() { mut ref wrap uint32 r; r.*; }","may be uninitialized"},
    {"safe pure ref wrap uint32 identity(const ref wrap uint32 a) { return a; } safe pure ref wrap uint32 bad() { const wrap uint32 x = 1; return identity(ref x); }","escapes a local"},
    {"pub safe pure ref wrap uint32 keep(const ref wrap uint32 a, const ref wrap uint32 b) from(a,b) { return a; } safe pure void main() { mut wrap uint32 a=1; mut wrap uint32 b=2; let const r=keep(ref a,ref b); b=3; r.*; }","cannot mutate"},
    {"safe pure ref wrap uint32 a(const ref wrap uint32 x, const ref wrap uint32 y) { return b(x,y); } safe pure ref wrap uint32 b(const ref wrap uint32 x, const ref wrap uint32 y) { return y; } safe pure void main() { mut wrap uint32 x=1; mut wrap uint32 y=2; let const r=a(ref x,ref y); y=3; r.*; }","cannot mutate"},
    {"safe pure void main() { mut wrap uint32 a=1; mut wrap uint32 b=2; mut ref wrap uint32 r=ref a; for(mut wrap uint32 i=0;i<2;i++) { b=3; r.*; r=ref b; } }","cannot mutate"},
    {"safe pure void main() { mut wrap uint32 a=1; mut wrap uint32 b=2; mut ref wrap uint32 r=ref a; if(true) { r=ref b; } b=3; r.*; }","cannot mutate"},
};
