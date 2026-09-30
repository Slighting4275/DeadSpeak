#define _POSIX_C_SOURCE 200809L
#include "explore.h"
#include "jni_scope.h"

#include <android/log.h>
#include <ctype.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DS_EXPLORE_LOG "DeadSpeakExplore"
#define DS_EXPLORE_LIMIT 8192
#define DS_EXPLORE_CACHE_SECONDS (30 * 24 * 60 * 60)
#define DS_WIKI_ROOT "https://en.wikipedia.org/w/api.php?action=query&list=categorymembers&cmtitle=Category%3AReportedly_haunted_locations_by_country&cmtype=subcat&cmlimit=100&format=json&maxlag=5"

typedef struct {
    JNIEnv *env;
    jclass url_class;
    jclass url_connection_class;
    jclass http_connection_class;
    jclass byte_stream_class;
    jclass json_object_class;
    jclass json_array_class;
    jclass iterator_class;
    jmethodID url_constructor;
    jmethodID url_open_connection;
    jmethodID set_connect_timeout;
    jmethodID set_read_timeout;
    jmethodID set_request_property;
    jmethodID set_use_caches;
    jmethodID get_response_code;
    jmethodID get_input_stream;
    jmethodID disconnect;
    jmethodID stream_read;
    jmethodID stream_close;
    jmethodID byte_stream_constructor;
    jmethodID byte_stream_write;
    jmethodID byte_stream_to_string;
    jmethodID json_constructor;
    jmethodID json_get_object;
    jmethodID json_get_array;
    jmethodID json_get_int;
    jmethodID json_get_string;
    jmethodID json_opt_string;
    jmethodID json_has;
    jmethodID json_opt_object;
    jmethodID json_opt_array;
    jmethodID json_keys;
    jmethodID json_get_double;
    jmethodID array_opt_object;
    jmethodID array_length;
    jmethodID array_get_object;
    jmethodID iterator_has_next;
    jmethodID iterator_next;
} DSJni;

static void clear_exception(JNIEnv *env) {
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
}

static bool jni_methods(DSJni *jni, JNIEnv *env) {
    memset(jni, 0, sizeof(*jni));
    jni->env = env;
    jni->url_class = (*env)->NewGlobalRef(env, (*env)->FindClass(env, "java/net/URL"));
    jni->url_connection_class = (*env)->NewGlobalRef(env, (*env)->FindClass(env, "java/net/URLConnection"));
    jni->http_connection_class = (*env)->NewGlobalRef(env, (*env)->FindClass(env, "java/net/HttpURLConnection"));
    jni->byte_stream_class = (*env)->NewGlobalRef(env, (*env)->FindClass(env, "java/io/ByteArrayOutputStream"));
    jni->json_object_class = (*env)->NewGlobalRef(env, (*env)->FindClass(env, "org/json/JSONObject"));
    jni->json_array_class = (*env)->NewGlobalRef(env, (*env)->FindClass(env, "org/json/JSONArray"));
    jni->iterator_class = (*env)->NewGlobalRef(env, (*env)->FindClass(env, "java/util/Iterator"));
    jclass input_stream_class = (*env)->FindClass(env, "java/io/InputStream");
    clear_exception(env);
    if (!jni->url_class || !jni->url_connection_class || !jni->http_connection_class ||
        !jni->byte_stream_class || !jni->json_object_class || !jni->json_array_class || !jni->iterator_class || !input_stream_class) {
        if (input_stream_class) (*env)->DeleteLocalRef(env, input_stream_class);
        return false;
    }

    jni->url_constructor = (*env)->GetMethodID(env, jni->url_class, "<init>", "(Ljava/lang/String;)V");
    jni->url_open_connection = (*env)->GetMethodID(env, jni->url_class, "openConnection", "()Ljava/net/URLConnection;");
    jni->set_connect_timeout = (*env)->GetMethodID(env, jni->url_connection_class, "setConnectTimeout", "(I)V");
    jni->set_read_timeout = (*env)->GetMethodID(env, jni->url_connection_class, "setReadTimeout", "(I)V");
    jni->set_request_property = (*env)->GetMethodID(env, jni->url_connection_class, "setRequestProperty",
                                                    "(Ljava/lang/String;Ljava/lang/String;)V");
    jni->set_use_caches = (*env)->GetMethodID(env, jni->url_connection_class, "setUseCaches", "(Z)V");
    jni->get_response_code = (*env)->GetMethodID(env, jni->http_connection_class, "getResponseCode", "()I");
    jni->get_input_stream = (*env)->GetMethodID(env, jni->url_connection_class, "getInputStream", "()Ljava/io/InputStream;");
    jni->disconnect = (*env)->GetMethodID(env, jni->http_connection_class, "disconnect", "()V");
    jni->stream_read = (*env)->GetMethodID(env, input_stream_class, "read", "([B)I");
    jni->stream_close = (*env)->GetMethodID(env, input_stream_class, "close", "()V");
    jni->byte_stream_constructor = (*env)->GetMethodID(env, jni->byte_stream_class, "<init>", "()V");
    jni->byte_stream_write = (*env)->GetMethodID(env, jni->byte_stream_class, "write", "([BII)V");
    jni->byte_stream_to_string = (*env)->GetMethodID(env, jni->byte_stream_class, "toString", "(Ljava/lang/String;)Ljava/lang/String;");
    jni->json_constructor = (*env)->GetMethodID(env, jni->json_object_class, "<init>", "(Ljava/lang/String;)V");
    jni->json_get_object = (*env)->GetMethodID(env, jni->json_object_class, "getJSONObject", "(Ljava/lang/String;)Lorg/json/JSONObject;");
    jni->json_get_array = (*env)->GetMethodID(env, jni->json_object_class, "getJSONArray", "(Ljava/lang/String;)Lorg/json/JSONArray;");
    jni->json_get_int = (*env)->GetMethodID(env, jni->json_object_class, "getInt", "(Ljava/lang/String;)I");
    jni->json_get_string = (*env)->GetMethodID(env, jni->json_object_class, "getString", "(Ljava/lang/String;)Ljava/lang/String;");
    jni->json_opt_string = (*env)->GetMethodID(env, jni->json_object_class, "optString", "(Ljava/lang/String;)Ljava/lang/String;");
    jni->json_has = (*env)->GetMethodID(env, jni->json_object_class, "has", "(Ljava/lang/String;)Z");
    jni->json_opt_object = (*env)->GetMethodID(env, jni->json_object_class, "optJSONObject", "(Ljava/lang/String;)Lorg/json/JSONObject;");
    jni->json_opt_array = (*env)->GetMethodID(env, jni->json_object_class, "optJSONArray", "(Ljava/lang/String;)Lorg/json/JSONArray;");
    jni->json_keys = (*env)->GetMethodID(env, jni->json_object_class, "keys", "()Ljava/util/Iterator;");
    jni->json_get_double = (*env)->GetMethodID(env, jni->json_object_class, "getDouble", "(Ljava/lang/String;)D");
    jni->array_opt_object = (*env)->GetMethodID(env, jni->json_array_class, "optJSONObject", "(I)Lorg/json/JSONObject;");
    jni->array_length = (*env)->GetMethodID(env, jni->json_array_class, "length", "()I");
    jni->array_get_object = (*env)->GetMethodID(env, jni->json_array_class, "getJSONObject", "(I)Lorg/json/JSONObject;");
    jni->iterator_has_next = (*env)->GetMethodID(env, jni->iterator_class, "hasNext", "()Z");
    jni->iterator_next = (*env)->GetMethodID(env, jni->iterator_class, "next", "()Ljava/lang/Object;");
    if (input_stream_class) (*env)->DeleteLocalRef(env, input_stream_class);
    clear_exception(env);
    return jni->url_constructor && jni->url_open_connection && jni->set_connect_timeout &&
           jni->set_read_timeout && jni->set_request_property && jni->set_use_caches && jni->get_response_code && jni->get_input_stream &&
           jni->disconnect && jni->stream_read && jni->stream_close && jni->byte_stream_constructor &&
           jni->byte_stream_write && jni->byte_stream_to_string && jni->json_constructor &&
           jni->json_get_object && jni->json_get_array && jni->json_get_int &&
           jni->json_get_string && jni->json_opt_string && jni->json_has && jni->json_opt_object && jni->json_opt_array && jni->json_keys &&
           jni->json_get_double && jni->array_opt_object && jni->array_length && jni->array_get_object &&
           jni->iterator_has_next && jni->iterator_next;
}

static void release_jni(DSJni *jni) {
    JNIEnv *env = jni->env;
    jobject refs[] = {jni->url_class, jni->url_connection_class, jni->http_connection_class,
                      jni->byte_stream_class, jni->json_object_class, jni->json_array_class, jni->iterator_class};
    for (size_t i = 0; i < sizeof(refs) / sizeof(refs[0]); ++i) {
        if (refs[i]) (*env)->DeleteGlobalRef(env, refs[i]);
    }
}

static jobject request_json(DSJni *jni, const char *url_text) {
    JNIEnv *env = jni->env;
    jobject connection = NULL;
    jobject input = NULL;
    jobject output = NULL;
    jbyteArray chunk = NULL;
    jobject result = NULL;
    jstring url_string = (*env)->NewStringUTF(env, url_text);
    jstring user_agent_key = (*env)->NewStringUTF(env, "User-Agent");
    jstring user_agent_value = (*env)->NewStringUTF(env, "DeadSpeak/0.1 (https://codeberg.org/Exalted6165/DeadSpeak)");
    jstring utf8 = (*env)->NewStringUTF(env, "UTF-8");
    jobject url = (*env)->NewObject(env, jni->url_class, jni->url_constructor, url_string);
    if (url) connection = (*env)->CallObjectMethod(env, url, jni->url_open_connection);
    if (!connection || (*env)->ExceptionCheck(env)) goto cleanup;
    (*env)->CallVoidMethod(env, connection, jni->set_connect_timeout, 6000);
    (*env)->CallVoidMethod(env, connection, jni->set_read_timeout, 12000);
    (*env)->CallVoidMethod(env, connection, jni->set_use_caches, JNI_FALSE);
    (*env)->CallVoidMethod(env, connection, jni->set_request_property, user_agent_key, user_agent_value);
    if ((*env)->ExceptionCheck(env)) goto cleanup;
    const jint response_code = (*env)->CallIntMethod(env, connection, jni->get_response_code);
    if ((*env)->ExceptionCheck(env)) goto cleanup;
    if (response_code != 200) {
        __android_log_print(ANDROID_LOG_WARN, DS_EXPLORE_LOG, "HTTP %d fetching %s", response_code, url_text);
        goto cleanup;
    }
    input = (*env)->CallObjectMethod(env, connection, jni->get_input_stream);
    if (!input || (*env)->ExceptionCheck(env)) goto cleanup;
    output = (*env)->NewObject(env, jni->byte_stream_class, jni->byte_stream_constructor);
    chunk = (*env)->NewByteArray(env, 4096);
    if (!output || !chunk) goto cleanup;
    size_t total = 0;
    while (total < 2U * 1024U * 1024U) {
        const jint received = (*env)->CallIntMethod(env, input, jni->stream_read, chunk);
        if ((*env)->ExceptionCheck(env) || received <= 0) break;
        (*env)->CallVoidMethod(env, output, jni->byte_stream_write, chunk, 0, received);
        if ((*env)->ExceptionCheck(env)) break;
        total += (size_t)received;
    }
    if (total > 0 && !(*env)->ExceptionCheck(env)) {
        jstring body = (jstring)(*env)->CallObjectMethod(env, output, jni->byte_stream_to_string, utf8);
        if (body && !(*env)->ExceptionCheck(env)) {
            result = (*env)->NewObject(env, jni->json_object_class, jni->json_constructor, body);
        }
        if (body) (*env)->DeleteLocalRef(env, body);
    }

cleanup:
    clear_exception(env);
    if (input) (*env)->CallVoidMethod(env, input, jni->stream_close);
    if (connection) (*env)->CallVoidMethod(env, connection, jni->disconnect);
    clear_exception(env);
    if (chunk) (*env)->DeleteLocalRef(env, chunk);
    if (output) (*env)->DeleteLocalRef(env, output);
    if (input) (*env)->DeleteLocalRef(env, input);
    if (connection) (*env)->DeleteLocalRef(env, connection);
    if (url) (*env)->DeleteLocalRef(env, url);
    if (url_string) (*env)->DeleteLocalRef(env, url_string);
    if (user_agent_key) (*env)->DeleteLocalRef(env, user_agent_key);
    if (user_agent_value) (*env)->DeleteLocalRef(env, user_agent_value);
    if (utf8) (*env)->DeleteLocalRef(env, utf8);
    return result;
}

static void ascii_label(const char *input, char *output, size_t capacity) {
    size_t out = 0;
    for (size_t i = 0; input[i] && out + 1 < capacity;) {
        const unsigned char first = (unsigned char)input[i++];
        if (first < 0x80) {
            output[out++] = (char)toupper(first);
        } else if ((first == 0xC3 || first == 0xC4 || first == 0xC5) && input[i]) {
            const unsigned char second = (unsigned char)input[i++];
            char replacement = ' ';
            if (first == 0xC3) {
                if ((second >= 0x80 && second <= 0x85) || (second >= 0xA0 && second <= 0xA5)) replacement = 'A';
                else if (second == 0x87 || second == 0xA7) replacement = 'C';
                else if ((second >= 0x88 && second <= 0x8B) || (second >= 0xA8 && second <= 0xAB)) replacement = 'E';
                else if ((second >= 0x8C && second <= 0x8F) || (second >= 0xAC && second <= 0xAF)) replacement = 'I';
                else if (second == 0x91 || second == 0xB1) replacement = 'N';
                else if ((second >= 0x92 && second <= 0x96) || (second >= 0xB2 && second <= 0xB6)) replacement = 'O';
                else if ((second >= 0x99 && second <= 0x9C) || (second >= 0xB9 && second <= 0xBC)) replacement = 'U';
                else if (second == 0x9D || second == 0xBD) replacement = 'Y';
                else if (second == 0x9F) replacement = 'S';
            }
            output[out++] = replacement;
        } else if ((first & 0xE0) == 0xC0 && input[i]) {
            ++i;
            output[out++] = ' ';
        } else if ((first & 0xF0) == 0xE0 && input[i] && input[i + 1]) {
            i += 2;
            output[out++] = ' ';
        } else if ((first & 0xF8) == 0xF0 && input[i] && input[i + 1] && input[i + 2]) {
            i += 3;
            output[out++] = ' ';
        }
    }
    output[out] = '\0';
}

static bool contains_folded(const char *text, const char *query) {
    if (!query || !query[0]) return true;
    for (size_t i = 0; text[i]; ++i) {
        size_t j = 0;
        while (query[j] && text[i + j] && toupper((unsigned char)text[i + j]) == toupper((unsigned char)query[j])) ++j;
        if (!query[j]) return true;
    }
    return false;
}

static jobject category_members(DSJni *jni, jobject root, const char *key) {
    JNIEnv *env = jni->env;
    jstring query_key = (*env)->NewStringUTF(env, "query");
    jobject query = (*env)->CallObjectMethod(env, root, jni->json_get_object, query_key);
    (*env)->DeleteLocalRef(env, query_key);
    if (!query || (*env)->ExceptionCheck(env)) {
        clear_exception(env);
        return NULL;
    }
    jstring members_key = (*env)->NewStringUTF(env, key);
    jobject members = (*env)->CallObjectMethod(env, query, jni->json_get_array, members_key);
    (*env)->DeleteLocalRef(env, members_key);
    (*env)->DeleteLocalRef(env, query);
    if ((*env)->ExceptionCheck(env)) {
        clear_exception(env);
        return NULL;
    }
    return members;
}

static bool add_place(DSExplore *explore, int64_t page_id, const char *region, const char *title,
                      const char *wikidata_id, bool has_coordinates, double latitude, double longitude) {
    if (page_id <= 0 || !title || !title[0]) return false;
    char clean_title[128], clean_region[96];
    ascii_label(title, clean_title, sizeof(clean_title));
    ascii_label(region, clean_region, sizeof(clean_region));
    pthread_mutex_lock(&explore->mutex);
    if (explore->count >= DS_EXPLORE_LIMIT) {
        pthread_mutex_unlock(&explore->mutex);
        return false;
    }
    for (size_t i = 0; i < explore->count; ++i) {
        if (explore->places[i].page_id == page_id) {
            pthread_mutex_unlock(&explore->mutex);
            return false;
        }
    }
    DSExplorePlace *place = &explore->places[explore->count++];
    place->page_id = page_id;
    snprintf(place->title, sizeof(place->title), "%s", clean_title);
    snprintf(place->region, sizeof(place->region), "%s", clean_region);
    if (wikidata_id && wikidata_id[0] == 'Q') snprintf(place->wikidata_id, sizeof(place->wikidata_id), "%s", wikidata_id);
    place->has_coordinates = has_coordinates;
    place->latitude = latitude;
    place->longitude = longitude;
    pthread_mutex_unlock(&explore->mutex);
    return true;
}

static size_t load_cache(DSExplore *explore, time_t *modified) {
    struct stat file_stat;
    if (stat(explore->cache_path, &file_stat) != 0) return 0;
    FILE *file = fopen(explore->cache_path, "r");
    if (!file) return 0;
    char line[512];
    size_t count = 0;
    while (fgets(line, sizeof(line), file) && count < DS_EXPLORE_LIMIT) {
        char *field[7];
        field[0] = line;
        size_t fields = 1;
        for (char *p = line; *p && fields < 7; ++p) if (*p == '\t') { *p = '\0'; field[fields++] = p + 1; }
        if (fields != 7) continue;
        char *end = strpbrk(field[6], "\r\n");
        if (end) *end = '\0';
        if (add_place(explore, strtoll(field[0], NULL, 10), field[1], field[2], field[3],
                      atoi(field[4]) != 0, strtod(field[5], NULL), strtod(field[6], NULL))) ++count;
    }
    fclose(file);
    if (modified) *modified = file_stat.st_mtime;
    return count;
}

static void save_cache(DSExplore *explore) {
    char temp_path[560];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", explore->cache_path);
    FILE *file = fopen(temp_path, "w");
    if (!file) return;
    pthread_mutex_lock(&explore->mutex);
    for (size_t i = 0; i < explore->count; ++i) {
        fprintf(file, "%lld\t%s\t%s\t%s\t%d\t%.7f\t%.7f\n", (long long)explore->places[i].page_id,
                explore->places[i].region, explore->places[i].title, explore->places[i].wikidata_id,
                explore->places[i].has_coordinates ? 1 : 0, explore->places[i].latitude, explore->places[i].longitude);
    }
    pthread_mutex_unlock(&explore->mutex);
    const bool okay = fclose(file) == 0;
    if (okay) rename(temp_path, explore->cache_path);
    else unlink(temp_path);
}

static bool api_get_categories(DSJni *jni, jobject root, int64_t *ids, char names[][96], size_t *count) {
    JNIEnv *env = jni->env;
    jobject members = category_members(jni, root, "categorymembers");
    if (!members) return false;
    const jint length = (*env)->CallIntMethod(env, members, jni->array_length);
    const jstring title_key = (*env)->NewStringUTF(env, "title");
    const jstring id_key = (*env)->NewStringUTF(env, "pageid");
    const char *prefix = "CATEGORY:REPORTEDLY HAUNTED LOCATIONS IN ";
    for (jint i = 0; i < length && *count < 100; ++i) {
        jobject item = (*env)->CallObjectMethod(env, members, jni->array_get_object, i);
        if (!item) { clear_exception(env); continue; }
        jstring title = (jstring)(*env)->CallObjectMethod(env, item, jni->json_get_string, title_key);
        jint page_id = (*env)->CallIntMethod(env, item, jni->json_get_int, id_key);
        if (title && page_id > 0 && !(*env)->ExceptionCheck(env)) {
            const char *chars = (*env)->GetStringUTFChars(env, title, NULL);
            char normalized[192];
            if (chars) ascii_label(chars, normalized, sizeof(normalized));
            if (chars && strncmp(normalized, prefix, strlen(prefix)) == 0) {
                ids[*count] = page_id;
                snprintf(names[*count], 96, "%s", normalized + strlen(prefix));
                ++*count;
            }
            if (chars) (*env)->ReleaseStringUTFChars(env, title, chars);
        }
        clear_exception(env);
        if (title) (*env)->DeleteLocalRef(env, title);
        (*env)->DeleteLocalRef(env, item);
    }
    (*env)->DeleteLocalRef(env, title_key);
    (*env)->DeleteLocalRef(env, id_key);
    (*env)->DeleteLocalRef(env, members);
    return *count > 0;
}

static bool api_get_places(DSJni *jni, DSExplore *explore, int64_t category_id, const char *region) {
    char url[512];
    snprintf(url, sizeof(url), "https://en.wikipedia.org/w/api.php?action=query&generator=categorymembers&gcmpageid=%lld&gcmtype=page&gcmlimit=500&prop=coordinates%%7Cpageprops&ppprop=wikibase_item&format=json&maxlag=5",
             (long long)category_id);
    jobject root = request_json(jni, url);
    if (!root) return false;
    JNIEnv *env = jni->env;
    jstring query_key = (*env)->NewStringUTF(env, "query");
    if (!query_key) { clear_exception(env); (*env)->DeleteLocalRef(env, root); return false; }
    jobject query = (*env)->CallObjectMethod(env, root, jni->json_opt_object, query_key);
    (*env)->DeleteLocalRef(env, query_key);
    if (!query || (*env)->ExceptionCheck(env)) {
        bool empty = false;
        if (!(*env)->ExceptionCheck(env)) {
            jstring complete_key = (*env)->NewStringUTF(env, "batchcomplete");
            if (complete_key) empty = (*env)->CallBooleanMethod(env, root, jni->json_has, complete_key);
            if (complete_key) (*env)->DeleteLocalRef(env, complete_key);
        }
        const bool okay = empty && !(*env)->ExceptionCheck(env);
        clear_exception(env);
        if (query) (*env)->DeleteLocalRef(env, query);
        (*env)->DeleteLocalRef(env, root);
        return okay;
    }
    jstring pages_key = (*env)->NewStringUTF(env, "pages");
    if (!pages_key) {
        clear_exception(env);
        (*env)->DeleteLocalRef(env, query);
        (*env)->DeleteLocalRef(env, root);
        return false;
    }
    jobject pages = (*env)->CallObjectMethod(env, query, jni->json_opt_object, pages_key);
    (*env)->DeleteLocalRef(env, pages_key);
    if (query) (*env)->DeleteLocalRef(env, query);
    jobject keys = pages && !(*env)->ExceptionCheck(env) ? (*env)->CallObjectMethod(env, pages, jni->json_keys) : NULL;
    if (!pages || !keys) {
        if (pages) (*env)->DeleteLocalRef(env, pages);
        (*env)->DeleteLocalRef(env, root);
        clear_exception(env);
        return false;
    }
    const jstring lat_key = (*env)->NewStringUTF(env, "lat");
    const jstring lon_key = (*env)->NewStringUTF(env, "lon");
    const jstring props_key = (*env)->NewStringUTF(env, "pageprops");
    const jstring qid_key = (*env)->NewStringUTF(env, "wikibase_item");
    const jstring coordinates_key = (*env)->NewStringUTF(env, "coordinates");
    const jstring id_key = (*env)->NewStringUTF(env, "pageid");
    const jstring title_key = (*env)->NewStringUTF(env, "title");
    bool okay = true;
    while ((*env)->CallBooleanMethod(env, keys, jni->iterator_has_next)) {
        jstring key = (jstring)(*env)->CallObjectMethod(env, keys, jni->iterator_next);
        jobject item = key && !(*env)->ExceptionCheck(env) ? (*env)->CallObjectMethod(env, pages, jni->json_get_object, key) : NULL;
        if (key) (*env)->DeleteLocalRef(env, key);
        if (!item || (*env)->ExceptionCheck(env)) {
            if (item) (*env)->DeleteLocalRef(env, item);
            okay = false;
            clear_exception(env);
            break;
        }
        jstring title = NULL, qid = NULL;
        jobject props = NULL, coords = NULL, coord = NULL;
        const jint page_id = (*env)->CallIntMethod(env, item, jni->json_get_int, id_key);
        if ((*env)->ExceptionCheck(env) || page_id <= 0) { okay = false; goto item_cleanup; }
        title = (jstring)(*env)->CallObjectMethod(env, item, jni->json_get_string, title_key);
        if (!title || (*env)->ExceptionCheck(env)) { okay = false; goto item_cleanup; }
        props = (*env)->CallObjectMethod(env, item, jni->json_opt_object, props_key);
        if ((*env)->ExceptionCheck(env)) { okay = false; goto item_cleanup; }
        qid = props ? (jstring)(*env)->CallObjectMethod(env, props, jni->json_opt_string, qid_key) : NULL;
        if ((*env)->ExceptionCheck(env)) { okay = false; goto item_cleanup; }
        char qid_text[16] = {0};
        if (qid) {
            const char *q = (*env)->GetStringUTFChars(env, qid, NULL);
            if (!q) { okay = false; goto item_cleanup; }
            snprintf(qid_text, sizeof(qid_text), "%s", q);
            (*env)->ReleaseStringUTFChars(env, qid, q);
        }
        coords = (*env)->CallObjectMethod(env, item, jni->json_opt_array, coordinates_key);
        if ((*env)->ExceptionCheck(env)) { okay = false; goto item_cleanup; }
        coord = coords ? (*env)->CallObjectMethod(env, coords, jni->array_opt_object, 0) : NULL;
        if ((*env)->ExceptionCheck(env)) { okay = false; goto item_cleanup; }
        bool has_coords = false;
        double latitude = 0.0, longitude = 0.0;
        if (coord) {
            latitude = (*env)->CallDoubleMethod(env, coord, jni->json_get_double, lat_key);
            if ((*env)->ExceptionCheck(env)) { okay = false; goto item_cleanup; }
            longitude = (*env)->CallDoubleMethod(env, coord, jni->json_get_double, lon_key);
            if ((*env)->ExceptionCheck(env)) { okay = false; goto item_cleanup; }
            has_coords = true;
        }
        if (!(*env)->ExceptionCheck(env) && page_id > 0 && title) {
            const char *chars = (*env)->GetStringUTFChars(env, title, NULL);
            if (!chars) { okay = false; goto item_cleanup; }
            add_place(explore, page_id, region, chars, qid_text, has_coords, latitude, longitude);
            (*env)->ReleaseStringUTFChars(env, title, chars);
        }
item_cleanup:
        clear_exception(env);
        if (title) (*env)->DeleteLocalRef(env, title);
        if (qid) (*env)->DeleteLocalRef(env, qid);
        if (props) (*env)->DeleteLocalRef(env, props);
        if (coords) (*env)->DeleteLocalRef(env, coords);
        if (coord) (*env)->DeleteLocalRef(env, coord);
        (*env)->DeleteLocalRef(env, item);
        pthread_mutex_lock(&explore->mutex);
        const bool full = explore->count >= DS_EXPLORE_LIMIT;
        pthread_mutex_unlock(&explore->mutex);
        if (full || !okay) break;
    }
    if ((*env)->ExceptionCheck(env)) okay = false;
    clear_exception(env);
    (*env)->DeleteLocalRef(env, title_key);
    (*env)->DeleteLocalRef(env, id_key);
    (*env)->DeleteLocalRef(env, coordinates_key);
    (*env)->DeleteLocalRef(env, qid_key);
    (*env)->DeleteLocalRef(env, props_key);
    (*env)->DeleteLocalRef(env, lat_key);
    (*env)->DeleteLocalRef(env, lon_key);
    (*env)->DeleteLocalRef(env, keys);
    (*env)->DeleteLocalRef(env, pages);
    (*env)->DeleteLocalRef(env, root);
    return okay;
}

/* Publish a complete download under one lock, replacing updated/removed pages. */
static void publish_places(DSExplore *explore, DSExplore *fresh) {
    pthread_mutex_lock(&explore->mutex);
    DSExplorePlace *old = explore->places;
    const size_t old_count = explore->count;
    explore->places = fresh->places;
    explore->count = fresh->count;
    fresh->places = old;
    fresh->count = old_count;
    pthread_mutex_unlock(&explore->mutex);
}

static void *explore_thread(void *context) {
    DSExplore *explore = (DSExplore *)context;
    time_t cache_modified = 0;
    const size_t cached = explore->force_refresh ? 0 : load_cache(explore, &cache_modified);
    const time_t now = time(NULL);
    if (!explore->force_refresh && cached > 0 && now - cache_modified < DS_EXPLORE_CACHE_SECONDS) {
        atomic_store(&explore->loading, false);
        return NULL;
    }

    DSExplore fresh;
    if (!ds_explore_init(&fresh)) {
        ds_explore_destroy(&fresh);
        goto failed;
    }
    JavaVM *vm = explore->activity->vm;
    JNIEnv *env = NULL;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK || !env) {
        ds_explore_destroy(&fresh);
        goto failed;
    }
    DSJni jni;
    if (!jni_methods(&jni, env)) {
        clear_exception(env);
        release_jni(&jni);
        (*vm)->DetachCurrentThread(vm);
        ds_explore_destroy(&fresh);
        goto failed;
    }

    jobject root = request_json(&jni, DS_WIKI_ROOT);
    if (!root) {
        release_jni(&jni);
        (*vm)->DetachCurrentThread(vm);
        ds_explore_destroy(&fresh);
        goto failed;
    }
    int64_t category_ids[100];
    char regions[100][96];
    size_t category_count = 0;
    const bool categories_found = api_get_categories(&jni, root, category_ids, regions, &category_count);
    (*env)->DeleteLocalRef(env, root);
    if (!categories_found) {
        release_jni(&jni);
        (*vm)->DetachCurrentThread(vm);
        ds_explore_destroy(&fresh);
        goto failed;
    }

    bool complete = true;
    for (size_t i = 0; i < category_count; ++i) {
        if (!api_get_places(&jni, &fresh, category_ids[i], regions[i])) {
            __android_log_print(ANDROID_LOG_WARN, DS_EXPLORE_LOG, "Wikipedia category %lld could not be refreshed", (long long)category_ids[i]);
            complete = false;
            break;
        }
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 250000000L};
        nanosleep(&delay, NULL);
    }
    if ((*env)->ExceptionCheck(env)) complete = false;
    clear_exception(env);
    release_jni(&jni);
    (*vm)->DetachCurrentThread(vm);
    if (!complete) { ds_explore_destroy(&fresh); goto failed; }
    publish_places(explore, &fresh);
    ds_explore_destroy(&fresh);
    save_cache(explore);
    __android_log_print(ANDROID_LOG_INFO, DS_EXPLORE_LOG, "Published %zu places from %zu Wikipedia categories", explore->count, category_count);
    atomic_store(&explore->failed, false);
    atomic_store(&explore->loading, false);
    return NULL;

failed:
    __android_log_print(ANDROID_LOG_WARN, DS_EXPLORE_LOG, "Wikipedia place refresh failed");
    atomic_store(&explore->failed, true);
    atomic_store(&explore->loading, false);
    return NULL;
}

bool ds_explore_init(DSExplore *explore) {
    memset(explore, 0, sizeof(*explore));
    if (pthread_mutex_init(&explore->mutex, NULL) != 0) return false;
    explore->mutex_initialized = true;
    explore->places = (DSExplorePlace *)calloc(DS_EXPLORE_LIMIT, sizeof(*explore->places));
    if (!explore->places) return false;
    return true;
}

void ds_explore_start(DSExplore *explore, ANativeActivity *activity, const char *cache_path) {
    if (!explore->places) return;
    if (explore->thread_created) {
        if (atomic_load(&explore->loading)) return;
        pthread_join(explore->thread, NULL);
        explore->thread_created = false;
    }
    if (explore->requested_once) return;
    explore->activity = activity;
    snprintf(explore->cache_path, sizeof(explore->cache_path), "%s", cache_path);
    explore->requested_once = true;
    explore->force_refresh = false;
    atomic_store(&explore->failed, false);
    atomic_store(&explore->loading, true);
    if (pthread_create(&explore->thread, NULL, explore_thread, explore) != 0) {
        atomic_store(&explore->loading, false);
        atomic_store(&explore->failed, true);
        return;
    }
    explore->thread_created = true;
}

void ds_explore_refresh(DSExplore *explore, ANativeActivity *activity, const char *cache_path) {
    if (!explore->places) return;
    if (explore->thread_created) {
        if (atomic_load(&explore->loading)) return;
        pthread_join(explore->thread, NULL);
        explore->thread_created = false;
    }
    explore->activity = activity;
    snprintf(explore->cache_path, sizeof(explore->cache_path), "%s", cache_path);
    explore->requested_once = true;
    explore->force_refresh = true;
    atomic_store(&explore->failed, false);
    atomic_store(&explore->loading, true);
    if (pthread_create(&explore->thread, NULL, explore_thread, explore) != 0) {
        atomic_store(&explore->loading, false);
        atomic_store(&explore->failed, true);
        return;
    }
    explore->thread_created = true;
}

void ds_explore_stop(DSExplore *explore) {
    if (explore->thread_created) {
        pthread_join(explore->thread, NULL);
        explore->thread_created = false;
    }
}

void ds_explore_destroy(DSExplore *explore) {
    ds_explore_stop(explore);
    if (explore->places) free(explore->places);
    if (explore->mutex_initialized) pthread_mutex_destroy(&explore->mutex);
    memset(explore, 0, sizeof(*explore));
}

size_t ds_explore_page(DSExplore *explore, const char *query, size_t page, size_t page_size,
                       DSExplorePlace out[DS_EXPLORE_PAGE_SIZE], size_t *match_count) {
    size_t matches = 0;
    size_t copied = 0;
    if (page_size == 0 || page_size > DS_EXPLORE_PAGE_SIZE) {
        if (match_count) *match_count = 0;
        return 0;
    }
    const size_t first = page <= SIZE_MAX / page_size ? page * page_size : SIZE_MAX;
    pthread_mutex_lock(&explore->mutex);
    for (size_t i = 0; i < explore->count; ++i) {
        if (!contains_folded(explore->places[i].title, query) &&
            !contains_folded(explore->places[i].region, query)) continue;
        if (matches >= first && copied < page_size) out[copied++] = explore->places[i];
        ++matches;
    }
    pthread_mutex_unlock(&explore->mutex);
    if (match_count) *match_count = matches;
    return copied;
}

static bool open_url(ANativeActivity *activity, const char *url) {
    DSJniScope scope;
    if (!ds_jni_scope_begin(&scope, activity)) return false;
    JNIEnv *env = scope.env;
    bool opened = false;
    jclass uri_class = (*env)->FindClass(env, "android/net/Uri");
    if (!uri_class) goto done;
    jclass intent_class = (*env)->FindClass(env, "android/content/Intent");
    if (!intent_class) goto done;
    jmethodID parse = (*env)->GetStaticMethodID(env, uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    if (!parse) goto done;
    jmethodID constructor = (*env)->GetMethodID(env, intent_class, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
    if (!constructor) goto done;
    jclass activity_class = (*env)->GetObjectClass(env, activity->clazz);
    if (!activity_class) goto done;
    jmethodID start_activity = (*env)->GetMethodID(env, activity_class, "startActivity", "(Landroid/content/Intent;)V");
    if (!start_activity) goto done;
    jstring action = (*env)->NewStringUTF(env, "android.intent.action.VIEW");
    if (!action) goto done;
    jstring url_string = (*env)->NewStringUTF(env, url);
    if (!url_string) goto done;
    jobject uri = (*env)->CallStaticObjectMethod(env, uri_class, parse, url_string);
    if (!uri || (*env)->ExceptionCheck(env)) goto done;
    jobject intent = (*env)->NewObject(env, intent_class, constructor, action, uri);
    if (!intent || (*env)->ExceptionCheck(env)) goto done;
    (*env)->CallVoidMethod(env, activity->clazz, start_activity, intent);
    opened = !(*env)->ExceptionCheck(env);
done:
    return ds_jni_scope_end(&scope) && opened;
}

bool ds_explore_open_article(ANativeActivity *activity, int64_t page_id) {
    if (!activity || page_id <= 0) return false;
    char url[96];
    snprintf(url, sizeof(url), "https://en.wikipedia.org/?curid=%lld", (long long)page_id);
    return open_url(activity, url);
}

bool ds_explore_open_wikidata(ANativeActivity *activity, const char *wikidata_id) {
    if (!activity || !wikidata_id || wikidata_id[0] != 'Q' || !wikidata_id[1]) return false;
    for (const char *p = wikidata_id + 1; *p; ++p) if (!isdigit((unsigned char)*p)) return false;
    char url[96];
    snprintf(url, sizeof(url), "https://www.wikidata.org/wiki/%s", wikidata_id);
    return open_url(activity, url);
}
