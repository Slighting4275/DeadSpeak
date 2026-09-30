#!/bin/sh
# Native-only signer regression suite. All keys, APK fixtures and C helpers are
# temporary; nothing is installed or sent to a device. Run: sh tests/signing_test.sh
project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd) || exit 1
# /tmp may be mounted noexec on Linux; use ignored native build scratch space.
mkdir -p "$project_dir/build" || exit 1
signing_test_dir=$(mktemp -d "$project_dir/build/signing-test-XXXXXX") || exit 1
trap 'rm -rf -- "$signing_test_dir"' EXIT
trap 'exit 1' HUP INT TERM
fail() { echo "FAIL: $*" >&2; exit 1; }
must() { "$@" || fail "$*"; }
reject() {
    if "$@" >"$signing_test_dir/rejection.log" 2>&1; then
        fail "unexpected success: $*"
    else
        rejection_status=$?
    fi
    [ "$rejection_status" -le 2 ] || fail "crash/unexpected exit $rejection_status: $*"
    if rg -q 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' "$signing_test_dir/rejection.log"; then
        fail "sanitizer finding: $*"
    fi
}
if [ -z "${ZIG_LIB_DIR:-}" ] && [ -f "$project_dir/local.properties" ]; then
    signing_zig_lib=$(sed -n 's/^deadspeak.zigLibDir=//p' "$project_dir/local.properties")
    if [ -n "$signing_zig_lib" ]; then export ZIG_LIB_DIR="$signing_zig_lib"; fi
fi
# Compiler options may be supplied by the native build's regression runner.
signing_cc_flags=${SIGNING_TEST_CFLAGS:--O2}
signing_crypto_cflags=$(pkg-config --cflags libcrypto) || fail "libcrypto development headers required"
signing_crypto_libs=$(pkg-config --libs libcrypto) || fail "libcrypto development library required"
signing_archive_cflags=$(pkg-config --cflags libarchive) || fail "libarchive development headers required for packaging fixtures"
signing_archive_libs=$(pkg-config --libs libarchive) || fail "libarchive development library required for packaging fixtures"
# Intentional word splitting for ordinary compiler/pkg-config argument lists; no eval.
# shellcheck disable=SC2086
must zig cc -std=c17 -Wall -Wextra -Werror $signing_cc_flags $signing_crypto_cflags \
    "$project_dir/tools/apk_sign.c" $signing_crypto_libs -o "$signing_test_dir/apk-sign"

cat >"$signing_test_dir/fixture.c" <<'C'
#define _POSIX_C_SOURCE 200809L
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <archive.h>
#include <archive_entry.h>

static uint16_t r16(const unsigned char *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t r32(const unsigned char *p) { return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static uint64_t r64(const unsigned char *p) { return r32(p) | (uint64_t)r32(p+4)<<32; }
static void w16(unsigned char *p, uint16_t v) { p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); }
static void w32(unsigned char *p, uint32_t v) { for(unsigned i=0;i<4;++i)p[i]=(unsigned char)(v>>(8*i)); }
static void w64(unsigned char *p, uint64_t v) { w32(p,(uint32_t)v);w32(p+4,(uint32_t)(v>>32)); }
static unsigned char *load(const char *path, size_t *n) {
    FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long size=ftell(f);assert(size>=0);
    *n=(size_t)size;unsigned char *p=malloc(*n+1);assert(p);rewind(f);assert(fread(p,1,*n,f)==*n);assert(!fclose(f));return p;
}
static void save(const char *path,const unsigned char *p,size_t n) {
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(p,1,n,f)==n);assert(!fclose(f));
}
static uint32_t crc32(const unsigned char *p,size_t n) {
    uint32_t c=UINT32_MAX;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned b=0;b<8;++b)c=(c>>1)^((c&1)?UINT32_C(0xedb88320):0);}return ~c;
}
static void make_zip(const char *path,size_t payload_size,unsigned count,unsigned descriptor,const char *first_name) {
    assert(count>0&&count<65535&&payload_size<8000000&&descriptor<=2);
    const size_t capacity=payload_size+(size_t)count*200+70000;
    unsigned char *zip=calloc(1,capacity);assert(zip);
    size_t *offsets=calloc(count,sizeof(*offsets));assert(offsets);
    size_t pos=0;unsigned char *payload=malloc(payload_size+1);assert(payload);
    for(size_t i=0;i<payload_size;++i)payload[i]=(unsigned char)(i*19+7);
    for(unsigned i=0;i<count;++i){
        char name[64];if(i==0)snprintf(name,sizeof(name),"%s",first_name);else snprintf(name,sizeof(name),"assets/item-%08u",i);
        size_t len=strlen(name),bytes=i==0?payload_size:0;uint32_t crc=crc32(payload,bytes);unsigned char *l=zip+pos;
        offsets[i]=pos;w32(l,UINT32_C(0x04034b50));w16(l+4,20);w16(l+6,descriptor?8:0);w16(l+26,(uint16_t)len);
        if(!descriptor){w32(l+14,crc);w32(l+18,(uint32_t)bytes);w32(l+22,(uint32_t)bytes);}
        memcpy(l+30,name,len);memcpy(l+30+len,payload,bytes);pos+=30+len+bytes;
        if(descriptor){if(descriptor==1){w32(zip+pos,UINT32_C(0x08074b50));pos+=4;}
            w32(zip+pos,crc);w32(zip+pos+4,(uint32_t)bytes);w32(zip+pos+8,(uint32_t)bytes);pos+=12;}
    }
    size_t cd=pos;
    for(unsigned i=0;i<count;++i){
        unsigned char *l=zip+offsets[i];size_t len=r16(l+26),bytes=i==0?payload_size:0;unsigned char *c=zip+pos;
        w32(c,UINT32_C(0x02014b50));w16(c+4,20);w16(c+6,20);w16(c+8,descriptor?8:0);
        w32(c+16,crc32(payload,bytes));w32(c+20,(uint32_t)bytes);w32(c+24,(uint32_t)bytes);
        w16(c+28,(uint16_t)len);w32(c+42,(uint32_t)offsets[i]);memcpy(c+46,l+30,len);pos+=46+len;
    }
    unsigned char *e=zip+pos;w32(e,UINT32_C(0x06054b50));w16(e+8,(uint16_t)count);w16(e+10,(uint16_t)count);
    w32(e+12,(uint32_t)(pos-cd));w32(e+16,(uint32_t)cd);pos+=22;
    assert(pos<=capacity);save(path,zip,pos);free(zip);free(offsets);free(payload);
}
static void make_libarchive(const char *path) {
    static const char *const names[]={"AndroidManifest.xml","res/drawable/ic_launcher_background.xml",
        "res/drawable/ic_launcher_foreground.xml","res/mipmap-anydpi-v21/ic_launcher.xml",
        "res/xml/data_extraction_rules.xml","resources.arsc","assets/words.bin","lib/arm64-v8a/libtest.so"};
    struct archive *writer=archive_write_new();assert(writer);
    assert(archive_write_set_format_zip(writer)==ARCHIVE_OK);
    assert(archive_write_set_options(writer,"zip:compression=store,zip:zip64=")==ARCHIVE_OK);
    assert(archive_write_open_filename(writer,path)==ARCHIVE_OK);
    size_t capacity=2200000;unsigned char *bytes=malloc(capacity);assert(bytes);
    for(size_t i=0;i<capacity;++i)bytes[i]=(unsigned char)(i*23+9);
    for(size_t i=0;i<sizeof(names)/sizeof(names[0]);++i){
        struct archive_entry *entry=archive_entry_new();assert(entry);size_t size=i==7?capacity:128;
        archive_entry_set_pathname(entry,names[i]);archive_entry_set_filetype(entry,AE_IFREG);
        archive_entry_set_perm(entry,0644);archive_entry_set_mtime(entry,315532800,0);archive_entry_set_size(entry,(la_int64_t)size);
        assert(archive_write_header(writer,entry)==ARCHIVE_OK);
        assert(archive_write_data(writer,bytes,size)==(la_ssize_t)size);archive_entry_free(entry);
    }
    free(bytes);assert(archive_write_close(writer)==ARCHIVE_OK);assert(archive_write_free(writer)==ARCHIVE_OK);
}
/* Independent framing extractor and digest oracle: output chunk inputs for
 * the OpenSSL CLI, never use the signer's code or its hashing implementation. */
static size_t eocd(const unsigned char *p,size_t n) {
    assert(n>=22);for(size_t i=n-22;;--i){if(r32(p+i)==UINT32_C(0x06054b50)&&r16(p+i+20)==n-i-22)return i;assert(i);}return 0;
}
typedef struct {size_t cd,e,block,sd,sd_n,sig,sig_n,key,key_n,digest,cert,cert_n;} Info;
static Info info(const unsigned char *p,size_t n) {
    Info v={0};v.e=eocd(p,n);v.cd=r32(p+v.e+16);assert(v.cd>=32);
    assert(!memcmp(p+v.cd-16,"APK Sig Block 42",16));v.block=v.cd-(size_t)r64(p+v.cd-24)-8;
    assert(r32(p+v.block+16)==UINT32_C(0x7109871a));size_t x=v.block+20;
    x+=4;size_t signer_n=r32(p+x);x+=4;size_t signer_end=x+signer_n;
    v.sd_n=r32(p+x);v.sd=x+4;size_t signatures=v.sd+v.sd_n;
    size_t signatures_n=r32(p+signatures);size_t sig_record=signatures+8;
    assert(r32(p+sig_record)==UINT32_C(0x0103));v.sig_n=r32(p+sig_record+4);v.sig=sig_record+8;
    assert(v.sig+v.sig_n==signatures+4+signatures_n);v.key_n=r32(p+v.sig+v.sig_n);v.key=v.sig+v.sig_n+4;
    assert(v.key+v.key_n==signer_end);size_t xdig=v.sd+8;assert(r32(p+xdig)==UINT32_C(0x0103));
    assert(r32(p+xdig+4)==32);v.digest=xdig+8;
    size_t certs=v.sd+4+r32(p+v.sd);v.cert_n=r32(p+certs+4);v.cert=certs+8;return v;
}
static void oracle(const char *path,const char *dir) {
    size_t n;unsigned char *p=load(path,&n);Info v=info(p,n);char out[1024];
    snprintf(out,sizeof(out),"%s/signed-data.bin",dir);save(out,p+v.sd,v.sd_n);
    snprintf(out,sizeof(out),"%s/signature.bin",dir);save(out,p+v.sig,v.sig_n);
    snprintf(out,sizeof(out),"%s/public.der",dir);save(out,p+v.key,v.key_n);
    snprintf(out,sizeof(out),"%s/certificate.der",dir);save(out,p+v.cert,v.cert_n);
    snprintf(out,sizeof(out),"%s/expected-root.bin",dir);save(out,p+v.digest,32);
    size_t starts[]={0,v.cd,v.e},sizes[]={v.block,v.e-v.cd,n-v.e};unsigned count=0;
    w32(p+v.e+16,(uint32_t)v.block);
    for(unsigned s=0;s<3;++s){for(size_t off=0;off<sizes[s];){size_t len=sizes[s]-off;if(len>1048576)len=1048576;
        unsigned char *chunk=malloc(len+5);assert(chunk);chunk[0]=0xa5;w32(chunk+1,(uint32_t)len);memcpy(chunk+5,p+starts[s]+off,len);
        snprintf(out,sizeof(out),"%s/chunk-%u.bin",dir,count++);save(out,chunk,len+5);free(chunk);off+=len;}}
    unsigned char prefix[5]={0x5a};w32(prefix+1,count);snprintf(out,sizeof(out),"%s/root-input.bin",dir);save(out,prefix,5);
    snprintf(out,sizeof(out),"%s/chunk-count",dir);FILE *f=fopen(out,"w");assert(f);fprintf(f,"%u\n",count);assert(!fclose(f));free(p);
}
static void root_input(const char *dir) {
    char path[1024];snprintf(path,sizeof(path),"%s/chunk-count",dir);FILE *f=fopen(path,"r");unsigned count;assert(f&&fscanf(f,"%u",&count)==1);assert(!fclose(f));
    snprintf(path,sizeof(path),"%s/root-input.bin",dir);f=fopen(path,"ab");assert(f);
    for(unsigned i=0;i<count;++i){snprintf(path,sizeof(path),"%s/chunk-%u.sha",dir,i);size_t n;unsigned char *p=load(path,&n);assert(n==32);assert(fwrite(p,1,n,f)==n);free(p);}assert(!fclose(f));
}
static void resign(unsigned char *p,Info v,const char *key_path) {
    FILE *f=fopen(key_path,"r");assert(f);EVP_PKEY *key=PEM_read_PrivateKey(f,NULL,NULL,NULL);assert(key);assert(!fclose(f));
    EVP_MD_CTX *ctx=EVP_MD_CTX_new();EVP_PKEY_CTX *pc=NULL;assert(ctx);
    assert(EVP_DigestSignInit(ctx,&pc,EVP_sha256(),NULL,key)==1);assert(EVP_PKEY_CTX_set_rsa_padding(pc,RSA_PKCS1_PADDING)>0);
    size_t size=v.sig_n;assert(EVP_DigestSign(ctx,p+v.sig,&size,p+v.sd,v.sd_n)==1&&size==v.sig_n);EVP_MD_CTX_free(ctx);EVP_PKEY_free(key);
}
static void mutate(const char *input,const char *output,const char *kind,const char *key_path) {
    size_t n;unsigned char *p=load(input,&n);size_t e=eocd(p,n),cd=r32(p+e+16);Info v={0};
    bool signed_apk=cd>=24&&!memcmp(p+cd-16,"APK Sig Block 42",16);
    if(signed_apk)v=info(p,n);
    if(!strcmp(kind,"payload"))p[49]^=1;
    else if(!strcmp(kind,"central"))p[cd+38]^=1;
    else if(!strcmp(kind,"eocd")){unsigned char *q=realloc(p,n+1);assert(q);p=q;w16(p+e+20,1);p[n++]=0x42;}
    else if(!strcmp(kind,"trailing")){unsigned char *q=realloc(p,n+1);assert(q);p=q;p[n++]=0x42;}
    else if(!strcmp(kind,"truncated"))--n;
    else if(!strcmp(kind,"multidisk"))w16(p+e+4,1);
    else if(!strcmp(kind,"zip64"))w32(p+e+12,UINT32_MAX);
    else if(!strcmp(kind,"local-padding")||!strcmp(kind,"local-padding-bad")||!strcmp(kind,"local-zip64-extra")){
        assert(!signed_apk&&r16(p+26)==19&&!r16(p+28));size_t padding=!strcmp(kind,"local-zip64-extra")?4:3;
        unsigned char *q=realloc(p,n+padding);assert(q);p=q;memmove(p+49+padding,p+49,n-49);memset(p+49,0,padding);
        w16(p+28,(uint16_t)padding);if(!strcmp(kind,"local-padding-bad"))p[49]=2;
        if(!strcmp(kind,"local-zip64-extra"))w16(p+49,1);w32(p+e+padding+16,(uint32_t)(cd+padding));n+=padding;
    }
    else if(!strcmp(kind,"central-zip64-extra")){
        assert(!signed_apk&&r16(p+cd+28)==19&&!r16(p+cd+30));size_t x=cd+65;
        unsigned char *q=realloc(p,n+4);assert(q);p=q;memmove(p+x+4,p+x,n-x);memset(p+x,0,4);w16(p+x,1);
        w16(p+cd+30,4);w32(p+e+4+12,r32(p+e+4+12)+4);n+=4;
    }
    else if(!strcmp(kind,"count"))w16(p+e+10,(uint16_t)(r16(p+e+10)+1));
    else if(!strcmp(kind,"offset"))w32(p+e+16,(uint32_t)(cd+1));
    else if(!strcmp(kind,"local-offset"))w32(p+cd+42,UINT32_MAX);
    else if(!strcmp(kind,"entry-size"))w32(p+cd+20,UINT32_MAX-1);
    else if(!strcmp(kind,"encrypted"))w16(p+cd+8,1);
    else if(!strcmp(kind,"local-name"))p[30]^=1;
    else if(!strcmp(kind,"duplicate")){assert(r16(p+e+10)==2);size_t second=cd+46+r16(p+cd+28);w32(p+second+42,0);}
    else if(!strcmp(kind,"block-head"))p[v.block]^=1;
    else if(!strcmp(kind,"block-tail"))p[cd-24]^=1;
    else if(!strcmp(kind,"block-huge"))w64(p+cd-24,UINT64_MAX);
    else if(!strcmp(kind,"magic"))p[cd-1]^=1;
    else if(!strcmp(kind,"pair-length"))w64(p+v.block+8,UINT64_MAX);
    else if(!strcmp(kind,"pair-id"))p[v.block+16]^=1;
    else if(!strcmp(kind,"signer-length"))w32(p+v.block+24,UINT32_MAX);
    else if(!strcmp(kind,"second-signer")){
        size_t signer=v.block+24,bytes=(size_t)r32(p+signer)+4,insert=signer+bytes;
        unsigned char *q=malloc(n+bytes);assert(q);memcpy(q,p,insert);memcpy(q+insert,p+signer,bytes);memcpy(q+insert+bytes,p+insert,n-insert);
        w64(q+v.block,r64(p+v.block)+bytes);w64(q+v.block+8,r64(p+v.block+8)+bytes);
        w32(q+v.block+20,r32(p+v.block+20)+(uint32_t)bytes);w64(q+cd+bytes-24,r64(p+cd-24)+bytes);
        w32(q+e+bytes+16,(uint32_t)(cd+bytes));free(p);p=q;n+=bytes;
    }
    else if(!strcmp(kind,"signed-data"))p[v.sd]^=1;
    else if(!strcmp(kind,"signature"))p[v.sig]^=1;
    else if(!strcmp(kind,"signature-alg"))w32(p+v.sig-8,0x0101);
    else if(!strcmp(kind,"public-key"))p[v.key+v.key_n-1]^=1;
    else if(!strcmp(kind,"digest-resign")){p[v.digest]^=1;resign(p,v,key_path);}
    else if(!strcmp(kind,"digest-alg-resign")){w32(p+v.digest-8,0x0101);resign(p,v,key_path);}
    else if(!strcmp(kind,"cert-key-resign")){size_t x=v.cert;bool changed=false;
        for(;x+v.key_n<=v.cert+v.cert_n;++x){if(!memcmp(p+x,p+v.key,v.key_n)){p[x+v.key_n-1]^=2;changed=true;break;}}assert(changed);resign(p,v,key_path);}
    else if(!strcmp(kind,"certificate-resign")){p[v.cert]=0;resign(p,v,key_path);}
    else if(!strcmp(kind,"cert-trailing-resign")){w32(p+v.cert-4,(uint32_t)(v.cert_n-1));resign(p,v,key_path);}
    else if(!strcmp(kind,"attributes-resign")){w32(p+v.sd+v.sd_n-4,UINT32_MAX);resign(p,v,key_path);}
    else if(!strcmp(kind,"ambiguous")){unsigned char *q=realloc(p,n+22);assert(q);p=q;w16(p+e+20,22);memcpy(p+n,p+e,22);w16(p+n+20,0);n+=22;}
    else abort();save(output,p,n);free(p);
}
int main(int argc,char **argv) {
    assert(argc>=3);
    if(!strcmp(argv[1],"make")){assert(argc==7);make_zip(argv[2],strtoul(argv[3],NULL,10),(unsigned)strtoul(argv[4],NULL,10),(unsigned)strtoul(argv[5],NULL,10),argv[6]);}
    else if(!strcmp(argv[1],"make-libarchive")){assert(argc==3);make_libarchive(argv[2]);}
    else if(!strcmp(argv[1],"oracle")){assert(argc==4);oracle(argv[2],argv[3]);}
    else if(!strcmp(argv[1],"root")){assert(argc==3);root_input(argv[2]);}
    else if(!strcmp(argv[1],"mutate")){assert(argc==6);mutate(argv[2],argv[3],argv[4],argv[5]);}
    else abort();return 0;
}
C
# shellcheck disable=SC2086
must zig cc -std=c17 -Wall -Wextra -Werror $signing_cc_flags $signing_crypto_cflags $signing_archive_cflags \
    "$signing_test_dir/fixture.c" $signing_crypto_libs $signing_archive_libs -o "$signing_test_dir/fixture"
signer="$signing_test_dir/apk-sign"
fixture="$signing_test_dir/fixture"
must openssl req -x509 -newkey rsa:2048 -keyout "$signing_test_dir/key.pem" \
    -out "$signing_test_dir/cert.pem" -sha256 -days 1 -nodes -subj /CN=DeadSpeak-native-signing-test \
    >"$signing_test_dir/keygen.log" 2>&1
must openssl req -x509 -newkey rsa:2048 -keyout "$signing_test_dir/other-key.pem" \
    -out "$signing_test_dir/other-cert.pem" -sha256 -days 1 -nodes -subj /CN=Other-test-key \
    >"$signing_test_dir/keygen.log" 2>&1
must openssl pkey -in "$signing_test_dir/key.pem" -pubout -out "$signing_test_dir/public.pem"

verify_oracle() {
    oracle_dir="$signing_test_dir/oracle-$2"
    must mkdir "$oracle_dir"
    must "$fixture" oracle "$1" "$oracle_dir"
    must openssl dgst -sha256 -verify "$signing_test_dir/public.pem" \
        -signature "$oracle_dir/signature.bin" "$oracle_dir/signed-data.bin"
    chunk_count=$(sed -n '1p' "$oracle_dir/chunk-count")
    [ "$chunk_count" = "$3" ] || fail "wrong independent chunk count: $chunk_count != $3"
    chunk_index=0
    while [ "$chunk_index" -lt "$chunk_count" ]; do
        must openssl dgst -sha256 -binary -out "$oracle_dir/chunk-$chunk_index.sha" "$oracle_dir/chunk-$chunk_index.bin"
        chunk_index=$((chunk_index + 1))
    done
    must "$fixture" root "$oracle_dir"
    must openssl dgst -sha256 -binary -out "$oracle_dir/actual-root.bin" "$oracle_dir/root-input.bin"
    must cmp "$oracle_dir/expected-root.bin" "$oracle_dir/actual-root.bin"
    must openssl pkey -pubin -in "$signing_test_dir/public.pem" -outform DER -out "$oracle_dir/expected-public.der"
    must cmp "$oracle_dir/public.der" "$oracle_dir/expected-public.der"
    must openssl x509 -in "$signing_test_dir/cert.pem" -outform DER -out "$oracle_dir/expected-cert.der"
    must cmp "$oracle_dir/certificate.der" "$oracle_dir/expected-cert.der"
}

for size in 128 1048527 1048528 2200000; do
    input="$signing_test_dir/input-$size.apk"
    output="$signing_test_dir/signed-$size.apk"
    must "$fixture" make "$input" "$size" 1 0 AndroidManifest.xml
    must "$signer" sign "$input" "$output" "$signing_test_dir/key.pem" "$signing_test_dir/cert.pem"
    must "$signer" verify "$output"
    must "$signer" sign "$input" "$signing_test_dir/repeat-$size.apk" "$signing_test_dir/key.pem" "$signing_test_dir/cert.pem"
    must cmp "$output" "$signing_test_dir/repeat-$size.apk"
    case "$size" in 128|1048527) chunks=3;; 1048528) chunks=4;; 2200000) chunks=5;; esac
    verify_oracle "$output" "$size" "$chunks"
done
# Both content and central directory independently cross the 1 MiB boundary.
must "$fixture" make "$signing_test_dir/many.apk" 128 22000 0 AndroidManifest.xml
must "$signer" sign "$signing_test_dir/many.apk" "$signing_test_dir/many-signed.apk" "$signing_test_dir/key.pem" "$signing_test_dir/cert.pem"
must "$signer" verify "$signing_test_dir/many-signed.apk"
verify_oracle "$signing_test_dir/many-signed.apk" many 5

# Optional integration with the SDK's native zipalign, including 16 KiB library
# alignment. Set SIGNING_TEST_ZIPALIGN to its executable path to run this check.
if [ -n "${SIGNING_TEST_ZIPALIGN:-}" ]; then
    must "$fixture" make "$signing_test_dir/unaligned.apk" 128 1 0 lib/arm64-v8a/libtest.so
    must "$SIGNING_TEST_ZIPALIGN" -P 16 -f 4 "$signing_test_dir/unaligned.apk" "$signing_test_dir/aligned.apk"
    must "$signer" sign "$signing_test_dir/aligned.apk" "$signing_test_dir/aligned-signed.apk" "$signing_test_dir/key.pem" "$signing_test_dir/cert.pem"
    must "$signer" verify "$signing_test_dir/aligned-signed.apk"
    must "$SIGNING_TEST_ZIPALIGN" -P 16 -c 4 "$signing_test_dir/aligned-signed.apk"
    must "$fixture" make-libarchive "$signing_test_dir/libarchive.apk"
    must "$SIGNING_TEST_ZIPALIGN" -P 16 -f 4 "$signing_test_dir/libarchive.apk" "$signing_test_dir/libarchive-aligned.apk"
    must "$signer" sign "$signing_test_dir/libarchive-aligned.apk" "$signing_test_dir/libarchive-signed.apk" "$signing_test_dir/key.pem" "$signing_test_dir/cert.pem"
    must "$signer" verify "$signing_test_dir/libarchive-signed.apk"
    must "$SIGNING_TEST_ZIPALIGN" -P 16 -c 4 "$signing_test_dir/libarchive-signed.apk"
    verify_oracle "$signing_test_dir/libarchive-signed.apk" libarchive 5
fi

small="$signing_test_dir/input-128.apk"
signed="$signing_test_dir/signed-128.apk"
# Private-key descriptor policy is separate from public certificate handling.
must cp "$signing_test_dir/key.pem" "$signing_test_dir/permissions-key.pem"
for mode in 0644 0620 0604 0610 0601; do
    must chmod "$mode" "$signing_test_dir/permissions-key.pem"
    reject "$signer" sign "$small" "$signing_test_dir/permissions-rejected.apk" "$signing_test_dir/permissions-key.pem" "$signing_test_dir/cert.pem"
    rg -q 'private key must have no group/other permissions' "$signing_test_dir/rejection.log" || fail "private-key permission check did not run: $mode"
    [ ! -e "$signing_test_dir/permissions-rejected.apk" ] || fail "published APK with unsafe private-key permissions"
done
must chmod 0400 "$signing_test_dir/permissions-key.pem"
must chmod 0644 "$signing_test_dir/cert.pem"
must "$signer" sign "$small" "$signing_test_dir/public-cert.apk" "$signing_test_dir/permissions-key.pem" "$signing_test_dir/cert.pem"
must "$signer" verify "$signing_test_dir/public-cert.apk"
must cmp "$signed" "$signing_test_dir/public-cert.apk"
must ln -s "$signing_test_dir/key.pem" "$signing_test_dir/symlink-key.pem"
reject "$signer" sign "$small" "$signing_test_dir/symlink-rejected.apk" "$signing_test_dir/symlink-key.pem" "$signing_test_dir/cert.pem"
rg -q 'cannot open private key without following symlinks' "$signing_test_dir/rejection.log" || fail "private-key symlink was not rejected at open"
[ ! -e "$signing_test_dir/symlink-rejected.apk" ] || fail "published APK from a private-key symlink"
reject "$signer" sign "$small" "$signing_test_dir/directory-rejected.apk" "$signing_test_dir" "$signing_test_dir/cert.pem"
rg -q 'private key must be a regular file' "$signing_test_dir/rejection.log" || fail "private-key directory was not rejected by fstat"
must mkfifo "$signing_test_dir/fifo-key.pem"
reject timeout 5 "$signer" sign "$small" "$signing_test_dir/fifo-rejected.apk" "$signing_test_dir/fifo-key.pem" "$signing_test_dir/cert.pem"
rg -q 'private key must be a regular file' "$signing_test_dir/rejection.log" || fail "private-key FIFO was not rejected by fstat"

# Exercise the effective-owner comparison without changing file ownership or
# requesting elevated access: this test executable supplies a different euid.
cat >"$signing_test_dir/owner-test.c" <<'C'
#define _POSIX_C_SOURCE 200809L
#include <sys/types.h>
#include <unistd.h>
uid_t geteuid(void) { return getuid() ^ (uid_t)1; }
C
# shellcheck disable=SC2086
must zig cc -std=c17 -Wall -Wextra -Werror $signing_cc_flags $signing_crypto_cflags \
    "$project_dir/tools/apk_sign.c" "$signing_test_dir/owner-test.c" $signing_crypto_libs -o "$signing_test_dir/owner-test"
reject "$signing_test_dir/owner-test" sign "$small" "$signing_test_dir/owner-rejected.apk" "$signing_test_dir/key.pem" "$signing_test_dir/cert.pem"
rg -q 'private key must be owned by the effective user' "$signing_test_dir/rejection.log" || fail "effective-owner check did not reject a mismatched owner"
[ ! -e "$signing_test_dir/owner-rejected.apk" ] || fail "published APK from a private key owned by another user"

reject "$signer" verify "$small"
reject "$signer" sign "$signed" "$signing_test_dir/resigned.apk" "$signing_test_dir/key.pem" "$signing_test_dir/cert.pem"
[ ! -e "$signing_test_dir/resigned.apk" ] || fail "published rejected signed input"
reject "$signer" sign "$small" "$signed" "$signing_test_dir/key.pem" "$signing_test_dir/cert.pem"
must cmp "$signed" "$signing_test_dir/repeat-128.apk"
reject "$signer" sign "$small" "$small" "$signing_test_dir/key.pem" "$signing_test_dir/cert.pem"
reject "$signer" sign "$small" "$signing_test_dir/mismatch.apk" "$signing_test_dir/key.pem" "$signing_test_dir/other-cert.pem"
[ ! -e "$signing_test_dir/mismatch.apk" ] || fail "published mismatched signer"
reject "$signer" sign "$small" "$signing_test_dir/public-key.apk" "$signing_test_dir/public.pem" "$signing_test_dir/cert.pem"
reject "$signer" sign
reject "$signer" verify
reject "$signer" unknown "$signed"
must openssl pkey -in "$signing_test_dir/key.pem" -aes-256-cbc -passout pass:test-only \
    -out "$signing_test_dir/encrypted-key.pem"
reject "$signer" sign "$small" "$signing_test_dir/encrypted.apk" "$signing_test_dir/encrypted-key.pem" "$signing_test_dir/cert.pem"
must openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -keyout "$signing_test_dir/ec-key.pem" \
    -out "$signing_test_dir/ec-cert.pem" -sha256 -days 1 -nodes -subj /CN=Unsupported-EC-test \
    >"$signing_test_dir/keygen.log" 2>&1
reject "$signer" sign "$small" "$signing_test_dir/ec.apk" "$signing_test_dir/ec-key.pem" "$signing_test_dir/ec-cert.pem"

for kind in payload central eocd trailing truncated multidisk zip64 count offset local-offset entry-size \
    encrypted local-name block-head block-tail block-huge magic pair-length pair-id signer-length second-signer \
    signed-data signature signature-alg public-key digest-resign digest-alg-resign cert-key-resign \
    certificate-resign cert-trailing-resign attributes-resign ambiguous; do
    must "$fixture" mutate "$signed" "$signing_test_dir/mutated.apk" "$kind" "$signing_test_dir/key.pem"
    reject "$signer" verify "$signing_test_dir/mutated.apk"
    case "$kind" in
        payload|central|eocd|digest-resign)
            rg -q 'APK content digest mismatch' "$signing_test_dir/rejection.log" || fail "mutation did not reach content verification: $kind";;
        cert-key-resign)
            rg -q 'certificate public key differs' "$signing_test_dir/rejection.log" || fail "certificate mismatch was not detected";;
    esac
done
for kind in trailing truncated multidisk zip64 count offset local-offset entry-size encrypted local-name ambiguous \
    local-padding-bad local-zip64-extra central-zip64-extra; do
    must "$fixture" mutate "$small" "$signing_test_dir/malformed.apk" "$kind" "$signing_test_dir/key.pem"
    reject "$signer" sign "$signing_test_dir/malformed.apk" "$signing_test_dir/rejected.apk" "$signing_test_dir/key.pem" "$signing_test_dir/cert.pem"
    [ ! -e "$signing_test_dir/rejected.apk" ] || fail "published malformed input: $kind"
done
must "$fixture" mutate "$small" "$signing_test_dir/padding.apk" local-padding "$signing_test_dir/key.pem"
must "$signer" sign "$signing_test_dir/padding.apk" "$signing_test_dir/padding-signed.apk" "$signing_test_dir/key.pem" "$signing_test_dir/cert.pem"
must "$signer" verify "$signing_test_dir/padding-signed.apk"
for name in META-INF/CERT.SF META-INF/CERT.RSA meta-inf/cert.ec META-INF/SIG-CERT; do
    must "$fixture" make "$signing_test_dir/v1.apk" 128 1 0 "$name"
    reject "$signer" sign "$signing_test_dir/v1.apk" "$signing_test_dir/rejected.apk" "$signing_test_dir/key.pem" "$signing_test_dir/cert.pem"
done
for descriptor in 1 2; do
    must "$fixture" make "$signing_test_dir/descriptor-$descriptor.apk" 128 2 "$descriptor" AndroidManifest.xml
    must "$signer" sign "$signing_test_dir/descriptor-$descriptor.apk" "$signing_test_dir/descriptor-$descriptor-signed.apk" "$signing_test_dir/key.pem" "$signing_test_dir/cert.pem"
    must "$signer" verify "$signing_test_dir/descriptor-$descriptor-signed.apk"
done
must "$fixture" make "$signing_test_dir/two.apk" 128 2 0 AndroidManifest.xml
must "$fixture" mutate "$signing_test_dir/two.apk" "$signing_test_dir/duplicate.apk" duplicate "$signing_test_dir/key.pem"
reject "$signer" sign "$signing_test_dir/duplicate.apk" "$signing_test_dir/rejected.apk" "$signing_test_dir/key.pem" "$signing_test_dir/cert.pem"

# Minimal malformed corpus exercises bounded readers without a JVM or zip tools.
for length in 0 1 21 22 80 1024; do
    must dd if=/dev/zero of="$signing_test_dir/zero.apk" bs=1 count="$length" status=none
    reject "$signer" verify "$signing_test_dir/zero.apk"
done
echo 'PASS: native APK v2 framing, independent OpenSSL signature/digest oracle, deterministic signing, 1 MiB boundaries, large central directory, descriptors, identity matching, private-key descriptor policy and mutation rejection'
