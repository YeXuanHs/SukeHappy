#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "SAPI.h"
#include "Zend/zend_exceptions.h"
#include "zend_compile.h"
#include "ext/standard/info.h"
#include "php_sukehappy.h"

ZEND_DECLARE_MODULE_GLOBALS(sukehappy)

static void (*suke_prev_execute_ex)(zend_execute_data *execute_data);
static void (*suke_prev_execute_internal)(zend_execute_data *execute_data, zval *return_value);
static zend_op_array *(*suke_orig_compile_file)(zend_file_handle *fh, int type);

static void suke_log(const char *fmt, ...);
static void suke_deny(const char *why, const char *detail);

/* ========================================================= */
/* 1. 运行时拦截逻辑（防后门、防毁库） */
/* ========================================================= */

static const char *suke_sql_bad[] = {
    "drop table", "drop database", "truncate ", "alter table",
    "into outfile", "into dumpfile", "load_file(", "load data",
    "create user", "grant ", "into dumpfile", NULL
};

static int suke_sql_dangerous(const char *sql) {
    char buf[4096];
    size_t i, n;
    const char **p;
    if (!sql) return 0;
    n = strlen(sql);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    for (i = 0; i < n; i++) {
        char c = sql[i];
        buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    buf[n] = '\0';
    for (p = suke_sql_bad; *p; p++) {
        if (strstr(buf, *p)) return 1;
    }
    return 0;
}

static int suke_in_list(const char *word, const char *list) {
    const char *p;
    size_t n;
    if (!word || !list || !*list) return 0;
    n = strlen(word);
    p = list;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        while (len && (*p == ' ' || *p == '\t')) { p++; len--; }
        while (len && (p[len - 1] == ' ' || p[len - 1] == '\t')) { len--; }
        if (len == n && strncasecmp(p, word, n) == 0) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

static int suke_uri_hit(const char *uri) {
    const char *p, *list;
    if (!uri || !*uri) return 0;
    list = SUKEHAPPY_G(block_uris);
    if (!list || !*list) return 0;
    p = list;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char tok[256];
        if (len > 0 && len < sizeof(tok)) {
            memcpy(tok, p, len);
            tok[len] = '\0';
            if (*tok && strstr(uri, tok)) return 1;
        }
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

static void suke_deny(const char *why, const char *detail) {
    if (strcmp(sapi_module.name, "cli") != 0) {
        sapi_header_op(SAPI_HEADER_SET_STATUS, (void *)(zend_long)404);
    }
    suke_log("[sukehappy] deny %s: %s uri=%s\n", why,
            detail ? detail : "-",
            SG(request_info).request_uri ? SG(request_info).request_uri : "-");
    zend_bailout();
}

static void suke_execute_internal(zend_execute_data *execute_data, zval *return_value) {
    if (execute_data && execute_data->func
        && execute_data->func->type == ZEND_INTERNAL_FUNCTION
        && SUKEHAPPY_G(block_sql)) {
        const char *fn = execute_data->func->common.function_name
                       ? ZSTR_VAL(execute_data->func->common.function_name) : NULL;
        if (fn && (strcasecmp(fn, "exec") == 0 || strcasecmp(fn, "query") == 0
                || strcasecmp(fn, "prepare") == 0 || strcasecmp(fn, "real_query") == 0
                || strcasecmp(fn, "multi_query") == 0)) {
            if (ZEND_CALL_NUM_ARGS(execute_data) >= 1) {
                zval *a = ZEND_CALL_ARG(execute_data, 1);
                if (a && Z_TYPE_P(a) == IS_STRING && suke_sql_dangerous(Z_STRVAL_P(a))) {
                    suke_log("[sukehappy] deny sql: %.200s\n", Z_STRVAL_P(a));
                    suke_deny("sql", fn);
                }
            }
        }
    }
    if (suke_prev_execute_internal) {
        suke_prev_execute_internal(execute_data, return_value);
    } else {
        execute_internal(execute_data, return_value);
    }
}

static void suke_execute_ex(zend_execute_data *execute_data) {
    if (execute_data && execute_data->func && execute_data->func->type == ZEND_USER_FUNCTION) {
        zend_function *f = execute_data->func;
        if (f->common.function_name) {
            const char *fn = ZSTR_VAL(f->common.function_name);
            if (suke_in_list(fn, SUKEHAPPY_G(block_functions))) {
                suke_deny("function", fn);
            }
        }
    }
    suke_prev_execute_ex(execute_data);
}

/* ========================================================= */
/* 2. 编译期重写授权逻辑 */
/* ========================================================= */

typedef struct {
    const char *anchor;
    const char *insert;
} suke_patch_t;

static const suke_patch_t suke_patches_common[] = {
    // 完美伪造授权载荷 ki：aes-256-cbc 加密 + 16字节前缀 + base64
    {
        "function getAuthorizationCurl(",
        "\n"
        "    $suke_key = '50ftv31walzbaga5';\n"
        "    $suke_iv = '0995964364864570';\n"
        "    $suke_data = json_encode(['domain' => $_SERVER['HTTP_HOST'] ?? 'localhost', 'expire' => '2099-12-31 23:59:59']);\n"
        "    $suke_cipher = openssl_encrypt($suke_data, 'aes-256-cbc', $suke_key, 0, $suke_iv);\n"
        "    $suke_ki = base64_encode(str_repeat('X', 16) . $suke_cipher);\n"
        "    \\think\\facade\\Cache::set('hzdtqmwbodere7d9', $suke_ki, 86400);\n"
        "    \\think\\facade\\Cache::set('8ue6hpsuffpheaaj', 'web', 86400);\n"
        "    return ['code' => 200, 'ki' => $suke_ki];\n"
    },
    // 直接短路模板授权校验，防止原代码抛出 U-1/U-2/U-3
    {
        "function templateAuthorization(",
        "\n    return true;\n"
    },
    { NULL, NULL }
};

/* 安全定位函数体的 '{' */
static const char *suke_find_body_brace(const char *pos, const char *end) {
    const char *p = pos;
    int depth = 0;
    char quote = 0;
    while (p < end) {
        char c = *p;
        if (quote) {
            if (c == '\\' && (p + 1) < end) { p += 2; continue; }
            if (c == quote) { quote = 0; }
            p++; continue;
        }
        if (c == '\'' || c == '"') { quote = c; p++; continue; }
        if (c == '/' && (p + 1) < end && p[1] == '/') {
            while (p < end && *p != '\n') p++;
            continue;
        }
        if (c == '#' && !((p + 1) < end && p[1] == '[')) {
            while (p < end && *p != '\n') p++;
            continue;
        }
        if (c == '/' && (p + 1) < end && p[1] == '*') {
            p += 2;
            while ((p + 1) < end && !(p[0] == '*' && p[1] == '/')) p++;
            if ((p + 1) < end) p += 2;
            continue;
        }
        if (c == '(' || c == '[') { depth++; p++; continue; }
        if (c == ')' || c == ']') { if (depth > 0) depth--; p++; continue; }
        if (c == '{') {
            if (depth == 0) return p;
            depth++; p++; continue;
        }
        if (c == '}') { if (depth > 0) depth--; p++; continue; }
        if (c == ';' && depth == 0) return NULL;
        p++;
    }
    return NULL;
}

/* 源码注入 */
static zend_string *suke_patch_source(zend_string *src, const suke_patch_t *patches, int *hits_out) {
    zend_string *cur = src;
    int i;
    int hits = 0;
    for (i = 0; patches[i].anchor != NULL; i++) {
        const char *a = patches[i].anchor;
        size_t alen = strlen(a);
        const char *hay, *pos, *brace;
        size_t hlen, off, ilen;
        zend_string *ns;

        hay = ZSTR_VAL(cur);
        hlen = ZSTR_LEN(cur);
        pos = zend_memnstr(hay, a, alen, hay + hlen);
        if (!pos) continue;

        brace = suke_find_body_brace(pos + alen, hay + hlen);
        if (!brace) continue;

        off = (size_t)(brace - hay) + 1;
        ilen = strlen(patches[i].insert);

        ns = zend_string_alloc(hlen + ilen, 0);
        memcpy(ZSTR_VAL(ns), hay, off);
        memcpy(ZSTR_VAL(ns) + off, patches[i].insert, ilen);
        memcpy(ZSTR_VAL(ns) + off + ilen, hay + off, hlen - off);
        ZSTR_VAL(ns)[ZSTR_LEN(ns)] = '\0';

        if (cur != src) zend_string_release(cur);
        cur = ns;
        hits++;
    }
    if (hits_out) *hits_out = hits;
    return cur;
}

static zend_string *suke_read_file(const char *path) {
    FILE *fp; long len; char *buf; zend_string *s;
    fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    len = ftell(fp);
    if (len <= 0) { fclose(fp); return NULL; }
    rewind(fp);
    buf = emalloc((size_t)len + 1);
    if (fread(buf, 1, (size_t)len, fp) != (size_t)len) { efree(buf); fclose(fp); return NULL; }
    fclose(fp);
    s = zend_string_init(buf, (size_t)len, 0);
    efree(buf);
    return s;
}

static zend_string *suke_strip_open_tag(zend_string *src) {
    const char *p = ZSTR_VAL(src);
    const char *end = p + ZSTR_LEN(src);
    if ((size_t)(end - p) >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    if ((size_t)(end - p) < 5 || strncmp(p, "<?php", 5) != 0) return NULL;
    p += 5;
    if (p < end && *p == '\r') { p++; if (p < end && *p == '\n') p++; }
    else if (p < end && *p == '\n') p++;
    return zend_string_init(p, (size_t)(end - p), 0);
}

static zend_op_array *suke_compile_file(zend_file_handle *fh, int type) {
    const char *fn = NULL;
    if (fh->opened_path) fn = ZSTR_VAL(fh->opened_path);
    if (!fn || !*fn) {
#if PHP_VERSION_ID >= 80100
        fn = fh->filename ? ZSTR_VAL(fh->filename) : NULL;
#else
        fn = fh->filename;
#endif
    }

    if (fn && strstr(fn, "app/template/common.php")) {
        zend_string *src = suke_read_file(fn);
        if (src) {
            int hits = 0;
            zend_string *patched = suke_patch_source(src, suke_patches_common, &hits);
            if (hits > 0) {
                zend_string *code = suke_strip_open_tag(patched);
                if (code) {
                    zval zv;
                    zend_op_array *op_array;
#if PHP_VERSION_ID >= 80200
                    op_array = zend_compile_string(code, fn, ZEND_COMPILE_POSITION_AFTER_OPEN_TAG);
#elif PHP_VERSION_ID >= 80000
                    op_array = zend_compile_string(code, fn);
#else
                    ZVAL_STR(&zv, code);
                    op_array = zend_compile_string(&zv, (char *)fn);
#endif
                    zend_string_release(code);
                    if (patched != src) zend_string_release(patched);
                    zend_string_release(src);

                    if (op_array) {
                        if (fh->opened_path) {
                            zend_hash_add_empty_element(&EG(included_files), fh->opened_path);
                        }
                        return op_array;
                    }
                    return suke_orig_compile_file(fh, type);
                }
                if (patched != src) zend_string_release(patched);
            }
            zend_string_release(src);
        }
    }
    return suke_orig_compile_file(fh, type);
}

/* ========================================================= */
/* 3. 模块生命周期与 INI 配置 */
/* ========================================================= */

static void suke_log(const char *fmt, ...) {
    char path[1024];
    FILE *fp;
    va_list ap;
    if (!SUKEHAPPY_G(logfile) || !*SUKEHAPPY_G(logfile)) return;
    snprintf(path, sizeof(path), "%s", SUKEHAPPY_G(logfile));
    fp = fopen(path, "a");
    if (!fp) return;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fclose(fp);
}

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("sukehappy.block_functions", "greatMove,e2iz72t17dzwmbnv,admin_decrypt", PHP_INI_SYSTEM, OnUpdateString, block_functions, zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.block_uris", "i5nyyoblvmcpuj71,o5tlkifzcrrgcvhx", PHP_INI_SYSTEM, OnUpdateString, block_uris, zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.block_sql", "1", PHP_INI_SYSTEM, OnUpdateBool, block_sql, zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.logfile", "/tmp/sukehappy.log", PHP_INI_SYSTEM, OnUpdateString, logfile, zend_sukehappy_globals, sukehappy_globals)
PHP_INI_END()

static PHP_GINIT_FUNCTION(sukehappy) {
#if defined(COMPILE_DL_SUKEHAPPY) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    sukehappy_globals->block_functions = NULL;
    sukehappy_globals->block_uris = NULL;
    sukehappy_globals->block_sql = 0;
    sukehappy_globals->logfile = NULL;
}

static PHP_MINIT_FUNCTION(sukehappy) {
    REGISTER_INI_ENTRIES();
    suke_orig_compile_file = zend_compile_file;
    zend_compile_file = suke_compile_file;
    return SUCCESS;
}

static PHP_MSHUTDOWN_FUNCTION(sukehappy) {
    UNREGISTER_INI_ENTRIES();
    if (suke_orig_compile_file) {
        zend_compile_file = suke_orig_compile_file;
        suke_orig_compile_file = NULL;
    }
    return SUCCESS;
}

static PHP_RINIT_FUNCTION(sukehappy) {
#if defined(COMPILE_DL_SUKEHAPPY) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    if (suke_uri_hit(SG(request_info).request_uri)) {
        suke_deny("uri", SG(request_info).request_uri);
    }
    suke_prev_execute_ex = zend_execute_ex;
    zend_execute_ex = suke_execute_ex;
    suke_prev_execute_internal = zend_execute_internal;
    zend_execute_internal = suke_execute_internal;
    return SUCCESS;
}

static PHP_RSHUTDOWN_FUNCTION(sukehappy) {
    if (suke_prev_execute_ex) {
        zend_execute_ex = suke_prev_execute_ex;
        suke_prev_execute_ex = NULL;
    }
    if (suke_prev_execute_internal || zend_execute_internal == suke_execute_internal) {
        zend_execute_internal = suke_prev_execute_internal;
        suke_prev_execute_internal = NULL;
    }
    return SUCCESS;
}

static PHP_MINFO_FUNCTION(sukehappy) {
    php_info_print_table_start();
    php_info_print_table_row(2, "sukehappy", "enabled");
    php_info_print_table_row(2, "blocked functions", SUKEHAPPY_G(block_functions) ? SUKEHAPPY_G(block_functions) : "");
    php_info_print_table_row(2, "block sql (ddl/outfile)", SUKEHAPPY_G(block_sql) ? "on" : "off");
    php_info_print_table_row(2, "blocked uris", SUKEHAPPY_G(block_uris) ? SUKEHAPPY_G(block_uris) : "");
    php_info_print_table_end();
    DISPLAY_INI_ENTRIES();
}

zend_module_entry sukehappy_module_entry = {
    STANDARD_MODULE_HEADER,
    "sukehappy",
    NULL,
    PHP_MINIT(sukehappy),
    PHP_MSHUTDOWN(sukehappy),
    PHP_RINIT(sukehappy),
    PHP_RSHUTDOWN(sukehappy),
    PHP_MINFO(sukehappy),
    PHP_SUKEHAPPY_VERSION,
    PHP_MODULE_GLOBALS(sukehappy),
    PHP_GINIT(sukehappy),
    NULL,
    NULL,
    STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_SUKEHAPPY
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(sukehappy)
#endif