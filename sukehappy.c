/*
 * sukehappy — 速科模板综合扩展
 *
 * 能力：
 *   1) 拦截后门入口（URI + 函数名），零功能影响
 *   2) 可选：拦截 DDL/OUTFILE 等危险 SQL
 *   3) 可选（mock_auth=1）：在 curl 层伪造 apiv1.sukeyun.com 的响应，
 *      使模板的授权校验与在线更新完全本地化，不再依赖厂商服务器。
 *
 * 为什么在 curl 层做：
 *   模板源码被 Swoole Loader 加密，编译期改不了（loader 绕开了全局
 *   zend_compile_file / zend_compile_string，自带编译流程）。
 *   但授权判定的咽喉是 PHP curl 扩展的 curl_exec —— 它是内部函数，
 *   可以在 zend_execute_internal 上精确劫持。
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "SAPI.h"
#include "Zend/zend_exceptions.h"
#include "ext/standard/info.h"
#include "php_sukehappy.h"

#include <string.h>
#include <stdlib.h>

ZEND_DECLARE_MODULE_GLOBALS(sukehappy)

static void (*suke_prev_execute_ex)(zend_execute_data *execute_data);
static void (*suke_prev_execute_internal)(zend_execute_data *execute_data, zval *return_value);

/* curl 句柄 → URL 映射（每请求） */
static HashTable suke_curl_urls;
/* 已伪造过响应的句柄 */
static HashTable suke_mocked;

static void suke_log(const char *fmt, ...);
static void suke_deny(const char *why, const char *detail);

/* ========================================================= */
/* 1. 通用工具 */
/* ========================================================= */

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

static int suke_sql_dangerous(const char *sql) {
    static const char *bad[] = {
        "drop table", "drop database", "truncate ", "alter table",
        "into outfile", "into dumpfile", "load_file(", "load data",
        "create user", "grant ", NULL
    };
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
    for (p = bad; *p; p++) {
        if (strstr(buf, *p)) return 1;
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

/* ========================================================= */
/* 2. 授权响应伪造 */
/* ========================================================= */

/* 取 URL query 里某参数的值（不解码 %xx） */
static int suke_url_param(const char *url, const char *key, char *out, size_t outlen) {
    const char *p = strstr(url, "?");
    size_t klen = strlen(key);
    if (!p) return 0;
    p++;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t i = 0;
            while (*p && *p != '&' && i + 1 < outlen) { out[i++] = *p++; }
            out[i] = '\0';
            return 1;
        }
        p = strchr(p, '&');
        if (!p) break;
        p++;
    }
    return 0;
}

/* 生成授权中心的伪造响应；返回 0 表示不伪造 */
static int suke_mock_response(const char *url, char *buf, size_t buflen) {
    const char *ki;
    char dom[256];

    if (!url || !strstr(url, "apiv1.sukeyun.com")) return 0;

    /* ① /ky/admin/template?url=<域名>&v=cx  （templateAuthorization）
     *    要求 code==200 且 msg 存在；msg 必须等于请求里的域名，否则 exit U-1 */
    if (strstr(url, "/ky/admin/template")) {
        if (!suke_url_param(url, "url", dom, sizeof(dom))) dom[0] = '\0';
        snprintf(buf, buflen,
                 "{\"code\":200,\"msg\":\"%s\",\"data\":{\"query\":true,\"version\":\"1.0.7.6\","
                 "\"status\":200,\"activation\":false,\"msg\":\"ok\"}}", dom);
        return 1;
    }

    /* ② /ky/admin  （getAuthorizationCurl）
     *    要求 code==200 且 ki 非空；ki 会写进 cache hzdtqmwbodere7d9 当 JWT 密钥 */
    if (strstr(url, "/ky/admin") && !strstr(url, "/ky/admin/")) {
        ki = SUKEHAPPY_G(auth_ki);
        snprintf(buf, buflen, "{\"code\":200,\"ki\":\"%s\",\"msg\":\"ok\"}",
                 (ki && *ki) ? ki : "sukehappy-local-ki-000000000000000");
        return 1;
    }

    /* ③ /ky/admin/verification?...  （verifySecurityCode）
     *    要求 code==200 且 msg=="ok" */
    if (strstr(url, "/ky/admin/verification")) {
        snprintf(buf, buflen, "{\"code\":200,\"msg\":\"ok\",\"data\":{\"query\":true}}");
        return 1;
    }

    /* ④ /ky/admin/version?upid=..&phpversion=..  （getNewVersion）
     *    要求 code==200、data.version、msg、status */
    if (strstr(url, "/ky/admin/version")) {
        snprintf(buf, buflen,
                 "{\"code\":200,\"msg\":\"ok\",\"status\":200,"
                 "\"data\":{\"version\":\"1.0.7.6\",\"status\":200,\"phpversion\":\"7.4.19\"}}");
        return 1;
    }

    /* ⑤ /ky/admin/callback?version=..  （updateSuccess） */
    if (strstr(url, "/ky/admin/callback")) {
        snprintf(buf, buflen, "{\"code\":200,\"msg\":\"ok\",\"data\":{\"query\":true}}");
        return 1;
    }

    return 0;
}

/* ========================================================= */
/* 3. curl 句柄跟踪与劫持 */
/* ========================================================= */

static zend_ulong suke_handle_id(zval *hv) {
    if (!hv) return 0;
#if PHP_VERSION_ID >= 80000
    if (Z_TYPE_P(hv) == IS_OBJECT) {
        return (zend_ulong)Z_OBJ_HANDLE_P(hv);
    }
#else
    if (Z_TYPE_P(hv) == IS_RESOURCE) {
        return (zend_ulong)Z_RES_HANDLE_P(hv);
    }
#endif
    return 0;
}

static const char *suke_handle_url(zend_ulong id) {
    zval *z;
    if (!id) return NULL;
    z = zend_hash_index_find(&suke_curl_urls, (zend_ulong)id);
    if (!z || Z_TYPE_P(z) != IS_STRING) return NULL;
    return Z_STRVAL_P(z);
}

static int suke_handle_mocked(zend_ulong id) {
    return id && zend_hash_index_exists(&suke_mocked, (zend_ulong)id);
}

static void suke_mark_mocked(zend_ulong id) {
    zval z;
    ZVAL_TRUE(&z);
    zend_hash_index_update(&suke_mocked, (zend_ulong)id, &z);
}

/* ========================================================= */
/* 4. 内部函数钩子：SQL 拦截 + curl 劫持 */
/* ========================================================= */

static void suke_execute_internal(zend_execute_data *execute_data, zval *return_value) {
    if (execute_data && execute_data->func
        && execute_data->func->type == ZEND_INTERNAL_FUNCTION) {
        const char *fn = execute_data->func->common.function_name
                       ? ZSTR_VAL(execute_data->func->common.function_name) : NULL;

        if (fn) {
            /* curl_setopt：记录 句柄 → URL （CURLOPT_URL = 10002） */
            if (strcmp(fn, "curl_setopt") == 0 && ZEND_CALL_NUM_ARGS(execute_data) >= 3) {
                zval *hv  = ZEND_CALL_ARG(execute_data, 1);
                zval *opt = ZEND_CALL_ARG(execute_data, 2);
                zval *val = ZEND_CALL_ARG(execute_data, 3);
                if (Z_TYPE_P(opt) == IS_LONG && Z_LVAL_P(opt) == 10002
                    && Z_TYPE_P(val) == IS_STRING) {
                    zend_ulong id = suke_handle_id(hv);
                    if (id) {
                        zval z;
                        ZVAL_STR_COPY(&z, Z_STR_P(val));
                        zend_hash_index_update(&suke_curl_urls, (zend_ulong)id, &z);
                    }
                }
            }
            /* curl_exec：命中授权域名则伪造响应，不真正发请求 */
            else if (strcmp(fn, "curl_exec") == 0 && ZEND_CALL_NUM_ARGS(execute_data) >= 1) {
                zval *hv = ZEND_CALL_ARG(execute_data, 1);
                zend_ulong id = suke_handle_id(hv);
                const char *url = suke_handle_url(id);
                if (url && SUKEHAPPY_G(mock_auth)) {
                    char body[4096];
                    if (suke_mock_response(url, body, sizeof(body))) {
                        suke_mark_mocked(id);
                        suke_log("[sukehappy] mock auth: %.120s\n", url);
                        if (return_value) {
                            ZVAL_STRING(return_value, body);
                        }
                        return;
                    }
                }
            }
            /* curl_getinfo：伪造的句柄要报 HTTP 200（CURLINFO_HTTP_CODE = 2097154） */
            else if (strcmp(fn, "curl_getinfo") == 0 && ZEND_CALL_NUM_ARGS(execute_data) >= 2) {
                zval *hv  = ZEND_CALL_ARG(execute_data, 1);
                zval *opt = ZEND_CALL_ARG(execute_data, 2);
                zend_ulong id = suke_handle_id(hv);
                if (id && suke_handle_mocked(id)
                    && Z_TYPE_P(opt) == IS_LONG && Z_LVAL_P(opt) == 2097154) {
                    if (return_value) ZVAL_LONG(return_value, 200);
                    return;
                }
            }
            /* SQL 拦截 */
            else if (SUKEHAPPY_G(block_sql)
                     && (strcmp(fn, "exec") == 0 || strcmp(fn, "query") == 0
                         || strcmp(fn, "prepare") == 0 || strcmp(fn, "real_query") == 0
                         || strcmp(fn, "multi_query") == 0)) {
                if (ZEND_CALL_NUM_ARGS(execute_data) >= 1) {
                    zval *a = ZEND_CALL_ARG(execute_data, 1);
                    if (a && Z_TYPE_P(a) == IS_STRING && suke_sql_dangerous(Z_STRVAL_P(a))) {
                        suke_log("[sukehappy] deny sql: %.200s\n", Z_STRVAL_P(a));
                        suke_deny("sql", fn);
                    }
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

/* ========================================================= */
/* 5. 用户函数拦截（后门函数） */
/* ========================================================= */

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
/* 6. 生命周期 */
/* ========================================================= */

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("sukehappy.block_functions", "greatMove,e2iz72t17dzwmbnv,admin_decrypt",
        PHP_INI_SYSTEM, OnUpdateString, block_functions, zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.block_uris", "i5nyyoblvmcpuj71,o5tlkifzcrrgcvhx",
        PHP_INI_SYSTEM, OnUpdateString, block_uris, zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.block_sql", "0",
        PHP_INI_SYSTEM, OnUpdateBool, block_sql, zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.logfile", "/tmp/sukehappy.log",
        PHP_INI_SYSTEM, OnUpdateString, logfile, zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.mock_auth", "0",
        PHP_INI_SYSTEM, OnUpdateBool, mock_auth, zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.auth_ki", "",
        PHP_INI_SYSTEM, OnUpdateString, auth_ki, zend_sukehappy_globals, sukehappy_globals)
PHP_INI_END()

static void suke_log(const char *fmt, ...) {
    va_list args;
    FILE *fp;
    const char *path = SUKEHAPPY_G(logfile);
    if (!path || !*path) return;
    fp = fopen(path, "a");
    if (!fp) return;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);
    fclose(fp);
}

PHP_GINIT_FUNCTION(sukehappy)
{
#if defined(COMPILE_DL_SUKEHAPPY) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    sukehappy_globals->block_functions = NULL;
    sukehappy_globals->block_uris = NULL;
    sukehappy_globals->block_sql = 0;
    sukehappy_globals->logfile = NULL;
    sukehappy_globals->auth_ki = NULL;
    sukehappy_globals->mock_auth = 0;
}

PHP_MINIT_FUNCTION(sukehappy)
{
    REGISTER_INI_ENTRIES();

    zend_hash_init(&suke_curl_urls, 8, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&suke_mocked, 8, NULL, ZVAL_PTR_DTOR, 0);

    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(sukehappy)
{
    zend_hash_destroy(&suke_curl_urls);
    zend_hash_destroy(&suke_mocked);
    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}

PHP_RINIT_FUNCTION(sukehappy)
{
#if defined(COMPILE_DL_SUKEHAPPY) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    zend_hash_clean(&suke_curl_urls);
    zend_hash_clean(&suke_mocked);

    if (suke_uri_hit(SG(request_info).request_uri)) {
        suke_deny("uri", SG(request_info).request_uri);
    }

    suke_prev_execute_ex = zend_execute_ex;
    zend_execute_ex = suke_execute_ex;

    suke_prev_execute_internal = zend_execute_internal;
    zend_execute_internal = suke_execute_internal;

    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(sukehappy)
{
    if (suke_prev_execute_ex) {
        zend_execute_ex = suke_prev_execute_ex;
        suke_prev_execute_ex = NULL;
    }
    if (suke_prev_execute_internal) {
        zend_execute_internal = suke_prev_execute_internal;
        suke_prev_execute_internal = NULL;
    }
    return SUCCESS;
}

PHP_MINFO_FUNCTION(sukehappy)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "sukehappy", "速科模板综合扩展");
    php_info_print_table_row(2, "block sql (ddl/outfile)", SUKEHAPPY_G(block_sql) ? "on" : "off");
    php_info_print_table_row(2, "mock auth (免远程授权)", SUKEHAPPY_G(mock_auth) ? "on" : "off");
    php_info_print_table_row(2, "auth ki", SUKEHAPPY_G(auth_ki) ? SUKEHAPPY_G(auth_ki) : "(默认)");
    php_info_print_table_row(2, "blocked functions", SUKEHAPPY_G(block_functions) ? SUKEHAPPY_G(block_functions) : "");
    php_info_print_table_row(2, "blocked uris", SUKEHAPPY_G(block_uris) ? SUKEHAPPY_G(block_uris) : "");
    php_info_print_table_row(2, "logfile", SUKEHAPPY_G(logfile) ? SUKEHAPPY_G(logfile) : "");
    php_info_print_table_end();
    display_ini_entries(NULL);
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
    STANDARD_MODULE_PROPERTIES
};

ZEND_GET_MODULE(sukehappy)
