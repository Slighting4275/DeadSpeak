#ifndef DEADSPEAK_CUSTOM_WORDS_H
#define DEADSPEAK_CUSTOM_WORDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DS_CUSTOM_WORDS_MAX_COUNT 256u
#define DS_CUSTOM_WORDS_MAX_LENGTH 31u
#define DS_CUSTOM_WORDS_BLOB_HEADER_SIZE 12u
#define DS_CUSTOM_WORDS_BLOB_MAX_SIZE \
    (DS_CUSTOM_WORDS_BLOB_HEADER_SIZE + DS_CUSTOM_WORDS_MAX_COUNT * (2u + DS_CUSTOM_WORDS_MAX_LENGTH))

typedef enum {
    DS_CUSTOM_WORDS_ERROR_NONE = 0,
    DS_CUSTOM_WORDS_ERROR_INVALID_ARGUMENT,
    DS_CUSTOM_WORDS_ERROR_INVALID_WORD,
    DS_CUSTOM_WORDS_ERROR_WORD_TOO_LONG,
    DS_CUSTOM_WORDS_ERROR_DUPLICATE,
    DS_CUSTOM_WORDS_ERROR_FULL,
    DS_CUSTOM_WORDS_ERROR_NOT_FOUND,
    DS_CUSTOM_WORDS_ERROR_IO,
    DS_CUSTOM_WORDS_ERROR_MALFORMED,
    DS_CUSTOM_WORDS_ERROR_SYMLINK,
    DS_CUSTOM_WORDS_ERROR_BUFFER_TOO_SMALL,
    DS_CUSTOM_WORDS_ERROR_DURABILITY_UNCONFIRMED
} DSCustomWordsError;

/*
 * Fixed-capacity, user-owned bank. Initialize it before any other operation.
 * Words are stored as uppercase ASCII; supported separators are spaces,
 * hyphens, and apostrophes. No bundled words are inserted by this module.
 */
typedef struct {
    uint32_t count;
    char words[DS_CUSTOM_WORDS_MAX_COUNT][DS_CUSTOM_WORDS_MAX_LENGTH + 1u];
    DSCustomWordsError last_error;
} DSCustomWords;

void ds_custom_words_init(DSCustomWords *bank);

/* A missing file successfully loads an empty bank. Malformed input or I/O
 * errors leave the existing words untouched; last_error describes the result.
 */
bool ds_custom_words_load(DSCustomWords *bank, const char *path);

/* Add normalizes ASCII a-z to A-Z and persists atomically. Duplicate detection
 * is performed on the normalized spelling. Failed writes leave the bank as-is.
 */
bool ds_custom_words_add(DSCustomWords *bank, const char *path, const char *text);

/* Remove by zero-based index and persist atomically. An invalid index changes
 * neither the in-memory bank nor its file.
 */
bool ds_custom_words_remove(DSCustomWords *bank, const char *path, size_t index);

size_t ds_custom_words_count(const DSCustomWords *bank);
const char *ds_custom_words_word(const DSCustomWords *bank, size_t index);
DSCustomWordsError ds_custom_words_error(const DSCustomWords *bank);
const char *ds_custom_words_error_message(const DSCustomWords *bank);

/* Encode engine.zig's DSWB version-1 format: magic, LE32 version, LE32 count,
 * then LE16 byte length + ASCII bytes per entry. Empty banks produce a valid
 * 12-byte zero-count blob, which the engine treats as having no selectable
 * word. Returns bytes written, or 0 on error; output is not modified if too
 * small. Caller must provide a non-overlapping output buffer.
 */
size_t ds_custom_words_blob(DSCustomWords *bank, uint8_t *output, size_t capacity);

#endif
