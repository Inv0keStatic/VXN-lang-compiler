#ifndef VXN_HEAP_H
#define VXN_HEAP_H
#ifndef VXN_HEAP_API_H
#include "heap_api.h"
#endif
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
/* The checked compiler supplies a valid exclusive view and element stride.
   No element is read/moved, and invalid cuts return the original descriptor. */
static inline void vx_slice_split(const uint64_t *args,size_t stride,uint64_t *out) {
    uint64_t address=args[0],length=args[1],cut=args[2];
    memset(out,0,5*sizeof *out);
    if(cut>length) { out[0]=1; out[1]=address; out[2]=length; return; }
    out[1]=address; out[2]=cut;
    out[3]=cut?(uint64_t)(uintptr_t)((uint64_t *)(uintptr_t)address+(size_t)cut*stride):address;
    out[4]=length-cut;
}
#ifndef VX_HEAP_ALLOC
#define VX_HEAP_ALLOC malloc
#endif
#ifndef VX_HEAP_FREE
#define VX_HEAP_FREE free
#endif
/* One allocation holds metadata and payload; moves/borrows/cleanup never allocate.
   Slot 0 is temporary; positive slots identify frame locals. Children move with owners. */
typedef struct VxHeapNode VxHeapNode;
struct VxHeapNode { VxHeapNode *next,**previous,*children,*owner; uint64_t slot; size_t cells; uint64_t data[]; };
typedef struct { VxHeapNode *frames[97]; uint64_t *regions[97]; unsigned depth; size_t live; } VxHeap;
/* A region is four scalar cells: previous record, storage address, cell count,
   cleanup slot. Records live on the caller's stack, never in heap allocations. */
static inline void vx_heap_region_pop(VxHeap *h,uint64_t *record) {
    uint64_t *previous=NULL,*r=h->regions[h->depth];
    while(r) {
        uint64_t *next=(uint64_t *)(uintptr_t)r[0];
        if(r==record) { if(previous) previous[0]=r[0]; else h->regions[h->depth]=next; return; }
        previous=r; r=next;
    }
}
static inline void vx_heap_region_end(VxHeap *h,uint64_t slot) {
    for(uint64_t *r=h->regions[h->depth];r;r=(uint64_t *)(uintptr_t)r[0])
        if(r[3]==slot) { vx_heap_region_pop(h,r); return; }
}
static inline void vx_heap_region_bind(VxHeap *h,uint64_t *record,const uint64_t *storage,size_t cells,uint64_t slot) {
    if(slot) vx_heap_region_end(h,slot);
    record[0]=(uint64_t)(uintptr_t)h->regions[h->depth]; record[1]=(uint64_t)(uintptr_t)storage;
    record[2]=(uint64_t)cells; record[3]=slot; h->regions[h->depth]=record;
}
static void vx_heap_detach(VxHeapNode *n) {
    *n->previous=n->next;
    if(n->next) n->next->previous=n->previous;
}
static void vx_heap_link_to(VxHeapNode **head,VxHeapNode *n,VxHeapNode *owner,uint64_t slot) {
    n->owner=owner; n->slot=slot; n->next=*head; n->previous=head;
    if(n->next) n->next->previous=&n->next;
    *head=n;
}
static void vx_heap_link(VxHeap *h,VxHeapNode *n,unsigned frame,uint64_t slot) {
    vx_heap_link_to(&h->frames[frame],n,NULL,slot);
}
static VxHeapNode *vx_heap_node(uint64_t address) {
    return (VxHeapNode *)((unsigned char *)(uintptr_t)address - offsetof(VxHeapNode,data));
}
static uint64_t vx_heap_new(VxHeap *h,const uint64_t *value,size_t cells) {
    if(cells>(SIZE_MAX-sizeof(VxHeapNode))/sizeof(uint64_t)) return 0;
    VxHeapNode *n=VX_HEAP_ALLOC(sizeof *n+cells*sizeof(uint64_t)); if(!n) return 0;
    n->children=NULL; n->cells=cells;
    if(value) memcpy(n->data,value,cells*sizeof(uint64_t));
    vx_heap_link(h,n,h->depth,0); ++h->live;
    return (uint64_t)(uintptr_t)n->data;
}
static void vx_heap_rehome(VxHeap *h,const uint64_t *value,const uint64_t *map,VxHeapNode *owner,int parent,uint64_t slot) {
    while(map&&*map) {
        uint64_t op=*map++;
        if(op==1) {
            uint64_t address=value[*map++];
            if(address) {
                VxHeapNode *n=vx_heap_node(address);
                vx_heap_detach(n);
                if(owner) vx_heap_link_to(&owner->children,n,owner,0);
                else vx_heap_link(h,n,h->depth-(unsigned)parent,slot);
            }
        } else {
            uint64_t offset=map[0],tag=map[1],skip=map[2]; map+=3;
            if(value[offset]!=tag) map+=skip;
        }
    }
}
static void vx_heap_adopt(VxHeap *h,const uint64_t *value,const uint64_t *map,int parent,uint64_t slot) {
    vx_heap_rehome(h,value,map,NULL,parent,slot);
}
static uint64_t vx_heap_box(VxHeap *h,const uint64_t *value,size_t cells,const uint64_t *map) {
    uint64_t address=vx_heap_new(h,value,cells);
    if(address) vx_heap_rehome(h,value,map,vx_heap_node(address),0,0);
    return address;
}
/* Free children before owners, without recursion or allocation. */
static void vx_heap_destroy(VxHeap *h,VxHeapNode *root) {
    VxHeapNode *n=root;
    for(;;) {
        if(n->children) { n=n->children; continue; }
        VxHeapNode *parent=n->owner; int done=n==root;
        vx_heap_detach(n); VX_HEAP_FREE(n); --h->live;
        if(done) return;
        n=parent;
    }
}
static void vx_heap_destroy_value(VxHeap *h,const uint64_t *value,const uint64_t *map) {
    while(map&&*map) {
        uint64_t op=*map++;
        if(op==1) {
            uint64_t address=value[*map++];
            if(address) vx_heap_destroy(h,vx_heap_node(address));
        } else {
            uint64_t offset=map[0],tag=map[1],skip=map[2]; map+=3;
            if(value[offset]!=tag) map+=skip;
        }
    }
}
static int vx_heap_contains(uintptr_t base,size_t capacity,uintptr_t address,size_t cells) {
    if(address<base||(address-base)%sizeof(uint64_t)) return 0;
    size_t offset=(address-base)/sizeof(uint64_t);
    return offset<capacity&&cells<=capacity-offset;
}
/* Find the storage even for empty choices and partially moved aggregates. */
typedef struct { VxHeapNode *owner; unsigned frame; uint64_t slot; } VxHeapPlace;
static inline int vx_heap_locate(VxHeap *h,uint64_t *destination,size_t cells,VxHeapPlace *place) {
    uintptr_t address=(uintptr_t)destination; VxHeapNode *owner=NULL;
    unsigned frame=0; uint64_t slot=0; int found=0;
    for(unsigned depth=h->depth+1;depth&&!found;) {
        --depth;
        for(uint64_t *r=h->regions[depth];r;r=(uint64_t *)(uintptr_t)r[0])
            if(vx_heap_contains((uintptr_t)r[1],(size_t)r[2],address,cells)) {
                frame=depth; slot=r[3]; found=1; break;
            }
        for(VxHeapNode *root=h->frames[depth];root&&!found;root=root->next) {
            VxHeapNode *n=root;
            for(;;) {
                if(vx_heap_contains((uintptr_t)n->data,n->cells,address,cells)) { owner=n; found=1; break; }
                if(n->children) { n=n->children; continue; }
                while(n!=root&&!n->next) n=n->owner;
                if(n==root) break;
                n=n->next;
            }
        }
    }
    if(found) *place=(VxHeapPlace){owner,frame,slot};
    return found;
}
/* Arguments precede exchange; no allocation, destruction or callbacks during mutation.
   Return the old owner through output, or destroy it for assignment. */
static inline int vx_heap_exchange(VxHeap *h,uint64_t *destination,const uint64_t *value,size_t cells,const uint64_t *map,uint64_t *previous) {
    VxHeapPlace place={0};
    if(map&&!vx_heap_locate(h,destination,cells,&place)) return 0;
    if(previous) {
        memcpy(previous,destination,cells*sizeof(uint64_t));
        vx_heap_adopt(h,previous,map,0,0);
    } else vx_heap_destroy_value(h,destination,map);
    memcpy(destination,value,cells*sizeof(uint64_t));
    vx_heap_rehome(h,destination,map,place.owner,(int)(h->depth-place.frame),place.slot);
    return 1;
}
static inline int vx_heap_replace(VxHeap *h,uint64_t *destination,const uint64_t *value,size_t cells,const uint64_t *map) {
    return vx_heap_exchange(h,destination,value,cells,map,NULL);
}
/* Checked disjoint initialized places; caller-owned scratch. Locate both before mutation. */
static inline int vx_heap_swap(VxHeap *h,uint64_t *left,uint64_t *right,size_t cells,const uint64_t *map,uint64_t *scratch) {
    VxHeapPlace a={0},b={0};
    if(map&&(!vx_heap_locate(h,left,cells,&a)||!vx_heap_locate(h,right,cells,&b))) return 0;
    memcpy(scratch,left,cells*sizeof(uint64_t));
    memcpy(left,right,cells*sizeof(uint64_t));
    memcpy(right,scratch,cells*sizeof(uint64_t));
    vx_heap_rehome(h,left,map,a.owner,(int)(h->depth-a.frame),a.slot);
    vx_heap_rehome(h,right,map,b.owner,(int)(h->depth-b.frame),b.slot);
    return 1;
}
static inline int vx_memory_exec(VxHeap *h,int swap,const uint64_t *args,size_t cells,const uint64_t *map,uint64_t *out) {
    uint64_t *target=(uint64_t *)(uintptr_t)args[0];
    return swap?vx_heap_swap(h,target,(uint64_t *)(uintptr_t)args[1],cells,map,out):vx_heap_exchange(h,target,args+1,cells,map,out);
}
static inline void vx_heap_unbox(VxHeap *h,uint64_t address,size_t cells,const uint64_t *map,uint64_t *result) {
    memcpy(result,(const void *)(uintptr_t)address,cells*sizeof(uint64_t));
    vx_heap_adopt(h,result,map,0,0);
    vx_heap_destroy(h,vx_heap_node(address));
}
/* Vec owns a stable three-cell header: storage address, initialized length,
   capacity. Its storage node owns only the live elements' nested allocations.
   Growth allocates before modifying any old state, then commits without failure. */
static int vx_vec_reserve(VxHeap *h,uint64_t *v,uint64_t additional,size_t stride) {
    if(additional>UINT32_MAX-v[1]) return 0;
    uint64_t need=v[1]+additional;
    if(need<=v[2]) return 1;
    uint64_t capacity=v[2]?v[2]*2:4;
    if(capacity>UINT32_MAX) capacity=UINT32_MAX;
    if(capacity<need) capacity=need;
    if(!stride||capacity>(SIZE_MAX-sizeof(VxHeapNode))/sizeof(uint64_t)/stride) return 0;
    uint64_t address=vx_heap_new(h,NULL,(size_t)capacity*stride);
    if(!address) return 0;
    VxHeapNode *fresh=vx_heap_node(address);
    if(v[0]) {
        VxHeapNode *old=vx_heap_node(v[0]);
        memcpy(fresh->data,old->data,(size_t)v[1]*stride*sizeof(uint64_t));
        fresh->children=old->children; old->children=NULL;
        if(fresh->children) fresh->children->previous=&fresh->children;
        for(VxHeapNode *n=fresh->children;n;n=n->next) n->owner=fresh;
        vx_heap_destroy(h,old);
    }
    vx_heap_detach(fresh);
    VxHeapNode *owner=vx_heap_node((uint64_t)(uintptr_t)v);
    vx_heap_link_to(&owner->children,fresh,owner,0);
    v[0]=address; v[2]=capacity; return 1;
}
/* Caller supplies a checked operation, argument cells, concrete element stride
   and owner map. Output is zeroed by each operation before selecting its tag. */
static inline void vx_vec_exec(VxHeap *h,int op,const uint64_t *args,size_t stride,const uint64_t *map,uint64_t *result) {
    if(op==VX_VEC_NEW) {
        const uint64_t empty[3]={0,0,0}; uint64_t address=vx_heap_new(h,empty,3);
        result[0]=address?0:1; result[1]=address; return;
    }
    uint64_t address=*(const uint64_t *)(uintptr_t)args[0];
    uint64_t *v=(uint64_t *)(uintptr_t)address;
    if(op==VX_VEC_LEN||op==VX_VEC_CAPACITY) { result[0]=v[op==VX_VEC_LEN?1:2]; return; }
    if(op==VX_VEC_SLICE||op==VX_VEC_UNIQ) {
        result[0]=v[0]?v[0]:(uint64_t)(uintptr_t)(v+3); result[1]=v[1]; return;
    }
    if(op==VX_VEC_INSERT) {
        memset(result,0,(stride+2)*sizeof(uint64_t)); uint64_t index=args[1];
        if(index>v[1]) { result[0]=1; memcpy(result+1,args+2,stride*sizeof(uint64_t)); vx_heap_adopt(h,result+1,map,0,0); return; }
        if(!vx_vec_reserve(h,v,1,stride)) { result[0]=2; memcpy(result+2,args+2,stride*sizeof(uint64_t)); vx_heap_adopt(h,result+2,map,0,0); return; }
        uint64_t *data=(uint64_t *)(uintptr_t)v[0],*target=data+(size_t)index*stride;
        memmove(target+stride,target,(size_t)(v[1]-index)*stride*sizeof(uint64_t));
        memcpy(target,args+2,stride*sizeof(uint64_t));
        vx_heap_rehome(h,target,map,vx_heap_node(v[0]),0,0); ++v[1]; return;
    }
    if(op==VX_VEC_RESERVE||op==VX_VEC_PUSH) {
        result[0]=result[1]=0;
        if(!vx_vec_reserve(h,v,op==VX_VEC_PUSH?1:args[1],stride)) { result[0]=1; return; }
        if(op==VX_VEC_PUSH) {
            uint64_t *target=(uint64_t *)(uintptr_t)v[0]+(size_t)v[1]*stride;
            memcpy(target,args+1,stride*sizeof(uint64_t));
            vx_heap_rehome(h,target,map,vx_heap_node(v[0]),0,0); ++v[1];
        }
        return;
    }
    if(op==VX_VEC_POP||op==VX_VEC_REMOVE||op==VX_VEC_SWAP_REMOVE) {
        memset(result,0,(stride+1)*sizeof(uint64_t));
        uint64_t index=op==VX_VEC_POP?v[1]-1:args[1];
        if(index>=v[1]) { result[0]=1; return; }
        uint64_t *data=(uint64_t *)(uintptr_t)v[0],*source=data+(size_t)index*stride;
        --v[1]; memcpy(result+1,source,stride*sizeof(uint64_t));
        if(index<v[1]) {
            if(op==VX_VEC_SWAP_REMOVE) memcpy(source,data+(size_t)v[1]*stride,stride*sizeof(uint64_t));
            else memmove(source,source+stride,(size_t)(v[1]-index)*stride*sizeof(uint64_t));
        }
        vx_heap_adopt(h,result+1,map,0,0); return;
    }
    if(op==VX_VEC_TRUNCATE) {
        /* Drop initialized elements only; spare capacity may contain stale descriptors. */
        while(v[1]>args[1]) {
            --v[1];
            vx_heap_destroy_value(h,(uint64_t *)(uintptr_t)v[0]+(size_t)v[1]*stride,map);
        }
        return;
    }
    if(op==VX_VEC_CLEAR) {
        if(v[0]) {
            VxHeapNode *buffer=vx_heap_node(v[0]);
            while(buffer->children) vx_heap_destroy(h,buffer->children);
        }
        v[1]=0; return;
    }
}
static void vx_heap_drop(VxHeap *h,uint64_t slot,int all) {
    VxHeapNode *n=h->frames[h->depth];
    while(n) {
        VxHeapNode *next=n->next;
        if(all||n->slot==slot) { vx_heap_destroy(h,n); }
        n=next;
    }
}
static void vx_heap_enter(VxHeap *h) { ++h->depth; h->regions[h->depth]=NULL; }
static void vx_heap_leave(VxHeap *h) { vx_heap_drop(h,0,1); h->regions[h->depth]=NULL; --h->depth; }
/* Panic teardown frees host allocations without language unwinding. Native panic exits. */
static void vx_heap_finish(VxHeap *h) {
    while(h->depth) vx_heap_leave(h);
    vx_heap_drop(h,0,1);
}
#endif
