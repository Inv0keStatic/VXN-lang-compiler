#define LIBRARY_HELPERS \
    "safe Canvas canvas(const CanvasOpen result) { match result { CanvasOpen::Ok(const c) -> { return c; }, CanvasOpen::Error(const e) -> { panic(); } } }" \
    "safe void need(const GfxResult result) { match result { GfxResult::Ok -> {}, GfxResult::Error(const e) -> { panic(); } } }" \
    "safe void pixel(const Canvas c, const wrap sint32 x, const wrap sint32 y) { match canvas_read_pixel(c,x,y) { PixelRead::Ok(const color) -> { output(color.r); output(color.g); output(color.b); output(color.a); }, PixelRead::Error(const e) -> { panic(); } } }"
static const Case library_cases[]={
    {"canvas copy named arguments clipping overlap and effects",
     LIBRARY_HELPERS
     "safe io noalloc noblock nopanic GfxResult copy(const Canvas destination,const Canvas source){return canvas_copy(y:1,source:source,x:1,destination:destination);}"
     "safe io void main(){const Canvas a=canvas(canvas_create(3,3));const Canvas b=canvas(canvas_create(3,3));need(draw_clear(a,rgb(1,2,3)));need(draw_pixel(a,0,0,rgb(9,8,7)));need(copy(b,a));pixel(b,1,1);pixel(b,0,0);need(canvas_copy(a,a,1,1));pixel(a,1,1);pixel(a,2,2);need(canvas_copy(b,a,-2,-2));pixel(b,0,0);need(canvas_close(a));match canvas_copy(b,a,0,0){GfxResult::Error(GfxError::InvalidHandle)->output(true),_->panic()}pixel(b,0,0);need(canvas_close(b));}",
     "9\n8\n7\n255\n0\n0\n0\n255\n9\n8\n7\n255\n1\n2\n3\n255\n1\n2\n3\n255\ntrue\n1\n2\n3\n255\n","",0},
    {"canvas pixels and alpha blending",
     LIBRARY_HELPERS
     "safe io void main() { const Canvas c=canvas(canvas_create(4,4)); need(draw_clear(c,rgb(10,20,30))); need(draw_pixel(c,1,2,rgba(210,120,30,128))); pixel(c,1,2); pixel(c,0,0); need(canvas_close(c)); }",
     "110\n70\n30\n255\n10\n20\n30\n255\n","",0},
    {"clipped rectangles and extreme line endpoints",
     LIBRARY_HELPERS
     "safe io void main() { const Canvas c=canvas(canvas_create(4,4)); need(draw_rectangle(c,-1,-1,3,3,rgb(255,0,0))); need(draw_line(c,-2147483648,3,2147483647,3,rgb(0,255,0))); pixel(c,0,0); pixel(c,3,3); pixel(c,2,2); need(canvas_close(c)); }",
     "255\n0\n0\n255\n0\n255\n0\n255\n0\n0\n0\n255\n","",0},
    {"filled circle and clipped text",
     LIBRARY_HELPERS
     "safe io void main() { const Canvas c=canvas(canvas_create(12,12)); need(draw_circle(c,5,5,2,rgb(0,0,255))); pixel(c,5,5); pixel(c,7,7); need(draw_text(c,0,0,1,rgb(255,255,255),b\"A\")); pixel(c,1,0); need(canvas_close(c)); }",
     "0\n0\n255\n255\n0\n0\n0\n255\n255\n255\n255\n255\n","",0},
    {"canvas aliases stale handles and slot reuse",
     LIBRARY_HELPERS
     "safe io void main() { const Canvas a=canvas(canvas_create(1,1)); const Canvas old=a; need(canvas_close(a)); const Canvas b=canvas(canvas_create(1,1)); match draw_clear(old,rgb(1,2,3)) { GfxResult::Error(const e) -> { output(true); }, GfxResult::Ok -> { panic(); } } pixel(b,0,0); need(canvas_close(b)); }",
     "true\n0\n0\n0\n255\n","",0},
    {"library failures are recoverable values",
     LIBRARY_HELPERS
     "safe io void main() { match canvas_create(0,5) { CanvasOpen::Error(const e) -> { output(true); }, CanvasOpen::Ok(const c) -> { panic(); } } const Canvas c=canvas(canvas_create(2,2)); match canvas_read_pixel(c,-1,0) { PixelRead::Error(const e) -> { output(true); }, PixelRead::Ok(const color) -> { panic(); } } match draw_text(c,0,0,1,rgb(0,0,0),b\"\\0\") { GfxResult::Error(const e) -> { output(true); }, GfxResult::Ok -> { panic(); } } need(canvas_close(c)); }",
     "true\ntrue\ntrue\n","",0},
    {"drawing supports noalloc noblock nopanic",
     LIBRARY_HELPERS
     "safe io noalloc noblock nopanic GfxResult paint(const Canvas c) { return draw_pixel(c,0,0,rgb(7,8,9)); }"
     "safe io void main() { const Canvas c=canvas(canvas_create(1,1)); need(paint(c)); pixel(c,0,0); }",
     "7\n8\n9\n255\n","",0},
    {"canvas cleanup on panic",
     LIBRARY_HELPERS
     "safe io void main() { const Canvas c=canvas(canvas_create(8,8)); need(draw_clear(c,rgb(1,2,3))); panic(); }",
     "","panic\n",1},
#ifdef _WIN32
    {"hidden Win32 window through Vxn cell ABI",
     LIBRARY_HELPERS
     "safe io void main() { match window_open(32,24,WindowMode::Hidden,b\"Vxn test\") { WindowOpen::Error(const e) -> { panic(); }, WindowOpen::Ok(const w) -> { const Canvas c=canvas(window_canvas(w)); need(draw_clear(c,rgb(21,22,23))); need(window_present(w)); match window_poll(w) { FrameRead::Error(const e) -> { panic(); }, FrameRead::Ok(const frame) -> { output(frame.width); output(frame.height); output(frame.should_close); output(key_down(ref frame,Key::Left)); output(mouse_pressed(ref frame,MouseButton::Left)); } } pixel(c,0,0); need(window_close(w)); match draw_clear(c,rgb(0,0,0)) { GfxResult::Error(const e) -> { output(true); }, GfxResult::Ok -> { panic(); } } } } }",
     "32\n24\nfalse\nfalse\nfalse\n21\n22\n23\n255\ntrue\n","",0},
#endif
#if defined(_WIN32) || defined(__linux__)
    {"monotonic clock and bounded sleep",
     "safe io void main() { match time_sleep(0) { TimeResult::Ok -> { output(true); }, TimeResult::Error(const e) -> { panic(); } } match time_sleep(60001) { TimeResult::Error(const e) -> { output(true); }, TimeResult::Ok -> { panic(); } } match time_now() { TimeRead::Seconds(const a) -> { match time_now() { TimeRead::Seconds(const b) -> { output(b >= a); }, TimeRead::Error(const e) -> { panic(); } } }, TimeRead::Error(const e) -> { panic(); } } }",
     "true\ntrue\ntrue\n","",0},
#endif
};
static const Bad library_bad[]={
    {"safe pure void main() { canvas_create(4,4); }","violates"},
    {"safe io noalloc void main() { canvas_create(4,4); }","violates"},
    {"safe io noblock void main() { time_sleep(1); }","violates"},
    {"safe io void main() { canvas_create(true,4); }","expected wrap uint32"},
    {"safe io void main() { window_open(4,4,b\"x\"); }","expects 4 arguments"},
    {"safe io void main() { window_open(4,4,WindowMode::Hidden,1); }","fixed array of wrap uint8"},
    {"safe pure void main() { const Canvas c=Canvas {}; }","Canvas is opaque"},
    {"safe pure void main() { const Window w=Window {}; }","Window is opaque"},
    {"struct Canvas { bool x; }","cannot redefine opaque"},
    {"safe pure wrap uint64 expose(const Canvas c) { return cast[wrap uint64](c); }","require numeric"},
    {"safe io GfxResult wrong(const Window w) { return draw_clear(w,rgb(0,0,0)); }","expected Canvas"},
    {"safe pure void canvas_create() {}","cannot redefine a builtin"},
};
#undef LIBRARY_HELPERS
