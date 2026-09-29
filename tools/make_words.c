#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
    FILE *file = fopen("app/src/main/assets/words.bin", "wb");
    if (!file) return 1;
    const uint32_t count = (uint32_t)(sizeof(words) / sizeof(words[0]));
    int ok = fwrite("DSWB", 1, 4, file) == 4;
    ok = ok && write_u32(file, 1);
    ok = ok && write_u32(file, count);
    for (uint32_t i = 0; ok && i < count; ++i) {
        const size_t length = strlen(words[i]);
        ok = write_u16(file, (uint16_t)length) && fwrite(words[i], 1, length, file) == length;
    }
    fclose(file);
    return ok ? 0 : 1;
}
