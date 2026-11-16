/*
 * sukehappy 授权本地化（最小实现）
 *
 * 目标：让模板对 apiv1.sukeyun.com 的授权校验请求被本地接管，
 *       模板拿到「授权通过」的响应，不再 exit「授权错误，错误码U-1」。
 *
 * 实现要点（都是实测踩出来的，务必保留）：
 *
 * 1) PHP 7 的 ZEND_DO_ICALL(fastcall) 路径绕过 zend_execute_internal 与
 *    zend_execute_ex，curl_setopt / curl_exec / curl_getinfo 只有直接替换
 *    zend_internal_function.handler 指针才拦得到。
 *
 * 2) 必须改 **CG(function_table)**（编译期共享表）——ICALL 用的 fbc
 *    来自 opcode cache，指向 CG 里的函数；改 EG 的请求级副本不生效。
 *
 * 3) 原 handler **只在第一次捕获**。CG 里存的就是我们的 wrapper，
 *    如果每次 RINIT 都重新捕获，就会把 wrapper 自己当成「原 handler」，
 *    wrapper 调 wrapper → 无限递归 → worker 反复 exit code 1，
 *    nginx 侧表现为随机 502。
 *
 * 4) 替换参数槽里的字符串必须先 ZVAL_STR_COPY（引用计数 +1）再
 *    zval_ptr_dtor（-1），最后让本 zval 接管新串。直接 zval_ptr_dtor 会把
 *    **可能被其它 zval 共享**的原字符串释放掉 → use-after-free。
 *
 * 5) 模板把响应的 msg 当「错误信息」看：msg 非空 = 授权失败（exit U-1），
 *    授权通过时 msg 必须是空串。伪造响应的字段结构也要与真实响应一致
 *    （真实响应只有 code 与 msg 两个字段），多给字段会让模板走进别的分支。
 *
 * 6) ki 必须留空：填假 ki 会让 v33 中间件拿它去验一个非法 token，
 *    走进 firebase/php-jwt 的异常路径 → zend_mm_heap corrupted → worker SIGSEGV。
 *
 * 7) curl_exec 不能跳过原 handler：跳过会让 curl 句柄处于半初始化状态，
 *    析构时 libcurl 崩溃。正确做法是让它正常跑完（改写到本地 mock），
 *    之后只把返回值换成伪造的响应。
 */

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_exceptions.h"

ZEND_BEGIN_MODULE_GLOBALS(sukehappy)
    char *logfile;
    zend_bool mock_auth;
    zend_long mock_port;
    zend_bool block_sql;
    char *block_functions;
    char *block_uris;
    HashTable curl_urls;      /* 句柄 → CURLOPT_URL */
    HashTable curl_mocked;    /* 命中授权域名、URL 已被改写的句柄 */
ZEND_END_MODULE_GLOBALS(sukehappy)

ZEND_EXTERN_MODULE_GLOBALS(sukehappy)

#define SUKE_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(sukehappy, v)

typedef void (*suke_handler)(zend_execute_data *, zval *);

static suke_handler suke_orig_setopt = NULL;
static suke_handler suke_orig_exec = NULL;
static suke_handler suke_orig_getinfo = NULL;

/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */

static void suke_log(const char *fmt, ...) {
    va_list args;
    FILE *fp;
    const char *path = SUKE_G(logfile);

    if (!path || !*path) path = "/tmp/sukehappy.log";
    fp = fopen(path, "a");
    if (!fp) return;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);
    fclose(fp);
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
    const char *p, *list = SUKE_G(block_uris);
    if (!uri || !*uri || !list || !*list) return 0;
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

/* curl 句柄 → 资源 id */
static zend_ulong suke_handle_id(zval *handle) {
    if (!handle || Z_TYPE_P(handle) != IS_RESOURCE) return 0;
    return (zend_ulong)Z_RES_HANDLE_P(handle);
}

/* 取句柄对应的原始 URL */
static const char *suke_handle_url(zend_ulong id) {
    zval *z;
    if (!id) return NULL;
    z = zend_hash_index_find(&SUKE_G(curl_urls), (zend_ulong)id);
    if (!z || Z_TYPE_P(z) != IS_STRING) return NULL;
    return Z_STRVAL_P(z);
}

/* ------------------------------------------------------------------ */
/* 授权中心的伪造响应                                                  */
/* ------------------------------------------------------------------ */

/* 结构必须与授权中心真实响应一致：只有 code 与 msg 两个字段 */
static int suke_fake_body(const char *url, char *buf, size_t buflen) {
    if (!url || !*url) return 0;

    if (strstr(url, "/ky/admin/template")) {
        snprintf(buf, buflen, "{\"code\":200,\"msg\":\"\"}");
        return 1;
    }
    if (strstr(url, "/ky/admin/verification")) {
        snprintf(buf, buflen, "{\"code\":200,\"msg\":\"ok\"}");
        return 1;
    }
    if (strstr(url, "/ky/admin/version")) {
        snprintf(buf, buflen, "{\"code\":200,\"msg\":\"ok\",\"status\":200,\"data\":{\"version\":\"1.0.7.6\",\"status\":200}}");
        return 1;
    }
    if (strstr(url, "/ky/admin/callback")) {
        snprintf(buf, buflen, "{\"code\":200,\"msg\":\"ok\"}");
        return 1;
    }
    if (strstr(url, "/ky/admin")) {
        /* ki 留空，见文件头说明 6) */
        snprintf(buf, buflen, "{\"code\":200,\"ki\":\"\",\"msg\":\"ok\"}");
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* curl handler 包装                                                   */
/* ------------------------------------------------------------------ */

static void suke_curl_setopt_handler(zend_execute_data *execute_data, zval *return_value) {
    /* handler 上下文里 ZEND_CALL_ARG(execute_data, 0) 是空槽，
     * 真正的参数从 index 1 开始（与 execute_internal 的 0-based 不同）。 */
    suke_log("[sukehappy] setopt entered mock_auth=%d\n", (int)SUKE_G(mock_auth));
    if (ZEND_CALL_NUM_ARGS(execute_data) >= 3) {
        zval *hv  = ZEND_CALL_ARG(execute_data, 1);
        zval *opt = ZEND_CALL_ARG(execute_data, 2);
        zval *val = ZEND_CALL_ARG(execute_data, 3);

        if (Z_TYPE_P(opt) == IS_LONG && Z_TYPE_P(val) == IS_STRING) {
            zend_ulong id = suke_handle_id(hv);
            if (id) {
                if (Z_LVAL_P(opt) == 10002 /*CURLOPT_URL*/) {
                    zval z;
                    ZVAL_STR_COPY(&z, Z_STR_P(val));
                    zend_hash_index_update(&SUKE_G(curl_urls), (zend_ulong)id, &z);

                    /* 命中授权域名 → 把请求改写到本地 mock 服务。
                     * 自我递归保护只看 host 前缀：模板拼的 URL 里 query 含
                     * url=127.0.0.1，整串 strstr 会误伤。 */
                    if (SUKE_G(mock_auth)
                        && strstr(Z_STRVAL_P(val), "apiv1.sukeyun.com")
                        && strncmp(Z_STRVAL_P(val), "http://127.0.0.1", 16) != 0) {
                        const char *sl = strstr(Z_STRVAL_P(val), "://");
                        if (sl) sl = strchr(sl + 3, '/');
                        if (sl) {
                            char newurl[1024];
                            snprintf(newurl, sizeof(newurl), "http://127.0.0.1:%d%s",
                                     (int)SUKE_G(mock_port), sl);
                            zend_hash_index_add_empty_element(&SUKE_G(curl_mocked), (zend_ulong)id);
                            {
                                /* 见文件头说明 4)：先 +1 再 -1，最后接管新串 */
                                zval tmp;
                                ZVAL_STR_COPY(&tmp, Z_STR_P(val));
                                zval_ptr_dtor(val);
                                ZVAL_STR_COPY(val, Z_STR_P(tmp));
                                zval_ptr_dtor(&tmp);
                            }
                            {
                                zval z2;
                                ZVAL_STR_COPY(&z2, Z_STR_P(val));
                                zend_hash_index_update(&SUKE_G(curl_urls), (zend_ulong)id, &z2);
                            }
                        }
                    }
                }
            }
        }
    }
    if (suke_orig_setopt) {
        suke_orig_setopt(execute_data, return_value);
    }
}

static void suke_curl_exec_handler(zend_execute_data *execute_data, zval *return_value) {
    const char *url = NULL;
    int mocked = 0;

    if (ZEND_CALL_NUM_ARGS(execute_data) >= 1) {
        zval *hv = ZEND_CALL_ARG(execute_data, 1);
        zend_ulong id = suke_handle_id(hv);
        url = suke_handle_url(id);
        if (url && SUKE_G(mock_auth)
            && zend_hash_index_exists(&SUKE_G(curl_mocked), (zend_ulong)id)) {
            mocked = 1;
        }
    }

    /* 必须让原 handler 正常跑完（curl 句柄状态完整，否则析构时 libcurl 崩） */
    if (suke_orig_exec) {
        suke_orig_exec(execute_data, return_value);
    }

    /* 把返回值换成伪造的「授权通过」响应 */
    if (mocked && return_value) {
        char body[1024];
        if (suke_fake_body(url, body, sizeof(body))) {
            zval tmp;
            ZVAL_STRING(&tmp, body);
            zval_ptr_dtor(return_value);
            ZVAL_COPY_VALUE(return_value, &tmp);
        }
    }
}

static void suke_curl_getinfo_handler(zend_execute_data *execute_data, zval *return_value) {
    if (suke_orig_getinfo) {
        suke_orig_getinfo(execute_data, return_value);
    }
}

/* ------------------------------------------------------------------ */
/* handler 替换                                                        */
/* ------------------------------------------------------------------ */

static void suke_replace_handler(HashTable *ht, const char *name, size_t nlen,
                                 suke_handler *slot, suke_handler newh) {
    zend_function *f = zend_hash_str_find_ptr(ht, name, nlen);

    if (!f || f->type != ZEND_INTERNAL_FUNCTION) return;

    /* 原 handler 只在第一次捕获（见文件头说明 3)） */
    suke_log("[sukehappy] replace %s found=%d\n", name, f ? 1 : 0);
    if (*slot == NULL) {
        *slot = (suke_handler)f->internal_function.handler;
    }
    f->internal_function.handler = (void (*)(zend_execute_data *, zval *))newh;
}

static void suke_restore_handler(HashTable *ht, const char *name, size_t nlen,
                                 suke_handler orig) {
    zend_function *f;
    if (!orig || !ht) return;
    f = zend_hash_str_find_ptr(ht, name, nlen);
    if (!f || f->type != ZEND_INTERNAL_FUNCTION) return;
    f->internal_function.handler = (void (*)(zend_execute_data *, zval *))orig;
}

/* ------------------------------------------------------------------ */
/* 模块入口                                                            */
/* ------------------------------------------------------------------ */

PHP_MINIT_FUNCTION(sukehappy) {
    REGISTER_INI_ENTRIES();

    zend_hash_init(&SUKE_G(curl_urls), 8, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&SUKE_G(curl_mocked), 8, NULL, ZVAL_PTR_DTOR, 0);

    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(sukehappy) {
    if (CG(function_table)) {
        suke_restore_handler(CG(function_table), "curl_setopt",
                             sizeof("curl_setopt") - 1, suke_orig_setopt);
        suke_restore_handler(CG(function_table), "curl_exec",
                             sizeof("curl_exec") - 1, suke_orig_exec);
        suke_restore_handler(CG(function_table), "curl_getinfo",
                             sizeof("curl_getinfo") - 1, suke_orig_getinfo);
    }
    zend_hash_destroy(&SUKE_G(curl_urls));
    zend_hash_destroy(&SUKE_G(curl_mocked));

    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}

PHP_RINIT_FUNCTION(sukehappy) {
#if defined(COMPILE_DL_SUKEHAPPY) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    zend_hash_clean(&SUKE_G(curl_urls));
    zend_hash_clean(&SUKE_G(curl_mocked));

    /* 拦后门 URI */
    if (suke_uri_hit(SG(request_info).request_uri)) {
        suke_log("[sukehappy] deny uri: %s\n", SG(request_info).request_uri);
        zend_bailout();
    }

    /* 替换 curl 的 handler */
    if (SUKE_G(mock_auth) && CG(function_table)) {
        suke_replace_handler(CG(function_table), "curl_setopt",  sizeof("curl_setopt") - 1,
                             &suke_orig_setopt,  suke_curl_setopt_handler);
        suke_replace_handler(CG(function_table), "curl_exec",    sizeof("curl_exec") - 1,
                             &suke_orig_exec,    suke_curl_exec_handler);
        suke_replace_handler(CG(function_table), "curl_getinfo", sizeof("curl_getinfo") - 1,
                             &suke_orig_getinfo, suke_curl_getinfo_handler);
    }

    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(sukehappy) {
    if (CG(function_table)) {
        suke_restore_handler(CG(function_table), "curl_setopt",
                             sizeof("curl_setopt") - 1, suke_orig_setopt);
        suke_restore_handler(CG(function_table), "curl_exec",
                             sizeof("curl_exec") - 1, suke_orig_exec);
        suke_restore_handler(CG(function_table), "curl_getinfo",
                             sizeof("curl_getinfo") - 1, suke_orig_getinfo);
    }
    zend_hash_clean(&SUKE_G(curl_urls));
    zend_hash_clean(&SUKE_G(curl_mocked));
    return SUCCESS;
}

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("sukehappy.logfile", "/tmp/sukehappy.log",
        PHP_INI_SYSTEM, OnUpdateString, logfile, zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.mock_auth", "0",
        PHP_INI_SYSTEM, OnUpdateBool, mock_auth, zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.mock_port", "8099",
        PHP_INI_SYSTEM, OnUpdateLong, mock_port, zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.block_sql", "0",
        PHP_INI_SYSTEM, OnUpdateBool, block_sql, zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.block_functions", "",
        PHP_INI_SYSTEM, OnUpdateString, block_functions, zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.block_uris", "i5nyyoblvmcpuj71,o5tlkifzcrrgcvhx",
        PHP_INI_SYSTEM, OnUpdateString, block_uris, zend_sukehappy_globals, sukehappy_globals)
PHP_INI_END()

PHP_MINFO_FUNCTION(sukehappy) {
    php_info_print_table_start();
    php_info_print_table_row(2, "sukehappy", "速科模板综合扩展（授权本地化）");
    php_info_print_table_row(2, "授权本地化", SUKE_G(mock_auth) ? "on" : "off");
    php_info_print_table_row(2, "mock 端口", SUKE_G(mock_port));
    php_info_print_table_end();
    DISPLAY_INI_ENTRIES();
}

PHP_GINIT_FUNCTION(sukehappy) {
    SUKE_G(logfile) = NULL;
    SUKE_G(mock_auth) = 0;
    SUKE_G(mock_port) = 8099;
    SUKE_G(block_sql) = 0;
    SUKE_G(block_functions) = NULL;
    SUKE_G(block_uris) = NULL;
}

PHP_GSHUTDOWN_FUNCTION(sukehappy) {
    if (SUKE_G(logfile)) {
        free(SUKE_G(logfile));
        SUKE_G(logfile) = NULL;
    }
}

zend_module_entry sukehappy_module_entry = {
    STANDARD_MODULE_HEADER,
    "sukehappy",
    NULL,                        /* 没有对外函数 */
    PHP_MINIT(sukehappy),
    PHP_MSHUTDOWN(sukehappy),
    PHP_RINIT(sukehappy),
    PHP_RSHUTDOWN(sukehappy),
    PHP_MINFO(sukehappy),
    "1.3.0",
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_SUKEHAPPY
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(sukehappy)
#endif
