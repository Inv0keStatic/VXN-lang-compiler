/* Image-output bridge for examples that only have scalar output().
   Input: decimal width, height, 255, then top-to-bottom RGB samples.
   Output: an uncompressed 24-bit BMP. No rendering happens in this helper.
   Usage: rendering_program | vxn_bitmap OUTPUT.bmp */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int read_number(unsigned maximum, unsigned *result) {
    int c;
    do { c = getchar(); } while (c != EOF && isspace((unsigned char)c));
    if (c < '0' || c > '9') return 0;
    unsigned value = 0;
    do {
        unsigned digit = (unsigned)(c - '0');
        if (value > maximum / 10 ||
            (value == maximum / 10 && digit > maximum % 10)) return 0;
        value = value * 10 + digit;
        c = getchar();
    } while (c >= '0' && c <= '9');
    if (c != EOF && !isspace((unsigned char)c)) return 0;
    if (ferror(stdin)) return 0;
    *result = value;
    return 1;
}

static void put_u16(unsigned char *p, uint16_t value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void put_u32(unsigned char *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (i * 8));
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fputs("usage: rendering_program | vxn_bitmap OUTPUT.bmp\n", stderr);
        return 1;
    }
    unsigned width, height, maximum;
    if (!read_number(8192, &width) || !width ||
        !read_number(8192, &height) || !height ||
        !read_number(255, &maximum) || maximum != 255) {
        fputs("expected width/height in 1..8192 and maximum sample 255\n", stderr);
        return 1;
    }

    uint32_t stride = ((uint32_t)width * 3 + 3) & ~UINT32_C(3);
    uint32_t image_size = stride * height;
    unsigned char header[54] = {0};
    header[0] = 'B'; header[1] = 'M';
    put_u32(header + 2, 54 + image_size);
    put_u32(header + 10, 54);
    put_u32(header + 14, 40);
    put_u32(header + 18, width);
    /* A negative signed height marks a top-down Windows bitmap. */
    put_u32(header + 22, UINT32_C(0) - height);
    put_u16(header + 26, 1);
    put_u16(header + 28, 24);
    put_u32(header + 34, image_size);

    unsigned char *row = calloc(stride, 1);
    if (!row) { fputs("cannot allocate image row\n", stderr); return 1; }
    FILE *out = fopen(argv[1], "wb");
    if (!out) { perror(argv[1]); free(row); return 1; }

    int ok = fwrite(header, 1, sizeof header, out) == sizeof header;
    for (unsigned y = 0; ok && y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            unsigned red, green, blue;
            if (!read_number(255, &red) || !read_number(255, &green) ||
                !read_number(255, &blue)) {
                fputs("missing or invalid RGB sample\n", stderr);
                ok = 0;
                break;
            }
            row[x * 3] = (unsigned char)blue;
            row[x * 3 + 1] = (unsigned char)green;
            row[x * 3 + 2] = (unsigned char)red;
        }
        if (ok) ok = fwrite(row, 1, stride, out) == stride;
    }
    if (ok) {
        int c;
        do { c = getchar(); } while (c != EOF && isspace((unsigned char)c));
        if (c != EOF || ferror(stdin)) {
            fputs("unexpected data after the final pixel\n", stderr);
            ok = 0;
        }
    }
    if (fclose(out)) ok = 0;
    free(row);
    if (!ok) {
        fputs("image was not completed\n", stderr);
        return 1;
    }
    return 0;
}
