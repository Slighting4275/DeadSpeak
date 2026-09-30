#include <stdint.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define MAX_WORDS 6000
#define WORD_BYTES 32

static const char *words[] = {
    "MORROW", "HOLLOW", "NORTH", "EMBER", "STILL", "BELOW", "THRESHOLD", "RIVER",
    "ASH", "LISTEN", "LANTERN", "ECHO", "ROOM", "WINTER", "RETURN", "QUIET",
    "ORCHARD", "DUST", "DOOR", "REMEMBER", "WATCH", "CANDLE", "NIGHT", "WEST",
    "BENEATH", "HOME", "VAULT", "RAIN", "SILENCE", "CLOSE", "WITNESS", "TIDE",
    /* English names */
    "ALEX", "JORDAN", "RILEY", "MORGAN", "JAMIE", "TAYLOR", "SAM", "CASEY",
    /* Spanish */
    "HOLA", "CASA", "LUZ", "AGUA", "NOCHE", "MARIA", "JOSE", "ELENA",
    /* French */
    "BONJOUR", "MAISON", "LUMIERE", "EAU", "NUIT", "MARIE", "JEAN", "LOUISE",
    /* German */
    "HALLO", "HAUS", "LICHT", "WASSER", "NACHT", "ANNA", "HANS", "LENA",
    /* Italian */
    "CIAO", "CASA", "LUCE", "ACQUA", "NOTTE", "GIULIA", "MARCO", "SOFIA",
    /* Portuguese */
    "OLA", "CASA", "LUZ", "AGUA", "NOITE", "JOAO", "ANA", "PEDRO",
    /* Dutch */
    "HALLO", "HUIS", "LICHT", "WATER", "NACHT", "EVA", "JAN", "NOOR",
    /* Polish */
    "CZESC", "DOM", "SWIATLO", "WODA", "NOC", "ANNA", "JAN", "EWA",
    /* Russian transliteration */
    "PRIVET", "DOM", "SVET", "VODA", "NOCH", "MARIA", "IVAN", "OLGA",
    /* Ukrainian transliteration */
    "PRYVIT", "DIM", "SVIT", "VODA", "NICH", "OLENA", "IVAN", "SOFIA",
    /* Japanese romaji */
    "KONNICHIWA", "IE", "HIKARI", "MIZU", "YORU", "YUKI", "HARU", "AKIRA",
    /* Korean romanization */
    "ANNYEONG", "JIP", "BIT", "MUL", "BAM", "MINJI", "JISU", "JIHO",
    /* Mandarin pinyin */
    "NIHAO", "JIA", "GUANG", "SHUI", "YE", "MEI", "LI", "WEI",
    /* Arabic transliteration */
    "MARHABA", "BAYT", "NUR", "MA", "LAYL", "AMINA", "OMAR", "LAYLA",
    /* Hindi transliteration */
    "NAMASTE", "GHAR", "ROSHNI", "PAANI", "RAAT", "PRIYA", "ARJUN", "KIRAN",
    /* Greek transliteration */
    "YASOU", "SPITI", "FOS", "NERO", "NYCHTA", "ELENI", "NIKOS", "MARIA",
    /* Turkish */
    "MERHABA", "EV", "ISIK", "SU", "GECE", "AYSE", "MEHMET", "DENIZ",
    /* Swedish */
    "HEJ", "HEM", "LJUS", "VATTEN", "NATT", "ANNA", "ERIK", "ASTRID",
    /* Norwegian */
    "HEI", "HJEM", "LYS", "VANN", "NATT", "OLE", "INGRID", "LARS",
    /* Finnish */
    "HEI", "KOTI", "VALO", "VESI", "YO", "ANNA", "MIKKO", "AINO",
    /* Hebrew transliteration */
    "SHALOM", "BAYIT", "OR", "MAYIM", "LAYLA", "DAVID", "MIRIAM", "NOAM",
    /* Romanian */
    "BUNA", "CASA", "LUMINA", "APA", "NOAPTE", "MARIA", "ION", "ELENA",
    /* Indonesian */
    "HALO", "RUMAH", "CAHAYA", "AIR", "MALAM", "SITI", "BUDI", "PUTRI",
};

static int write_u16(FILE *file, uint16_t value) {
    return fputc(value & 0xff, file) != EOF && fputc((value >> 8) & 0xff, file) != EOF;
}

static int write_u32(FILE *file, uint32_t value) {
    return write_u16(file, (uint16_t)(value & 0xffff)) && write_u16(file, (uint16_t)(value >> 16));
}

int main(void) {
    char entries[MAX_WORDS][WORD_BYTES];
    uint32_t count = 0;
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        bool duplicate = false;
        for (uint32_t n = 0; n < count; ++n) {
            if (strcmp(entries[n], words[i]) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate && count < MAX_WORDS) snprintf(entries[count++], WORD_BYTES, "%s", words[i]);
    }

    FILE *source = fopen("tools/data/english-common.txt", "r");
    if (!source) {
        fprintf(stderr, "Could not open tools/data/english-common.txt\n");
        return 1;
    }
    char line[128];
    while (count < MAX_WORDS && fgets(line, sizeof(line), source)) {
        size_t length = strcspn(line, "\r\n");
        if (length < 3 || length >= WORD_BYTES) continue;
        char normalized[WORD_BYTES];
        bool valid = true;
        for (size_t i = 0; i < length; ++i) {
            const unsigned char c = (unsigned char)line[i];
            if (!isalpha(c) || c > 127) {
                valid = false;
                break;
            }
            normalized[i] = (char)toupper(c);
        }
        if (!valid) continue;
        normalized[length] = '\0';
        bool duplicate = false;
        for (uint32_t i = 0; i < count; ++i) {
            if (strcmp(entries[i], normalized) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) snprintf(entries[count++], WORD_BYTES, "%s", normalized);
    }
    fclose(source);

    FILE *file = fopen("app/src/main/assets/words.bin", "wb");
    if (!file) return 1;
    int ok = fwrite("DSWB", 1, 4, file) == 4;
    ok = ok && write_u32(file, 1);
    ok = ok && write_u32(file, count);
    for (uint32_t i = 0; ok && i < count; ++i) {
        const size_t length = strlen(entries[i]);
        ok = write_u16(file, (uint16_t)length) && fwrite(entries[i], 1, length, file) == length;
    }
    fclose(file);
    if (ok) fprintf(stderr, "Wrote %u unique dictionary entries\n", count);
    return ok ? 0 : 1;
}
