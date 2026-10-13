#include "../app/src/main/cpp/typography.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static void check(bool condition, const char *msg) {
    if (!condition) {
        fprintf(stderr, "Assertion failed: %s\n", msg);
        abort();
    }
}

int main(void) {
    /* 1. Verify glyph indexing for alphabets */
    for (char c = 'A'; c <= 'Z'; ++c) {
        int idx_upper = ds_glyph_index(c);
        int idx_lower = ds_glyph_index((char)(c + 32));
        check(idx_upper == c - 'A', "uppercase glyph match");
        check(idx_lower == idx_upper, "lowercase glyph match");
    }

    /* 2. Verify glyph indexing for numeric digits */
    for (char c = '0'; c <= '9'; ++c) {
        int idx = ds_glyph_index(c);
        check(idx == 26 + (c - '0'), "digit glyph match");
    }

    /* 3. Verify glyph indexing for punctuation symbols */
    const char puncts[] = ":-.+()%!?/'\"_#&*[]<>=;~";
    for (size_t i = 0; puncts[i] != '\0'; ++i) {
        int idx = ds_glyph_index(puncts[i]);
        check(idx >= 36 && idx < DS_FONT_GLYPH_COUNT, "punctuation in range");
    }

    /* 4. Verify invalid / out-of-range characters return -1 */
    check(ds_glyph_index(' ') == -1, "space has no glyph");
    check(ds_glyph_index('\n') == -1, "newline has no glyph");
    check(ds_glyph_index('\t') == -1, "tab has no glyph");
    check(ds_glyph_index('@') == -1, "at symbol has no glyph");
    check(ds_glyph_index('{') == -1, "brace has no glyph");
    check(ds_glyph_index('}') == -1, "brace has no glyph");

    /* 5. Verify all glyph rows conform to 5-bit bounds */
    for (int g = 0; g < DS_FONT_GLYPH_COUNT; ++g) {
        for (int r = 0; r < 7; ++r) {
            check(ds_font_bitmap[g][r] <= 0x1F, "glyph row width <= 5 bits");
        }
    }

    /* 6. Verify font advances across styles */
    const float scale = 2.0f;
    check(fabsf(ds_text_char_advance('A', scale, DS_FONT_STANDARD) - 12.0f) < 1e-4f, "standard advance");
    check(fabsf(ds_text_char_advance('A', scale, DS_FONT_MONO) - 12.0f) < 1e-4f, "mono advance");
    check(fabsf(ds_text_char_advance('A', scale, DS_FONT_BOLD) - 12.4f) < 1e-4f, "bold advance");
    check(fabsf(ds_text_char_advance('A', scale, DS_FONT_CONDENSED) - 10.0f) < 1e-4f, "condensed advance");

    /* 7. Verify text width calculation */
    const char *sample = "DEADSPEAK";
    size_t len = strlen(sample);
    check(fabsf(ds_text_width(sample, 1.0f, DS_FONT_STANDARD) - (float)len * 6.0f) < 1e-4f, "standard width");
    check(fabsf(ds_text_width(sample, 1.0f, DS_FONT_CONDENSED) - (float)len * 5.0f) < 1e-4f, "condensed width");
    check(fabsf(ds_text_width(NULL, 1.0f, DS_FONT_STANDARD)) < 1e-4f, "null string width");

    /* 8. Verify fit scale behavior */
    float fit_unconstrained = ds_fit_text_scale_styled("TEST", 2.0f, 500.0f, DS_FONT_STANDARD);
    check(fabsf(fit_unconstrained - 2.0f) < 1e-4f, "unconstrained fit");

    float fit_constrained = ds_fit_text_scale_styled("TEST", 2.0f, 12.0f, DS_FONT_STANDARD);
    /* 4 chars * 6.0f = 24.0f unit width; available is 12.0f; fit = 12 / 24 = 0.5f */
    check(fabsf(fit_constrained - 0.5f) < 1e-4f, "constrained fit");

    /* 9. Verify typographic hierarchy scales */
    check(fabsf(ds_sp_to_scale(DS_FONT_SIZE_DISPLAY_XL) - (48.0f / 7.0f)) < 1e-4f, "display xl scale");
    check(fabsf(ds_sp_to_scale(DS_FONT_SIZE_DISPLAY_LG) - (34.0f / 7.0f)) < 1e-4f, "display lg scale");
    check(fabsf(ds_sp_to_scale(DS_FONT_SIZE_TITLE_LG) - (24.0f / 7.0f)) < 1e-4f, "title lg scale");
    check(fabsf(ds_sp_to_scale(DS_FONT_SIZE_TITLE_MD) - (20.0f / 7.0f)) < 1e-4f, "title md scale");
    check(fabsf(ds_sp_to_scale(DS_FONT_SIZE_BODY_LG) - (16.0f / 7.0f)) < 1e-4f, "body lg scale");
    check(fabsf(ds_sp_to_scale(DS_FONT_SIZE_BODY_MD) - (14.0f / 7.0f)) < 1e-4f, "body md scale");
    check(fabsf(ds_sp_to_scale(DS_FONT_SIZE_CAPTION) - (12.0f / 7.0f)) < 1e-4f, "caption scale");
    check(fabsf(ds_sp_to_scale(DS_FONT_SIZE_BADGE) - (10.0f / 7.0f)) < 1e-4f, "badge scale");
    check(fabsf(ds_sp_to_scale(DS_FONT_SIZE_MICRO) - (8.0f / 7.0f)) < 1e-4f, "micro scale");

    printf("PASS: typography glyph index, punctuation set, font variants, advances, and typographic scale\n");
    return 0;
}
