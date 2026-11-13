/* End-to-end tests use isolated paths, raw bytes and an independent file read. */
static const char io_helpers[] =
    "safe pure wrap uint32 error_code(const IoError e) { return match e {"
    "IoError::NotFound -> 1, IoError::PermissionDenied -> 2, IoError::InvalidPath -> 3,"
    "IoError::InvalidHandle -> 4, IoError::WrongMode -> 5, IoError::ResourceLimit -> 6,"
    "IoError::ReadFailed -> 7, IoError::WriteFailed -> 8, IoError::CloseFailed -> 9,"
    "IoError::FlushFailed -> 10, IoError::OpenFailed -> 11 }; }"
    "safe pure wrap uint32 status(const IoResult r) { return match r {"
    "IoResult::Ok -> 0, IoResult::Error(const e) -> error_code(e) }; }"
    "safe io IoFile opened(const IoOpen r) { return match r {"
    "IoOpen::Ok(const f) -> f, IoOpen::Error(const e) -> panic() }; }"
    "safe pure wrap uint32 open_error(const IoOpen r) { return match r {"
    "IoOpen::Ok(const f) -> 0, IoOpen::Error(const e) -> error_code(e) }; }"
    "safe io void print_read(const IoRead r) { match r {"
    "IoRead::Byte(const b) -> output(cast[wrap uint32](b)), IoRead::End -> output(256),"
    "IoRead::Error(const e) -> output(512 + error_code(e)) } }";

static const char io_buffer_helpers[] =
    "safe io void print_buffer_read(const IoReadBuffer r){match r {"
    "IoReadBuffer::Read(const n)->output(n),IoReadBuffer::End->output(1000),"
    "IoReadBuffer::Error(const e,const n)->{output(2000+error_code(e));output(n);}}}";
static void io_file_tests(const char *cc) {
    Files f; if(!setup(&f)) return;
    char source[14000],path[256];
    snprintf(path,sizeof path,"./%s/data.bin",f.dir);
    snprintf(source,sizeof source,
        "%s safe io void line(const IoLine r){match r{IoLine::Read(const n)->output(n),IoLine::End->output(1000),IoLine::Error(IoLineError::Limit,const n)->output(2000+n),_->panic()}}"
        "safe io void main(){const IoFile w=opened(io_create(b\"%s\"));io_write(w,b\"A\\r\\n\\nB\\0\\xffZ\");io_close(w);"
        "const IoFile r=opened(io_open_read(b\"%s\"));match vec_new[wrap uint8](){VecResult[wrap uint8]::Ok(mut bytes)->{"
        "line(io_read_line(r,uniq mut bytes,0));line(io_read_line(r,uniq mut bytes,2));line(io_read_line(limit:1,buffer:uniq mut bytes,file:r));"
        "output(vec_len(ref bytes));output(vec_slice(ref bytes)[1]);output(vec_slice(ref bytes)[2]);vec_clear(uniq mut bytes);"
        "line(io_read_line(r,uniq mut bytes,1));vec_clear(uniq mut bytes);line(io_read_line(r,uniq mut bytes,20));"
        "output(vec_slice(ref bytes)[0]);output(vec_slice(ref bytes)[1]);output(vec_slice(ref bytes)[2]);output(vec_slice(ref bytes)[3]);"
        "line(io_read_line(r,uniq mut bytes,20));io_close(r);match io_read_line(r,uniq mut bytes,20){"
        "IoLine::Error(IoLineError::Io(IoError::InvalidHandle),0)->output(7),_->panic()}"
        "match io_read_line(io_stdout(),uniq mut bytes,20){IoLine::Error(IoLineError::Io(IoError::WrongMode),0)->output(9),_->panic()}"
        "},_->panic()}}",io_helpers,path,path);
    Case lines={"bounded binary line input, retries, EOF and errors",source,"2000\n2002\n1\n3\n13\n10\n1\n4\n66\n0\n255\n90\n1000\n7\n9\n","",0};
    if(cc) native_case(&lines,cc,&f); else interpret(&lines);

    snprintf(source,sizeof source,
        "%s safe io void main() {"
        "let const f = opened(io_create(b\"%s\")); let const alias = f;"
        "output(status(io_write(f,b\"\\0\\n\\r\\x1a\\xffABC\")));"
        "output(status(io_flush(f))); output(status(io_close(f))); output(status(io_close(alias)));"
        "let const r = opened(io_open_read(b\"%s\"));"
        "output(status(io_write_byte(r,1))); print_read(io_read_byte(alias));"
        "output(status(io_write(alias,b\"\")));"
        "for (let mut i = 0; i < 10; i++) { print_read(io_read_byte(r)); }"
        "output(status(io_close(r)));"
        "let const a = opened(io_open_append(b\"%s\"));"
        "output(status(io_write_byte(a,90))); output(status(io_close(a)));"
        "output(open_error(io_open_read(b\"\")));"
        "output(open_error(io_open_read(b\"%s\\0suffix\")));"
        "output(open_error(io_open_read(b\"./%s/missing.bin\")));"
        "output(status(io_close(io_stdout())));"
        "print_read(io_read_byte(io_stdout())); output(status(io_write_byte(io_stdin(),1)));"
        "}",io_helpers,path,path,path,path,f.dir);
    Case roundtrip={"I/O files, modes, EOF and stale aliases",source,
        "0\n0\n0\n4\n5\n516\n4\n0\n10\n13\n26\n255\n65\n66\n67\n256\n256\n0\n0\n0\n3\n3\n1\n5\n517\n5\n","",0};
    if(cc) native_case(&roundtrip,cc,&f); else interpret(&roundtrip);
    FILE *file=fopen(path,"rb"); unsigned char bytes[16];
    if(file) {
        size_t n=fread(bytes,1,sizeof bytes,file); fclose(file);
        const unsigned char wanted[]={0,10,13,26,255,65,66,67,90};
        expect(n==sizeof wanted && !memcmp(bytes,wanted,sizeof wanted),"file bytes and append verified independently");
    } else expect(false,"open I/O roundtrip result");

    snprintf(source,sizeof source,"%s safe io void main() {"
        "let const f = opened(io_create(b\"%s\")); io_write(f,b\"new\");"
        "io_close(f); let const r = opened(io_open_read(b\"%s\"));"
        "for (let mut i = 0; i < 4; i++) { print_read(io_read_byte(r)); } }",io_helpers,path,path);
    Case truncate={"create truncates and shutdown closes remaining input",source,"110\n101\n119\n256\n","",0};
    if(cc) native_case(&truncate,cc,&f); else interpret(&truncate);

    snprintf(source,sizeof source,"%s safe io void main(){"
        "let const path_bytes=b\"[%s]\";let const path=slice(ref path_bytes,1,%zu);"
        "let const data=b\"xABCy\";let const f=opened(io_create(path));"
        "output(status(io_write(f,slice(ref data,1,2))));io_close(f);"
        "let const a=opened(io_open_append(path));io_write(a,slice(ref data,3,1));io_close(a);"
        "let const r=opened(io_open_read(path));"
        "for(let mut i=0;i<4;i++){print_read(io_read_byte(r));}io_close(r);}",io_helpers,path,strlen(path));
    Case sliced={"sliced file paths and byte buffers",source,"0\n65\n66\n67\n256\n","",0};
    if(cc) native_case(&sliced,cc,&f); else interpret(&sliced);
    file=fopen(path,"rb");
    if(file) { size_t n=fread(bytes,1,sizeof bytes,file); fclose(file); expect(n==3&&!memcmp(bytes,"ABC",3),"sliced file output verified independently"); }
    else expect(false,"open sliced I/O result");

    snprintf(source,sizeof source,"%s%s safe io void main(){"
        "let const f=opened(io_create(b\"%s\"));io_write(f,b\"\\0\\xffABC\");io_close(f);"
        "let const r=opened(io_open_read(b\"%s\"));mut wrap uint8[7] bytes=b\"xxxxxxx\";"
        "let const view=uniq_slice(uniq mut bytes,1,5);"
        "print_buffer_read(io_read(buffer:uniq_slice(uniq mut view,0,0),file:r));"
        "print_buffer_read(io_read(r,uniq_slice(uniq mut view,0,3)));"
        "print_buffer_read(io_read(r,uniq_slice(uniq mut view,3,2)));"
        "print_buffer_read(io_read(r,uniq mut view));"
        "output(bytes[0]);output(bytes[1]);output(bytes[2]);output(bytes[3]);output(bytes[4]);output(bytes[5]);output(bytes[6]);"
        "io_close(r);print_buffer_read(io_read(r,uniq_slice(uniq mut bytes)));"
        "print_buffer_read(io_read(io_stdout(),uniq_slice(uniq mut bytes)));}"
        ,io_helpers,io_buffer_helpers,path,path);
    Case buffered={"buffered input, named reborrows, EOF and stale handles",source,
        "0\n3\n2\n1000\n120\n0\n255\n65\n66\n67\n120\n2004\n0\n2005\n0\n","",0};
    if(cc) native_case(&buffered,cc,&f); else interpret(&buffered);

    snprintf(source,sizeof source,"%s%s safe io void main(){"
        "let const r=opened(io_open_read(b\"%s\"));mut wrap uint8[8] bytes=b\"xxxxxxxx\";"
        "print_buffer_read(io_read(r,uniq_slice(uniq mut bytes)));"
        "output(bytes[4]);output(bytes[5]);output(bytes[7]);io_close(r);}",io_helpers,io_buffer_helpers,path);
    Case short_read={"buffered short read preserves suffix",source,"5\n67\n120\n120\n","",0};
    if(cc) native_case(&short_read,cc,&f); else interpret(&short_read);

    snprintf(source,sizeof source,"%s safe io void main() {"
        "for (let mut i = 0; i < 64; i++) { opened(io_open_append(b\"%s\")); }"
        "output(open_error(io_open_append(b\"%s\"))); }",io_helpers,path,path);
    Case limit={"file-table exhaustion is recoverable",source,"6\n","",0};
    if(cc) native_case(&limit,cc,&f); else interpret(&limit);

    snprintf(source,sizeof source,"%s safe io void main() {"
        "let const f = opened(io_create(b\"%s\")); io_write(f,b\"cleanup\"); panic(); }",io_helpers,path);
    Case cleanup_case={"panic closes and flushes files",source,"","panic\n",1};
    if(cc) native_case(&cleanup_case,cc,&f); else interpret(&cleanup_case);
    file=fopen(path,"rb");
    if(file) {
        size_t n=fread(bytes,1,sizeof bytes,file); fclose(file);
        expect(n==7 && !memcmp(bytes,"cleanup",7),"panic preserves buffered file output");
    } else expect(false,"open panic cleanup result");
    expect(remove(path)==0,"all library file handles released"); cleanup(&f);
}

static void io_stream_tests(const char *cc) {
    const char *source=
        "safe io void main() { for (;;) { match io_read_byte(io_stdin()) {"
        "IoRead::Byte(const b) -> { match io_write_byte(io_stdout(),b) {"
        "IoResult::Ok -> {}, IoResult::Error(const e) -> { panic(); } } },"
        "IoRead::End -> { return; }, IoRead::Error(const e) -> { panic(); } } } }";
    unsigned char wanted[512],actual[513];
    for(size_t i=0;i<sizeof wanted;++i) wanted[i]=(unsigned char)i;
    VxnProgram *p=vxn_compile(source); expect(vxn_ok(p),"binary echo compiles");
    if(!vxn_ok(p)) { vxn_diagnostic(p,"echo",stderr); vxn_free(p); return; }
    FILE *in=tmpfile(),*out=tmpfile(),*err=tmpfile();
    if(in&&out&&err) {
        fwrite(wanted,1,sizeof wanted,in); rewind(in);
        expect(vxn_run_io(p,in,out,err)==0,"binary interpreter echo");
        rewind(out); size_t n=fread(actual,1,sizeof actual,out);
        expect(n==sizeof wanted&&!memcmp(actual,wanted,n),"all 256 byte values survive interpreter I/O");
        char error[128]; contents(err,error,sizeof error); expect(!error[0],"binary interpreter stderr empty");
    } else expect(false,"binary echo streams");
    if(in) fclose(in);
    if(out) fclose(out);
    if(err) fclose(err);
    if(cc) {
        Files f;
        if(setup(&f)) {
            char input[256],error[8192]; snprintf(input,sizeof input,"./%s/stdin.bin",f.dir);
            FILE *file=fopen(input,"wb");
            if(file) { fwrite(wanted,1,sizeof wanted,file); fclose(file); }
            else expect(false,"binary native input fixture");
            for(int optimize=0;optimize<=1;++optimize) {
                snprintf(f.exe,sizeof f.exe,"./%s/program_%u.exe",f.dir,f.executables++);
                int status=build_case(p,source,cc,&f,optimize);
                expect(status==0,"build binary echo");
                if(status) { read_file(f.err,error,sizeof error); fputs(error,stderr); break; }
                const char *run[]={f.exe,NULL};
                expect(vxn_process_redirect_io(run,input,f.out,f.err)==0,"native binary echo");
                file=fopen(f.out,"rb");
                if(file) {
                    size_t n=fread(actual,1,sizeof actual,file); fclose(file);
                    expect(n==sizeof wanted&&!memcmp(actual,wanted,n),"all 256 byte values survive native I/O");
                } else expect(false,"native binary echo result");
                read_file(f.err,error,sizeof error); expect(!error[0],"native binary stderr empty");
            }
            remove(input); cleanup(&f);
        }
    }
    vxn_free(p);
}
