#include <stdio.h>
int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) return 2;
    FILE *in = fopen(argv[1],"rb"), *out = fopen(argv[2],"w");
    if (!in || !out) { if (in) fclose(in); if (out) fclose(out); return 2; }
    fprintf(out,"static const char %s[] = {\n",argc==4?argv[3]:"vx_runtime_text");
    int c, count = 0;
    while ((c = fgetc(in)) != EOF) {
        if (c == '\r') continue;
        fprintf(out,"%u,",(unsigned)c);
        if (count % 32 == 31) fputc('\n',out);
        ++count;
    }
    fputs("0};\n",out);
    int status = !count || ferror(in) || ferror(out); fclose(in);
    if (fclose(out)) status = 1;
    return status;
}
