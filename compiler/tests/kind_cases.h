static const Case kind_cases[]={
    {"constructor arguments in result hint","struct Cell[T:Move]{T value;}safe io F[T] same[F[_],T:Move](const F[T] value){return value;}safe io void main(){output(same[Cell,uint32](Cell[uint32]{value=42}).value);}","42\n","",0},
    {"constructor inference and callbacks","choice Maybe[T:Move]{None,Some(T)}safe io F[B] map[F[_],A:Move,B:Move,C:safe io fn once(const F[A])->F[B]](const F[A] value,const C callback){return callback(value);}safe io Maybe[bool] positive(const Maybe[uint32] value){return match value{Maybe[uint32]::None->Maybe[bool]::None,Maybe[uint32]::Some(const n)->Maybe[bool]::Some(n>0)};}safe io void main(){let const value=map(Maybe[uint32]::Some(42),positive);output(match value{Maybe[bool]::None->false,Maybe[bool]::Some(const b)->b});}","true\n","",0},
    {"explicit constructor argument","struct Cell[T:Move]{T value;}safe io F[T] same[F[_],T:Move](const F[T] value){return value;}safe io void main(){let const value=same[Cell,uint32](Cell[uint32]{value=42});output(value.value);}","42\n","",0},
    {"binary constructors","choice Pair[A:Move,B:Move]{Both(A,B)}safe io F[A,B] same[F[_,_],A:Move,B:Move](const F[A,B] value){return value;}safe io void main(){let const pair=same(Pair[uint32,bool]::Both(42,true));match pair{Pair[uint32,bool]::Both(const n,const b)->{output(n);output(b);}}}","42\ntrue\n","",0},
    {"constructor forwarding between templates","struct Cell[T:Move]{T value;}safe io F[T] same[F[_],T:Move](const F[T] value){return value;}safe io G[A] forward[G[_],A:Move](const G[A] value){return same(value);}safe io void main(){output(forward(Cell[uint32]{value=42}).value);}","42\n","",0},
    {"constructor ownership and builtin Box","safe io F[T] same[F[_],T:Move](const F[T] value){return value;}safe io void main(){for(mut wrap uint32 i=0;i<200;i++){match box_new[uint32](42){BoxResult[uint32]::Ok(const b)->{const Box[uint32] result=same(b);if(box_into_inner(result)!=42){panic();}},_->panic()}}output(42);}","42\n","",0},
    {"explicit builtin Vec constructor","safe io F[T] same[F[_],T:Move](const F[T] value){return value;}safe io void main(){match vec_new[uint32](){VecResult[uint32]::Ok(mut v)->{vec_push(uniq mut v,42);let const result=same[Vec,uint32](v);output(vec_slice(ref result)[0]);},_->panic()}}","42\n","",0},
    {"constructor parameter captured in a partial application","struct Cell[T:Move]{T value;}safe io F[T] choose[F[_],T:Move](const F[T] a,const F[T] b){return b;}safe io F[T] forward[F[_],T:Move](const F[T] a,const F[T] b){let const f=choose(a,_);return f(b);}safe io void main(){output(forward(Cell[uint32]{value=7},Cell[uint32]{value=42}).value);}","42\n","",0},
};
static const Bad kind_bad[]={
    {"struct View[T:Move]{ref T value;}safe io void discard[F[_],T:Move](const F[T] value){}safe io void main(){const uint32 n=42;discard(View[uint32]{value=ref n});}","ordinary owned values"},
    {"safe io void discard[F[_,_]](){}safe io void main(){discard[Lazy]();}","ordinary owned values"},
    {"safe io F[T] f[F[_],T:Move](const F[T] x){return x;}safe io void main(){f[uint32,uint32](42);}","kind/arity"},
    {"struct Pair[A:Move,B:Move]{A a;B b;}safe io F[T] f[F[_],T:Move](const F[T] x){return x;}safe io void main(){f[Pair,uint32](Pair[uint32,uint32]{a=1,b=2});}","kind/arity"},
    {"struct Cell[T:Copy]{T value;}safe io F[T] f[F[_],T:Move](const F[T] x){return x;}safe io void main(){f(Cell[uint32]{value=42});}","arbitrary owned"},
    {"safe io void f[F[_]](const F value){}","explicit type arguments"},
    {"safe io void f[F[_],T:Move](const F[T,T] value){}","incorrect arity"},
    {"safe io uint32 f[F[_],T:Move](const F[T] value){return value.x;}","field access requires"},
    {"safe pure F[T] f[F[_],T:Move](const F[T] value){return value;}","violates `pure`"},
    {"safe io void f[F[_]:Copy](){}","constructor parameters"},
};
