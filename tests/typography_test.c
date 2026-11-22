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
        auto idx_upper = ds_glyph_index(c);
        auto idx_lower = ds_glyph_index((char)(c + 32));
        check(idx_upper == c - 'A', "uppercase glyph match");
        check(idx_lower == idx_upper, "lowercase glyph match");
    }

    /* 2. Verify glyph indexing for numeric digits */
    for (char c = '0'; c <= '9'; ++c) {
        auto idx = ds_glyph_index(c);
        check(idx == 26 + (c - '0'), "digit glyph match");
    }

    /* 3. Verify glyph indexing for punctuation symbols */
    constexpr char puncts[] = ":-.+()%!?/'\"_#&*[]<>=;~";
    for (size_t i = 0; puncts[i] != '\0'; ++i) {
        auto idx = ds_glyph_index(puncts[i]);
        check(idx >= 36 && idx < (int)DS_FONT_GLYPH_COUNT, "punctuation in range");
    }

    /* 4. Verify invalid / out-of-range characters return -1 */
    check(ds_glyph_index(' ') == -1, "space has no glyph");
    check(ds_glyph_index('\n') == -1, "newline has no glyph");
    check(ds_glyph_index('\t') == -1, "tab has no glyph");
    check(ds_glyph_index('@') == -1, "at symbol has no glyph");
    check(ds_glyph_index('{') == -1, "brace has no glyph");
    check(ds_glyph_index('}') == -1, "brace has no glyph");

    /* 5. Verify all glyph rows conform to 5-bit bounds */
    for (size_t g = 0; g < DS_FONT_GLYPH_COUNT; ++g) {
        for (size_t r = 0; r < DS_FONT_GLYPH_ROWS; ++r) {
            check(ds_font_bitmap[g][r] <= 0x1F, "glyph row width <= 5 bits");
        }
    }

    /* 6. Verify font atlas unpacking */
    uint8_t *unpacked = malloc(DS_FONT_ATLAS_RAW_SIZE);
    check(unpacked != nullptr, "atlas memory allocation");
    if (!unpacked) abort();
    ds_font_unpack_atlas(unpacked, DS_FONT_ATLAS_RAW_SIZE);
    bool has_non_zero = false;
    for (size_t i = 0; i < DS_FONT_ATLAS_RAW_SIZE; ++i) {
        if (unpacked[i] > 0) { has_non_zero = true; break; }
    }
    check(has_non_zero, "unpacked atlas contains font data");
    free(unpacked);

    /* 7. Verify font advances across styles */
    constexpr float scale = 2.0f;
    const float adv_std = ds_text_char_advance('A', scale, DS_FONT_STANDARD);
    const float adv_bold = ds_text_char_advance('A', scale, DS_FONT_BOLD);
    const float adv_cond = ds_text_char_advance('A', scale, DS_FONT_CONDENSED);
    const float adv_mono = ds_text_char_advance('A', scale, DS_FONT_MONO);
    check(adv_std > 0.0f, "standard advance positive");
    check(adv_bold > adv_std, "bold advance > standard advance");
    check(adv_cond < adv_std, "condensed advance < standard advance");
    check(adv_mono > 0.0f, "mono advance positive");

    /* 8. Verify monospace and proportional properties */
    const float adv_digit_0 = ds_text_char_advance('0', scale, DS_FONT_MONO);
    const float adv_digit_9 = ds_text_char_advance('9', scale, DS_FONT_MONO);
    check(fabsf(adv_digit_0 - adv_digit_9) < 1e-4f, "mono digits have identical advance");

    const float adv_W = ds_text_char_advance('W', scale, DS_FONT_STANDARD);
    const float adv_I = ds_text_char_advance('I', scale, DS_FONT_STANDARD);
    check(adv_W > adv_I, "proportional W > I advance");

    /* 9. Verify text width calculation */
    const char *sample = "DEADSPEAK";
    const float width_std = ds_text_width(sample, 1.0f, DS_FONT_STANDARD);
    const float width_cond = ds_text_width(sample, 1.0f, DS_FONT_CONDENSED);
    check(width_std > 0.0f, "standard width positive");
    check(width_cond < width_std, "condensed width < standard width");
    check(fabsf(ds_text_width(nullptr, 1.0f, DS_FONT_STANDARD)) < 1e-4f, "nullptr string width");

    /* 10. Verify fit scale behavior */
    auto fit_unconstrained = ds_fit_text_scale_styled("TEST", 2.0f, 500.0f, DS_FONT_STANDARD);
    check(fabsf(fit_unconstrained - 2.0f) < 1e-4f, "unconstrained fit");

    auto fit_constrained = ds_fit_text_scale_styled("TEST", 2.0f, 10.0f, DS_FONT_STANDARD);
    check(fit_constrained < 2.0f && fit_constrained > 0.0f, "constrained fit scales down");
    check(fabsf(ds_fit_text_scale_styled(nullptr, 2.0f, 100.0f, DS_FONT_STANDARD) - 2.0f) < 1e-4f, "nullptr fit scale");

    /* 11. Verify typographic hierarchy scales */
    check(fabsf(ds_sp_to_scale(DS_FONT_SIZE_DISPLAY_XL) - (48.0f / 7.0f)) < 1e-4f, "display xl scale");
    check(fabsf(ds_sp_to_scale(DS_FONT_SIZE_DISPLAY_LG) - (34.0f / 7.0f)) < 1e-4f, "display lg scale");
    check(fabsf(ds_sp_to_scale(DS_FONT_SIZE_TITLE_LG) - (24.0f / 7.0f)) < 1e-4f, "title lg scale");
    check(fabsf(ds_sp_to_scale(DS_FONT_SIZE_TITLE_MD) - (20.0f / 7.0f)) < 1e-4f, "title md scale");
    check(fabsf(ds_sp_to_scale(DS_FONT_SIZE_BODY_LG) - (16.0f / 7.0f)) < 1e-4f, "body lg scale");
    check(fabsf(ds_sp_to_scale(DS_FONT_SIZE_BODY_MD) - (14.0f / 7.0f)) < 1e-4f, "body md scale");
    check(fabsf(ds_sp_to_scale(DS_FONT_SIZE_CAPTION) - (12.0f / 7.0f)) < 1e-4f, "caption scale");
    check(fabsf(ds_sp_to_scale(DS_FONT_SIZE_BADGE) - (10.0f / 7.0f)) < 1e-4f, "badge scale");
    check(fabsf(ds_sp_to_scale(DS_FONT_SIZE_MICRO) - (8.0f / 7.0f)) < 1e-4f, "micro scale");

    printf("PASS: C23 typography font atlas, constexpr scale, nullptr guards, attributes, and font engine\n");
    return 0;
}
