#ifndef DS_PLATFORM_SYMBOLS_H
#define DS_PLATFORM_SYMBOLS_H
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <dirent.h>

/* A resource-ID header for AAPT2, not a replacement Android framework. Its
   contents are never shipped; references continue to use OS resource IDs. */
typedef struct { char type[32], name[256], id[32]; } DSSymbol;
static DSSymbol ds_symbols[1024];
static size_t ds_symbol_count;
static const char ds_android_namespace[] = "http://schemas.android.com/apk/res/android";

static void ds_symbol_add(const char *type, const char *name) {
    if (!type || !name || !*name || strlen(type) >= sizeof(ds_symbols[0].type) ||
        strlen(name) >= sizeof(ds_symbols[0].name)) fail("invalid framework resource reference");
    for (size_t i = 0; i < ds_symbol_count; ++i)
        if (!strcmp(ds_symbols[i].type, type) && !strcmp(ds_symbols[i].name, name)) return;
    if (ds_symbol_count == sizeof(ds_symbols) / sizeof(ds_symbols[0])) fail("too many framework symbols");
    DSSymbol *symbol = &ds_symbols[ds_symbol_count++];
    strcpy(symbol->type, type);
    strcpy(symbol->name, name);
}

static void ds_symbol_reference(const char *value) {
    if (!value) return;
    if (*value == '@' || *value == '?') ++value;
    if (strncmp(value, "android:", 8) != 0) return;
    value += 8;
    const char *slash = strchr(value, '/');
    if (!slash) { ds_symbol_add("attr", value); return; }
    char type[32];
    size_t length = (size_t)(slash - value);
    if (length == 0 || length >= sizeof(type)) fail("invalid framework resource type");
    memcpy(type, value, length);
    type[length] = '\0';
    ds_symbol_add(type, slash + 1);
}

static void ds_symbol_scan_nodes(xmlNode *node) {
    for (; node; node = node->next) {
        if (node->type == XML_ELEMENT_NODE) {
            for (xmlAttr *attr = node->properties; attr; attr = attr->next) {
                if (attr->ns && attr->ns->href && !xmlStrcmp(attr->ns->href, BAD_CAST ds_android_namespace))
                    ds_symbol_add("attr", (const char *)attr->name);
                xmlChar *value = xmlNodeListGetString(node->doc, attr->children, 1);
                ds_symbol_reference((const char *)value);
                xmlFree(value);
            }
        } else if (node->type == XML_TEXT_NODE) {
            ds_symbol_reference((const char *)node->content);
        }
        ds_symbol_scan_nodes(node->children);
    }
}

static xmlDoc *ds_xml_read(const char *filename) {
    /* Never resolve external entities, fetch DTDs, or enable entity expansion. */
    xmlDoc *doc = xmlReadFile(filename, NULL, XML_PARSE_NONET | XML_PARSE_NOBLANKS);
    if (!doc || doc->intSubset || doc->extSubset) fail("invalid or DTD-bearing XML: %s", filename);
    return doc;
}

static void ds_symbol_scan_file(const char *filename) {
    xmlDoc *doc = ds_xml_read(filename);
    ds_symbol_scan_nodes(xmlDocGetRootElement(doc));
    xmlFreeDoc(doc);
}

static void ds_symbol_scan_directory(const char *name) {
    DIR *dir = opendir(name);
    if (!dir) fail("open %s: %s", name, strerror(errno));
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char filename[PATH_MAX];
        path(filename, "%s/%s", name, entry->d_name);
        struct stat st;
        if (lstat(filename, &st) != 0) fail("stat %s failed", filename);
        if (S_ISLNK(st.st_mode)) fail("symbol scanner rejects symlinks: %s", filename);
        if (S_ISDIR(st.st_mode)) ds_symbol_scan_directory(filename);
        else if (S_ISREG(st.st_mode)) {
            const char *dot = strrchr(entry->d_name, '.');
            if (dot && !strcmp(dot, ".xml")) ds_symbol_scan_file(filename);
        }
    }
    closedir(dir);
}

static xmlNode *ds_symbol_definition(xmlNode *node, const char *type, const char *name) {
    for (; node; node = node->next) {
        if (node->type == XML_ELEMENT_NODE && !xmlStrcmp(node->name, BAD_CAST type)) {
            xmlChar *candidate = xmlGetProp(node, BAD_CAST "name");
            bool match = candidate && !strcmp((const char *)candidate, name);
            xmlFree(candidate);
            if (match) {
                if (strcmp(type, "attr") != 0 || xmlHasProp(node, BAD_CAST "format")) return node;
                for (xmlNode *child = node->children; child; child = child->next)
                    if (child->type == XML_ELEMENT_NODE &&
                        (!xmlStrcmp(child->name, BAD_CAST "enum") || !xmlStrcmp(child->name, BAD_CAST "flag"))) return node;
            }
        }
        xmlNode *found = ds_symbol_definition(node->children, type, name);
        if (found) return found;
    }
    return NULL;
}

static void ds_platform_symbols(const char *sdk) {
    ds_symbol_count = 0;
    ds_symbol_scan_file("app/src/main/AndroidManifest.xml");
    ds_symbol_scan_directory("app/src/main/res");
    ds_symbol_add("attr", "debuggable"); /* Native debug packaging adds this. */
    char filename[PATH_MAX];
    path(filename, "%s/platforms/%s/data/res/values/public-final.xml", sdk, DS_PLATFORM_VERSION);
    xmlDoc *ids = ds_xml_read(filename);
    const char *definition_files[] = {"attrs.xml", "attrs_manifest.xml", "colors.xml"};
    xmlDoc *definitions[3];
    for (size_t i = 0; i < 3; ++i) {
        path(filename, "%s/platforms/%s/data/res/values/%s", sdk, DS_PLATFORM_VERSION, definition_files[i]);
        definitions[i] = ds_xml_read(filename);
    }
    directory("build/native-platform/res");
    directory("build/native-platform/res/values");
    xmlDoc *output = xmlNewDoc(BAD_CAST "1.0");
    xmlNode *root = xmlNewNode(NULL, BAD_CAST "resources");
    if (!output || !root) fail("XML allocation failed");
    xmlDocSetRootElement(output, root);
    for (size_t i = 0; i < ds_symbol_count; ++i) {
        DSSymbol *symbol = &ds_symbols[i];
        for (xmlNode *node = xmlDocGetRootElement(ids)->children; node; node = node->next) {
            if (node->type != XML_ELEMENT_NODE || xmlStrcmp(node->name, BAD_CAST "public")) continue;
            xmlChar *name = xmlGetProp(node, BAD_CAST "name"), *type = xmlGetProp(node, BAD_CAST "type");
            if (name && type && !strcmp((const char *)name, symbol->name) && !strcmp((const char *)type, symbol->type)) {
                xmlChar *id = xmlGetProp(node, BAD_CAST "id");
                if (!id || strlen((const char *)id) >= sizeof(symbol->id)) fail("invalid public resource ID");
                strcpy(symbol->id, (const char *)id);
                xmlFree(id);
            }
            xmlFree(name);
            xmlFree(type);
        }
        if (!*symbol->id) fail("no published framework ID for %s/%s; review the SDK and resource usage", symbol->type, symbol->name);
        xmlNode *declaration = xmlNewChild(root, NULL, BAD_CAST "public", NULL);
        xmlNewProp(declaration, BAD_CAST "type", BAD_CAST symbol->type);
        xmlNewProp(declaration, BAD_CAST "name", BAD_CAST symbol->name);
        xmlNewProp(declaration, BAD_CAST "id", BAD_CAST symbol->id);
        xmlNode *definition = NULL;
        for (size_t d = 0; d < 3 && !definition; ++d)
            definition = ds_symbol_definition(xmlDocGetRootElement(definitions[d]), symbol->type, symbol->name);
        if (definition) {
            xmlNode *copy = xmlDocCopyNode(definition, output, 1);
            if (!copy) fail("XML allocation failed");
            /* Exclude source documentation, including embedded Java examples. */
            xmlNode *child = copy->children;
            while (child) {
                xmlNode *next = child->next;
                if (child->type == XML_COMMENT_NODE) { xmlUnlinkNode(child); xmlFreeNode(child); }
                child = next;
            }
            xmlAddChild(root, copy);
        } else if (!strcmp(symbol->type, "style")) {
            /* Style content is supplied by the OS. Only its ID is needed here. */
            xmlNode *style = xmlNewChild(root, NULL, BAD_CAST "style", NULL);
            xmlNewProp(style, BAD_CAST "name", BAD_CAST symbol->name);
            xmlNewProp(style, BAD_CAST "parent", BAD_CAST "");
        } else fail("no SDK definition for %s/%s", symbol->type, symbol->name);
    }
    if (xmlSaveFormatFileEnc("build/native-platform/res/values/symbols.xml", output, "UTF-8", 1) < 0)
        fail("saving framework resource IDs failed");
    printf("Generated %zu framework resource IDs from raw SDK XML; no Java archives.\n", ds_symbol_count);
    xmlFreeDoc(output);
    xmlFreeDoc(ids);
    for (size_t i = 0; i < 3; ++i) xmlFreeDoc(definitions[i]);
}
#endif
