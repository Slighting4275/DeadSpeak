/* Native APK Signature Scheme v2 signer/verifier (Android 11+).
 * Build: zig cc -std=c23 -O2 -Wall -Wextra -Werror tools/apk_sign.c -lcrypto -o apk-sign
 * Usage: apk-sign sign input.apk output.apk key.pem cert.pem
 *        apk-sign verify signed.apk
 * Strict ZIP32, one v2 signer, one RSA/PKCS1-v1_5 SHA-256 algorithm (0x0103),
 * one certificate, no attributes/other signing schemes. No v1 downgrade.
 * verify checks integrity and embedded signer identity, not external trust.
 * Private keys must be regular, owned by the effective user, have no group or
 * other permission bits, and not be symlinks. Public certificates may be 0644.
 * ZIP framing is checked; compressed payloads/entry CRCs are not decompressed.
 * Specification: https://source.android.com/docs/security/features/apksigning/v2
 */
#define _POSIX_C_SOURCE 200809L
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define V2_ID UINT32_C(0x7109871a)
#define RSA_SHA256 UINT32_C(0x0103)
#define CHUNK_SIZE ((size_t)1048576)
#define MAX_SIGNING_SIZE ((size_t)16777216)
static const unsigned char magic[] = "APK Sig Block 42";

typedef struct { const unsigned char *p; size_t n; } Slice;
typedef struct { unsigned char *p; size_t n, cap; } Buffer;
typedef struct {
    Buffer file;
    size_t cd, cd_size, eocd, block;
    uint16_t entries;
    Slice v2;
    bool signed_apk;
} Apk;
typedef struct { size_t begin, end; Slice name; } ZipEntry;

static bool fail(const char *message) {
    fprintf(stderr, "apk-sign: %s\n", message);
    return false;
}
static uint16_t u16(const unsigned char *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t u64(const unsigned char *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }
static void put32(unsigned char *p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(n >> (i * 8));
}
static void put64(unsigned char *p, uint64_t n) {
    put32(p, (uint32_t)n); put32(p + 4, (uint32_t)(n >> 32));
}
static bool equal(Slice a, Slice b) { return a.n == b.n && !memcmp(a.p, b.p, a.n); }
static void release(Buffer *b) { free(b->p); *b = (Buffer){0}; }

static bool append(Buffer *b, const void *p, size_t n) {
    if (n > MAX_SIGNING_SIZE || b->n > MAX_SIGNING_SIZE - n) return fail("signing metadata exceeds limit");
    const size_t required = b->n + n;
    if (required > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < required) cap *= 2;
        unsigned char *new_data = realloc(b->p, cap);
        if (!new_data) return fail("out of memory");
        b->p = new_data; b->cap = cap;
    }
    if (n) memcpy(b->p + b->n, p, n);
    b->n += n;
    return true;
}
static bool append32(Buffer *b, uint32_t n) { unsigned char p[4]; put32(p, n); return append(b, p, 4); }
static bool append64(Buffer *b, uint64_t n) { unsigned char p[8]; put64(p, n); return append(b, p, 8); }
static Slice view(const Buffer *b) { return (Slice){b->p, b->n}; }
static bool lp(Buffer *b, Slice s) { return s.n <= UINT32_MAX && append32(b, (uint32_t)s.n) && append(b, s.p, s.n); }
static bool take(Slice *s, size_t n, Slice *out) {
    if (n > s->n) return false;
    *out = (Slice){s->p, n}; s->p += n; s->n -= n;
    return true;
}
static bool take_lp(Slice *s, Slice *out) {
    Slice length;
    return take(s, 4, &length) && take(s, u32(length.p), out);
}

static bool read_file(const char *path, Buffer *out) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return fail("cannot open APK input");
    struct stat st;
    bool ok = false;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 22 || (uint64_t)st.st_size > UINT32_MAX) {
        fail("input must be a regular ZIP32 APK (22 bytes to less than 4 GiB)"); goto done;
    }
    out->n = (size_t)st.st_size;
    out->p = malloc(out->n);
    if (!out->p) { fail("out of memory reading APK"); goto done; }
    size_t offset = 0;
    while (offset < out->n) {
        const size_t count = out->n - offset > CHUNK_SIZE ? CHUNK_SIZE : out->n - offset;
        const ssize_t got = read(fd, out->p + offset, count);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { fail("cannot read complete APK"); goto done; }
        offset += (size_t)got;
    }
    unsigned char extra;
    ssize_t got;
    do { got = read(fd, &extra, 1); } while (got < 0 && errno == EINTR);
    if (got != 0) { fail("APK changed size while reading"); goto done; }
    ok = true;
done:
    close(fd);
    if (!ok) release(out);
    return ok;
}

static bool extra_valid(Slice extra, bool local_padding) {
    while (extra.n) {
        /* Native zipalign appends zero bytes to local extra fields, sometimes
         * ending with fewer than four bytes. Central extras stay strict TLVs. */
        if (local_padding && extra.n < 4) {
            for (size_t i = 0; i < extra.n; ++i) if (extra.p[i]) return false;
            return true;
        }
        Slice header, value;
        if (!take(&extra, 4, &header) || u16(header.p) == 1 || !take(&extra, u16(header.p + 2), &value))
            return false;
    }
    return true;
}
static bool ascii_equal(Slice s, const char *value) {
    const size_t length = strlen(value);
    if (s.n != length) return false;
    for (size_t i = 0; i < length; ++i)
        if (toupper((unsigned char)s.p[i]) != (unsigned char)value[i]) return false;
    return true;
}
static bool v1_name(Slice name) {
    if (name.n < 9 || !ascii_equal((Slice){name.p, 9}, "META-INF/")) return false;
    Slice basename = {name.p + 9, name.n - 9};
    if (memchr(basename.p, '/', basename.n)) return false;
    if (basename.n >= 4 && ascii_equal((Slice){basename.p, 4}, "SIG-")) return true;
    static const char *const suffixes[] = {".SF", ".RSA", ".DSA", ".EC"};
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        const size_t length = strlen(suffixes[i]);
        if (basename.n >= length && ascii_equal((Slice){basename.p + basename.n - length, length}, suffixes[i])) return true;
    }
    return false;
}
static int compare_offsets(const void *left, const void *right) {
    const ZipEntry *a = left, *b = right;
    return a->begin < b->begin ? -1 : a->begin > b->begin;
}
static int compare_names(const void *left, const void *right) {
    const ZipEntry *a = left, *b = right;
    const size_t common = a->name.n < b->name.n ? a->name.n : b->name.n;
    const int compared = memcmp(a->name.p, b->name.p, common);
    return compared ? compared : a->name.n < b->name.n ? -1 : a->name.n > b->name.n;
}

/* Validate all central/local records, including descriptor bounds and extras.
 * Requiring complete coverage of section 1 rejects hidden/stale signing blocks,
 * prepended archives and overlapping entries without inspecting payload content. */
static bool zip_records(const Apk *apk) {
    const unsigned char *data = apk->file.p;
    const size_t limit = apk->block;
    Slice cd = {data + apk->cd, apk->cd_size};
    ZipEntry *entries = calloc(apk->entries, sizeof(*entries));
    if (!entries) return fail("out of memory validating ZIP");
    bool ok = false;
    for (uint16_t i = 0; i < apk->entries; ++i) {
        Slice central, name, extra, comment;
        if (!take(&cd, 46, &central) || u32(central.p) != UINT32_C(0x02014b50)) goto malformed;
        const unsigned char *c = central.p;
        const uint16_t flags = u16(c + 8), method = u16(c + 10);
        const uint32_t compressed = u32(c + 20), uncompressed = u32(c + 24), offset = u32(c + 42);
        if (u16(c + 6) > 20 || u16(c + 34) != 0 || (flags & ~UINT16_C(0x080e)) ||
            (method != 0 && method != 8) || compressed == UINT32_MAX || uncompressed == UINT32_MAX ||
            offset == UINT32_MAX || !take(&cd, u16(c + 28), &name) || !name.n || memchr(name.p, 0, name.n) ||
            !take(&cd, u16(c + 30), &extra) || !extra_valid(extra, false) || !take(&cd, u16(c + 32), &comment)) goto malformed;
        if (v1_name(name)) { fail("existing v1 signature is forbidden in a v2-only APK"); goto done; }
        if (offset > limit || limit - offset < 30) goto malformed;
        const unsigned char *local = data + offset;
        const size_t local_name_length = u16(local + 26), local_extra_length = u16(local + 28);
        const size_t header_length = 30 + local_name_length + local_extra_length;
        if (u32(local) != UINT32_C(0x04034b50) || u16(local + 4) > 20 || u16(local + 6) != flags ||
            u16(local + 8) != method || memcmp(local + 10, c + 12, 4) || header_length > limit - offset ||
            !equal((Slice){local + 30, local_name_length}, name) ||
            !extra_valid((Slice){local + 30 + local_name_length, local_extra_length}, true) ||
            compressed > limit - offset - header_length || (method == 0 && compressed != uncompressed)) goto malformed;
        size_t end = offset + header_length + compressed;
        const uint32_t crc = u32(c + 16);
        if (flags & 8) {
            if ((u32(local + 14) && u32(local + 14) != crc) ||
                (u32(local + 18) && u32(local + 18) != compressed) ||
                (u32(local + 22) && u32(local + 22) != uncompressed)) goto malformed;
            const unsigned char *descriptor = data + end;
            if (limit - end >= 16 && u32(descriptor) == UINT32_C(0x08074b50) &&
                u32(descriptor + 4) == crc && u32(descriptor + 8) == compressed && u32(descriptor + 12) == uncompressed) {
                end += 16;
            } else if (limit - end >= 12 && u32(descriptor) == crc &&
                       u32(descriptor + 4) == compressed && u32(descriptor + 8) == uncompressed) {
                end += 12;
            } else goto malformed;
        } else if (u32(local + 14) != crc || u32(local + 18) != compressed || u32(local + 22) != uncompressed) goto malformed;
        entries[i] = (ZipEntry){offset, end, name};
    }
    if (cd.n) goto malformed;
    qsort(entries, apk->entries, sizeof(*entries), compare_offsets);
    size_t end = 0;
    for (uint16_t i = 0; i < apk->entries; ++i) {
        if (entries[i].begin != end) goto malformed;
        end = entries[i].end;
    }
    if (end != limit) goto malformed;
    qsort(entries, apk->entries, sizeof(*entries), compare_names);
    for (uint16_t i = 1; i < apk->entries; ++i) if (equal(entries[i - 1].name, entries[i].name)) goto malformed;
    ok = true;
    goto done;
malformed:
    fail("malformed/unsupported ZIP32 records (ZIP64, encryption, gaps and duplicate entries are rejected)");
done:
    free(entries);
    return ok;
}

static bool parse_apk(Apk *apk, bool require_signed) {
    const unsigned char *data = apk->file.p;
    const size_t size = apk->file.n;
    size_t candidates = 0;
    const size_t lower = size > 65557 ? size - 65557 : 0;
    for (size_t position = size - 22;; --position) {
        if (u32(data + position) == UINT32_C(0x06054b50) && u16(data + position + 20) == size - position - 22) {
            apk->eocd = position; ++candidates;
        }
        if (position == lower) break;
    }
    if (candidates != 1) return fail("missing or ambiguous canonical EOCD; trailing bytes are forbidden");
    const unsigned char *eocd = data + apk->eocd;
    apk->entries = u16(eocd + 10);
    apk->cd_size = u32(eocd + 12); apk->cd = u32(eocd + 16);
    if (u16(eocd + 4) || u16(eocd + 6) || u16(eocd + 8) != apk->entries ||
        !apk->entries || apk->entries == UINT16_MAX || apk->cd_size == UINT32_MAX || apk->cd == UINT32_MAX ||
        apk->cd > apk->eocd || apk->cd_size != apk->eocd - apk->cd)
        return fail("unsupported EOCD: require nonempty single-disk ZIP32 and adjacent central directory");
    apk->block = apk->cd;
    apk->signed_apk = apk->cd >= 24 && !memcmp(data + apk->cd - 16, magic, 16);
    if (apk->signed_apk) {
        if (!require_signed) return fail("input already contains an APK signing block; refusing to re-sign");
        const uint64_t block_size = u64(data + apk->cd - 24);
        if (block_size < 24 || block_size > MAX_SIGNING_SIZE || block_size > apk->cd - 8)
            return fail("invalid signing block size");
        apk->block = apk->cd - (size_t)block_size - 8;
        if (u64(data + apk->block) != block_size) return fail("signing block size fields disagree");
        Slice pairs = {data + apk->block + 8, (size_t)block_size - 24};
        Slice header, pair;
        if (!take(&pairs, 8, &header) || u64(header.p) < 4 || u64(header.p) > pairs.n ||
            !take(&pairs, (size_t)u64(header.p), &pair) || u32(pair.p) != V2_ID || pairs.n)
            return fail("require exactly one v2 signing-block pair");
        apk->v2 = (Slice){pair.p + 4, pair.n - 4};
    } else if (require_signed) return fail("v2 signing block missing; no downgrade permitted");
    return zip_records(apk);
}

static bool content_digest(const Apk *apk, unsigned char result[32]) {
    const size_t eocd_size = apk->file.n - apk->eocd;
    unsigned char *eocd = malloc(eocd_size);
    EVP_MD_CTX *root = EVP_MD_CTX_new(), *chunk = EVP_MD_CTX_new();
    bool ok = false;
    if (!eocd || !root || !chunk) { fail("out of memory hashing APK"); goto done; }
    memcpy(eocd, apk->file.p + apk->eocd, eocd_size);
    put32(eocd + 16, (uint32_t)apk->block);
    const Slice sections[] = {
        {apk->file.p, apk->block}, {apk->file.p + apk->cd, apk->cd_size}, {eocd, eocd_size}
    };
    uint32_t count = 0;
    for (size_t i = 0; i < 3; ++i) count += (uint32_t)(sections[i].n / CHUNK_SIZE + (sections[i].n % CHUNK_SIZE != 0));
    unsigned char prefix[5] = {0x5a};
    put32(prefix + 1, count);
    if (EVP_DigestInit_ex(root, EVP_sha256(), NULL) != 1 || EVP_DigestUpdate(root, prefix, sizeof(prefix)) != 1) goto crypto_error;
    for (size_t i = 0; i < 3; ++i) {
        Slice section = sections[i];
        while (section.n) {
            Slice bytes;
            const size_t length = section.n < CHUNK_SIZE ? section.n : CHUNK_SIZE;
            if (!take(&section, length, &bytes)) goto crypto_error;
            prefix[0] = 0xa5; put32(prefix + 1, (uint32_t)length);
            unsigned char digest[32]; unsigned digest_length = 0;
            if (EVP_DigestInit_ex(chunk, EVP_sha256(), NULL) != 1 ||
                EVP_DigestUpdate(chunk, prefix, sizeof(prefix)) != 1 ||
                EVP_DigestUpdate(chunk, bytes.p, bytes.n) != 1 ||
                EVP_DigestFinal_ex(chunk, digest, &digest_length) != 1 || digest_length != 32 ||
                EVP_DigestUpdate(root, digest, sizeof(digest)) != 1) goto crypto_error;
        }
    }
    unsigned length = 0;
    if (EVP_DigestFinal_ex(root, result, &length) != 1 || length != 32) goto crypto_error;
    ok = true;
    goto done;
crypto_error:
    fail("OpenSSL SHA-256 failure");
done:
    free(eocd); EVP_MD_CTX_free(root); EVP_MD_CTX_free(chunk);
    return ok;
}

static bool rsa_key(EVP_PKEY *key) {
    if (!key || !EVP_PKEY_is_a(key, "RSA")) return fail("require an unrestricted RSA key");
    const int bits = EVP_PKEY_get_bits(key);
    if (bits != 2048 && bits != 4096 && bits != 8192 && bits != 16384)
        return fail("require an Android-supported RSA key size of 2048, 4096, 8192 or 16384 bits");
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(key, NULL);
    const bool ok = ctx && EVP_PKEY_public_check(ctx) == 1;
    EVP_PKEY_CTX_free(ctx);
    if (!ok) return fail("invalid RSA public key");
    return true;
}
static bool public_der(EVP_PKEY *key, Buffer *out) {
    const int length = i2d_PUBKEY(key, NULL);
    if (length <= 0 || (size_t)length > MAX_SIGNING_SIZE) return fail("cannot encode RSA public key");
    out->p = malloc((size_t)length); out->n = (size_t)length;
    if (!out->p) return fail("out of memory encoding key");
    unsigned char *position = out->p;
    if (i2d_PUBKEY(key, &position) != length) return fail("cannot encode RSA public key");
    return true;
}
static bool rsa_signature(EVP_PKEY *key, Slice data, Buffer *signature, bool signing) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new(); EVP_PKEY_CTX *pctx = NULL;
    bool ok = false;
    if (!ctx) return fail("out of memory signing/verifying");
    const int init = signing ? EVP_DigestSignInit(ctx, &pctx, EVP_sha256(), NULL, key) :
                               EVP_DigestVerifyInit(ctx, &pctx, EVP_sha256(), NULL, key);
    if (init != 1 || EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PADDING) <= 0) goto done;
    if (signing) {
        size_t size = 0;
        if (EVP_DigestSign(ctx, NULL, &size, data.p, data.n) != 1 || !size || size > MAX_SIGNING_SIZE) goto done;
        signature->p = malloc(size); signature->n = size;
        if (!signature->p) goto done;
        ok = EVP_DigestSign(ctx, signature->p, &signature->n, data.p, data.n) == 1;
    } else ok = EVP_DigestVerify(ctx, signature->p, signature->n, data.p, data.n) == 1;
done:
    EVP_MD_CTX_free(ctx);
    if (!ok) return fail(signing ? "RSA signing failed" : "RSA signature verification failed");
    return true;
}
static bool verify_apk(const Apk *apk) {
    Slice block = apk->v2, signers, signer, signed_data, signatures, public_key, record, signature, id;
    if (!take_lp(&block, &signers) || block.n || !take_lp(&signers, &signer) || signers.n ||
        !take_lp(&signer, &signed_data) || !take_lp(&signer, &signatures) || !take_lp(&signer, &public_key) || signer.n ||
        !take_lp(&signatures, &record) || signatures.n || !take(&record, 4, &id) || u32(id.p) != RSA_SHA256 ||
        !take_lp(&record, &signature) || record.n || !signature.n || !public_key.n)
        return fail("invalid v2 signer/signature framing; require one signer and algorithm 0x0103");
    const unsigned char *position = public_key.p;
    EVP_PKEY *key = d2i_PUBKEY(NULL, &position, (long)public_key.n);
    X509 *cert = NULL;
    EVP_PKEY *cert_key = NULL;
    Buffer cert_public = {0}, canonical_public = {0};
    bool ok = false;
    if (!key || position != public_key.p + public_key.n || !rsa_key(key) ||
        !public_der(key, &canonical_public) || !equal(view(&canonical_public), public_key)) {
        fail("invalid/noncanonical SubjectPublicKeyInfo"); goto done;
    }
    Buffer signature_view = {(unsigned char *)signature.p, signature.n, 0};
    if (!rsa_signature(key, signed_data, &signature_view, false)) goto done;
    /* The nested signed data is interpreted only after authenticating it. */
    Slice signed_cursor = signed_data, digests, certificates, attributes, digest, der;
    if (!take_lp(&signed_cursor, &digests) || !take_lp(&signed_cursor, &certificates) ||
        !take_lp(&signed_cursor, &attributes) || signed_cursor.n || attributes.n ||
        !take_lp(&digests, &record) || digests.n || !take(&record, 4, &id) || u32(id.p) != RSA_SHA256 ||
        !take_lp(&record, &digest) || record.n || digest.n != 32 ||
        !take_lp(&certificates, &der) || certificates.n || !der.n) {
        fail("invalid authenticated digest/certificate/attribute framing"); goto done;
    }
    position = der.p;
    cert = d2i_X509(NULL, &position, (long)der.n);
    if (!cert || position != der.p + der.n || !(cert_key = X509_get_pubkey(cert)) ||
        !public_der(cert_key, &cert_public) || !equal(view(&cert_public), public_key) || EVP_PKEY_eq(cert_key, key) != 1) {
        fail("certificate public key differs from signer public key"); goto done;
    }
    unsigned char actual[32];
    if (!content_digest(apk, actual)) goto done;
    if (CRYPTO_memcmp(actual, digest.p, 32)) { fail("APK content digest mismatch"); goto done; }
    ok = true;
done:
    release(&cert_public); release(&canonical_public);
    EVP_PKEY_free(cert_key); X509_free(cert); EVP_PKEY_free(key);
    return ok;
}

static int no_password(char *buffer, int size, int writing, void *context) {
    (void)buffer; (void)size; (void)writing; (void)context;
    return 0;
}
static bool pem_tail_empty(BIO *bio) {
    unsigned char byte;
    int result;
    while ((result = BIO_read(bio, &byte, 1)) == 1) if (!isspace(byte)) return false;
    return result == 0;
}
static BIO *private_key_bio(const char *path) {
    /* Validate the descriptor we will parse, avoiding a stat/open path race.
     * NONBLOCK makes an accidental FIFO safe to inspect and reject immediately. */
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) { fail("cannot open private key without following symlinks"); return NULL; }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd); fail("private key must be a regular file"); return NULL;
    }
    if (st.st_uid != geteuid()) {
        close(fd); fail("private key must be owned by the effective user"); return NULL;
    }
    if (st.st_mode & (S_IRWXG | S_IRWXO)) {
        close(fd); fail("private key must have no group/other permissions (use mode 0600 or 0400)"); return NULL;
    }
    BIO *bio = BIO_new_fd(fd, BIO_CLOSE);
    if (!bio) { close(fd); fail("cannot create private key reader"); }
    return bio;
}
static bool make_block(const Apk *apk, const char *key_path, const char *cert_path, Buffer *block) {
    BIO *key_bio = private_key_bio(key_path), *cert_bio = NULL;
    EVP_PKEY *key = NULL, *cert_key = NULL;
    X509 *cert = NULL;
    Buffer cert_der = {0}, public_key = {0}, digest_record = {0}, digests = {0}, certificates = {0};
    Buffer signed_data = {0}, signature = {0}, signature_record = {0}, signatures = {0};
    Buffer signer = {0}, signers = {0}, v2 = {0}, pair = {0};
    bool ok = false;
    if (!key_bio) goto done;
    cert_bio = BIO_new_file(cert_path, "r");
    if (!cert_bio || !(key = PEM_read_bio_PrivateKey(key_bio, NULL, no_password, NULL)) ||
        !(cert = PEM_read_bio_X509(cert_bio, NULL, no_password, NULL)) ||
        !pem_tail_empty(key_bio) || !pem_tail_empty(cert_bio)) {
        fail("require one unencrypted PEM private key and one PEM certificate"); goto done;
    }
    if (!rsa_key(key)) goto done;
    EVP_PKEY_CTX *check = EVP_PKEY_CTX_new(key, NULL);
    const bool private_ok = check && EVP_PKEY_private_check(check) == 1 && EVP_PKEY_pairwise_check(check) == 1;
    EVP_PKEY_CTX_free(check);
    if (!private_ok || !(cert_key = X509_get_pubkey(cert)) || EVP_PKEY_eq(key, cert_key) != 1) {
        fail("private key is invalid or does not match certificate"); goto done;
    }
    const int cert_length = i2d_X509(cert, NULL);
    if (cert_length <= 0 || (size_t)cert_length > MAX_SIGNING_SIZE) { fail("cannot encode certificate"); goto done; }
    cert_der.p = malloc((size_t)cert_length); cert_der.n = (size_t)cert_length;
    if (!cert_der.p) { fail("out of memory encoding certificate"); goto done; }
    unsigned char *position = cert_der.p;
    if (i2d_X509(cert, &position) != cert_length || !public_der(key, &public_key)) goto done;
    unsigned char digest[32];
    if (!content_digest(apk, digest) ||
        !append32(&digest_record, RSA_SHA256) || !lp(&digest_record, (Slice){digest, sizeof(digest)}) ||
        !lp(&digests, view(&digest_record)) || !lp(&certificates, view(&cert_der)) ||
        !lp(&signed_data, view(&digests)) || !lp(&signed_data, view(&certificates)) || !append32(&signed_data, 0) ||
        !rsa_signature(key, view(&signed_data), &signature, true) ||
        !append32(&signature_record, RSA_SHA256) || !lp(&signature_record, view(&signature)) ||
        !lp(&signatures, view(&signature_record)) || !lp(&signer, view(&signed_data)) ||
        !lp(&signer, view(&signatures)) || !lp(&signer, view(&public_key)) ||
        !lp(&signers, view(&signer)) || !lp(&v2, view(&signers)) ||
        !append64(&pair, 4 + v2.n) || !append32(&pair, V2_ID) || !append(&pair, v2.p, v2.n) ||
        !append64(block, pair.n + 24) || !append(block, pair.p, pair.n) ||
        !append64(block, pair.n + 24) || !append(block, magic, sizeof(magic) - 1)) goto done;
    ok = true;
done:
    BIO_free(key_bio); BIO_free(cert_bio); EVP_PKEY_free(key); EVP_PKEY_free(cert_key); X509_free(cert);
    Buffer *buffers[] = {&cert_der, &public_key, &digest_record, &digests, &certificates, &signed_data,
                        &signature, &signature_record, &signatures, &signer, &signers, &v2, &pair};
    for (size_t i = 0; i < sizeof(buffers) / sizeof(buffers[0]); ++i) release(buffers[i]);
    return ok;
}
static bool write_all(int fd, Slice s) {
    while (s.n) {
        const size_t size = s.n > CHUNK_SIZE ? CHUNK_SIZE : s.n;
        const ssize_t written = write(fd, s.p, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        s.p += written; s.n -= (size_t)written;
    }
    return true;
}
/* Publish a complete fsynced file atomically, never replacing an existing path. */
static bool write_signed(Apk *apk, const Buffer *block, const char *output) {
    if (block->n > UINT32_MAX - apk->file.n || block->n > UINT32_MAX - apk->cd)
        return fail("signed APK would require ZIP64");
    struct stat st;
    if (lstat(output, &st) == 0 || errno != ENOENT) return fail("output exists or cannot be inspected; not overwritten");
    const size_t path_length = strlen(output);
    if (path_length > SIZE_MAX - 12) return fail("output path too long");
    char *temporary = malloc(path_length + 12);
    if (!temporary) return fail("out of memory publishing APK");
    snprintf(temporary, path_length + 12, "%s.tmp.XXXXXX", output);
    int fd = mkstemp(temporary);
    const bool created = fd >= 0;
    bool ok = false;
    if (fd < 0) { fail("cannot create output temporary file"); goto done; }
    put32(apk->file.p + apk->eocd + 16, (uint32_t)(apk->cd + block->n));
    if (!write_all(fd, (Slice){apk->file.p, apk->cd}) || !write_all(fd, view(block)) ||
        !write_all(fd, (Slice){apk->file.p + apk->cd, apk->file.n - apk->cd}) || fsync(fd)) {
        fail("cannot write/sync signed APK"); goto done;
    }
    if (close(fd)) { fd = -1; fail("cannot close signed APK"); goto done; }
    fd = -1;
    if (link(temporary, output)) { fail("cannot publish APK (existing output is never replaced)"); goto done; }
    ok = true;
done:
    if (fd >= 0) close(fd);
    if (created) unlink(temporary);
    free(temporary);
    return ok;
}

int main(int argc, char **argv) {
    const bool signing = argc == 6 && !strcmp(argv[1], "sign");
    const bool verifying = argc == 3 && !strcmp(argv[1], "verify");
    if (!signing && !verifying) {
        fprintf(stderr, "Usage: %s sign input.apk output.apk key.pem cert.pem\n       %s verify signed.apk\n", argv[0], argv[0]);
        return 2;
    }
    Apk apk = {0}; Buffer block = {0};
    bool ok = read_file(argv[2], &apk.file) && parse_apk(&apk, verifying);
    if (ok && verifying) ok = verify_apk(&apk);
    if (ok && signing) ok = make_block(&apk, argv[4], argv[5], &block) && write_signed(&apk, &block, argv[3]);
    release(&block); release(&apk.file);
    if (ok) puts(signing ? "Signed APK: v2 RSA/PKCS1 SHA-256 (0x0103)" : "Verified APK: v2 integrity and certificate/public-key match");
    return ok ? 0 : 1;
}
