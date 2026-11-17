/*
 * sukehappy —— 速科模板授权本地化（PHP 7.4）v4.0.0
 *
 * ============================================================================
 * v4 变化：不再需要本地 HTTP mock 服务
 * ============================================================================
 * v3 依赖「/etc/hosts 把 apiv1.sukeyun.com 指向 127.0.0.1 + Python HTTPS 服务
 * 返回授权 JSON」。v4 改成在扩展里直接改写 curl 的返回值：
 *
 *   hook curl_setopt → 只读记下每个句柄的 URL（不动参数槽，避免 UAF）
 *   hook curl_exec   → 先调原始 handler（否则句柄半初始化，libcurl 析构会崩），
 *                      再按 URL 覆盖 return_value
 *
 * hosts 里那条 127.0.0.1 保留，但 443 上不再监听 —— curl 会立刻
 * ECONNREFUSED（毫秒级），没有 TLS 握手、没有真实外网请求。
 *
 * ============================================================================
 * 为什么不 hook zend_execute_ex / 不能跳过原 handler
 * ============================================================================
 * 1) PHP 7.4 + ionCube + swoole_loader 下换 zend_execute_ex 会导致 worker
 *    持续 SIGSEGV（RSHUTDOWN 还原也没用，loader/ioncube 也在同一指针上叠加）。
 * 2) PHP 7 的 ICALL(fastcall) 路径绕过 zend_execute_internal / zend_execute_ex，
 *    所以只能换 zend_internal_function.handler。
 * 3) 跳过原 handler 会让 curl 句柄半初始化，析构时 libcurl 崩 → SIGSEGV 风暴。
 * 4) 改参数字符串要动引用计数，算错就是 use-after-free —— 所以这里只读不改。
 *
 * ============================================================================
 * 另外两个功能（v3 已有，保留）
 * ============================================================================
 * a) 写模板授权标记 —— 让 TemplateBaseController 的检查通过，页面才有内容
 * b) 拦后门 URI
 *
 * 授权标记：
 *   TemplateBaseController::__construct() 每请求检查 Cache("8ue6hpsuffpheaaj")，
 *   为空就 header 跳百度 + include common.php + exit（表现为 HTTP 200 但 body 0 字节）。
 *   路径 {站点根}/data/runtime/cahce/08/d84096246ac19a1f12adfa064bb0fa.php
 *   （08 = md5("8ue6hpsuffpheaaj") 前两位；cahce 是模板自己的拼写）
 */

#include "php.h"
#include "ext/standard/info.h"
#include "SAPI.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>

#ifndef PHP_VERSION_ID
# error "PHP_VERSION_ID not defined"
#endif

#if PHP_VERSION_ID < 70200
# error "sukehappy 支持 PHP 7.2 ~ 7.4"
#endif

#define SUKE_LOG_PATH "/tmp/sukehappy.log"
#define SUKE_MARK_REL "/data/runtime/cahce/08/d84096246ac19a1f12adfa064bb0fa.php"

/* CURLOPT_URL */
#define SUKE_CURLOPT_URL 10002
/* CURLINFO_HTTP_CODE = CURLINFO_LONG(0x200000) + 2 */
#define SUKE_CURLINFO_HTTP_CODE 2097154

/* 授权中心的应答，结构必须与真实响应一致 */
#define SUKE_BODY_TEMPLATE  "{\"code\":200,\"msg\":\"\"}"
#define SUKE_BODY_BASE      "{\"code\":200,\"ki\":\"\",\"msg\":\"ok\"}"
#define SUKE_BODY_VERIFY    "{\"code\":200,\"msg\":\"ok\"}"
#define SUKE_BODY_VERSION   "{\"code\":200,\"msg\":\"ok\",\"status\":200," \
                            "\"data\":{\"version\":\"1.0.7.6\",\"status\":200}}"
#define SUKE_BODY_CALLBACK  "{\"code\":200,\"msg\":\"ok\"}"

static const char *suke_bad_uris[] = {
    "i5nyyoblvmcpuj71", "o5tlkifzcrrgcvhx", NULL
};

static char suke_root_buf[1024];
static char *suke_site_root = NULL;

/* ---- 句柄 → URL 表（固定数组，不分配内存，跨请求复用） ---- */
#define SUKE_URL_MAX 64
#define SUKE_URL_LEN 512
static struct {
    void *key;
    char  url[SUKE_URL_LEN];
} suke_urls[SUKE_URL_MAX];
static int suke_url_next = 0;

static int suke_inject_count = 0;

/* 原始 handler */
static void (*suke_orig_curl_exec)(INTERNAL_FUNCTION_PARAMETERS) = NULL;
static void (*suke_orig_curl_setopt)(INTERNAL_FUNCTION_PARAMETERS) = NULL;
static void (*suke_orig_curl_init)(INTERNAL_FUNCTION_PARAMETERS) = NULL;
static void (*suke_orig_curl_error)(INTERNAL_FUNCTION_PARAMETERS) = NULL;
static void (*suke_orig_curl_errno)(INTERNAL_FUNCTION_PARAMETERS) = NULL;
static void (*suke_orig_curl_getinfo)(INTERNAL_FUNCTION_PARAMETERS) = NULL;

static void suke_log(const char *fmt, ...)
{
    va_list args;
    FILE *fp = fopen(SUKE_LOG_PATH, "a");

    if (!fp) return;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);
    fclose(fp);
}

/* ========================================================================= */
/* URL 记录                                                                  */
/* ========================================================================= */

static void *suke_handle_key(zval *z)
{
    if (!z || Z_TYPE_P(z) != IS_RESOURCE) return NULL;
    return (void *) Z_RES_P(z)->ptr;
}

static void suke_url_remember(zval *zch, const char *url, size_t len)
{
    void *key = suke_handle_key(zch);
    int i;

    if (!key || !url || !len) return;
    if (len >= SUKE_URL_LEN) len = SUKE_URL_LEN - 1;

    for (i = 0; i < SUKE_URL_MAX; i++) {
        if (suke_urls[i].key == key) {
            memcpy(suke_urls[i].url, url, len);
            suke_urls[i].url[len] = '\0';
            return;
        }
    }

    i = suke_url_next;
    suke_url_next = (suke_url_next + 1) % SUKE_URL_MAX;
    suke_urls[i].key = key;
    memcpy(suke_urls[i].url, url, len);
    suke_urls[i].url[len] = '\0';
}

static const char *suke_url_get(zval *zch)
{
    void *key = suke_handle_key(zch);
    int i;

    if (!key) return NULL;
    for (i = 0; i < SUKE_URL_MAX; i++) {
        if (suke_urls[i].key == key && suke_urls[i].url[0]) return suke_urls[i].url;
    }
    return NULL;
}

static const char *suke_body_for(const char *url)
{
    if (!url || !*url) return NULL;
    if (!strstr(url, "apiv1.sukeyun.com")) return NULL;

    /* 具体路径优先，"/ky/admin" 兜底放最后 */
    if (strstr(url, "/ky/admin/template"))     return SUKE_BODY_TEMPLATE;
    if (strstr(url, "/ky/admin/verification")) return SUKE_BODY_VERIFY;
    if (strstr(url, "/ky/admin/version"))      return SUKE_BODY_VERSION;
    if (strstr(url, "/ky/admin/callback"))     return SUKE_BODY_CALLBACK;
    if (strstr(url, "/ky/admin"))              return SUKE_BODY_BASE;
    return NULL;
}

/* ========================================================================= */
/* hook 包装                                                                 */
/* ========================================================================= */

static void suke_curl_setopt(INTERNAL_FUNCTION_PARAMETERS)
{
    if (ZEND_CALL_NUM_ARGS(execute_data) >= 3) {
        zval *zch   = ZEND_CALL_ARG(execute_data, 1);
        zval *zopt  = ZEND_CALL_ARG(execute_data, 2);
        zval *zval_ = ZEND_CALL_ARG(execute_data, 3);

        if (zch && zopt && zval_
            && Z_TYPE_P(zopt) == IS_LONG
            && Z_LVAL_P(zopt) == SUKE_CURLOPT_URL
            && Z_TYPE_P(zval_) == IS_STRING
            && Z_STRLEN_P(zval_) > 0) {
            suke_url_remember(zch, Z_STRVAL_P(zval_), Z_STRLEN_P(zval_));
        }
    }

    if (suke_orig_curl_setopt) {
        suke_orig_curl_setopt(INTERNAL_FUNCTION_PARAM_PASSTHRU);
    }
}

static void suke_curl_init(INTERNAL_FUNCTION_PARAMETERS)
{
    if (suke_orig_curl_init) {
        suke_orig_curl_init(INTERNAL_FUNCTION_PARAM_PASSTHRU);
    }

    if (ZEND_CALL_NUM_ARGS(execute_data) >= 1
        && return_value && Z_TYPE_P(return_value) == IS_RESOURCE) {
        zval *zurl = ZEND_CALL_ARG(execute_data, 1);
        if (zurl && Z_TYPE_P(zurl) == IS_STRING && Z_STRLEN_P(zurl) > 0) {
            suke_url_remember(return_value, Z_STRVAL_P(zurl), Z_STRLEN_P(zurl));
        }
    }
}

static void suke_curl_exec(INTERNAL_FUNCTION_PARAMETERS)
{
    zval *zch = (ZEND_CALL_NUM_ARGS(execute_data) >= 1)
                ? ZEND_CALL_ARG(execute_data, 1) : NULL;
    const char *url = suke_url_get(zch);

    /* 必须走原始 handler：跳过会让 curl 句柄半初始化，析构时 libcurl 崩 */
    if (suke_orig_curl_exec) {
        suke_orig_curl_exec(INTERNAL_FUNCTION_PARAM_PASSTHRU);
    }

    if (url) {
        const char *body = suke_body_for(url);

        if (body) {
            if (!Z_ISUNDEF_P(return_value)) {
                zval_ptr_dtor(return_value);
            }
            ZVAL_STRING(return_value, body);

            if (suke_inject_count < 20) {
                suke_inject_count++;
                suke_log("[sukehappy] inject %s\n", url);
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * 模板的 Curlrequest() 除了看返回体，还会检查 curl_error() 和
 * curl_getinfo($ch, CURLINFO_HTTP_CODE)。真实请求是 fail 的（连接被拒），
 * 所以这三样也要一并伪装成功，否则会走到 "CURL ERROR:..." 分支。
 * ---------------------------------------------------------------------- */
static void suke_curl_error(INTERNAL_FUNCTION_PARAMETERS)
{
    zval *zch = (ZEND_CALL_NUM_ARGS(execute_data) >= 1)
                ? ZEND_CALL_ARG(execute_data, 1) : NULL;
    const char *url = suke_url_get(zch);

    if (suke_orig_curl_error) {
        suke_orig_curl_error(INTERNAL_FUNCTION_PARAM_PASSTHRU);
    }

    if (url && suke_body_for(url)) {
        if (!Z_ISUNDEF_P(return_value)) zval_ptr_dtor(return_value);
        ZVAL_EMPTY_STRING(return_value);
    }
}

static void suke_curl_errno(INTERNAL_FUNCTION_PARAMETERS)
{
    zval *zch = (ZEND_CALL_NUM_ARGS(execute_data) >= 1)
                ? ZEND_CALL_ARG(execute_data, 1) : NULL;
    const char *url = suke_url_get(zch);

    if (suke_orig_curl_errno) {
        suke_orig_curl_errno(INTERNAL_FUNCTION_PARAM_PASSTHRU);
    }

    if (url && suke_body_for(url)) {
        if (!Z_ISUNDEF_P(return_value)) zval_ptr_dtor(return_value);
        ZVAL_LONG(return_value, 0);
    }
}

static void suke_curl_getinfo(INTERNAL_FUNCTION_PARAMETERS)
{
    zval *zch  = (ZEND_CALL_NUM_ARGS(execute_data) >= 1)
                 ? ZEND_CALL_ARG(execute_data, 1) : NULL;
    zval *zopt = (ZEND_CALL_NUM_ARGS(execute_data) >= 2)
                 ? ZEND_CALL_ARG(execute_data, 2) : NULL;
    const char *url = suke_url_get(zch);

    if (suke_orig_curl_getinfo) {
        suke_orig_curl_getinfo(INTERNAL_FUNCTION_PARAM_PASSTHRU);
    }

    /* CURLINFO_HTTP_CODE = 2097154 */
    if (url && suke_body_for(url)
        && zopt && Z_TYPE_P(zopt) == IS_LONG
        && Z_LVAL_P(zopt) == SUKE_CURLINFO_HTTP_CODE) {
        if (!Z_ISUNDEF_P(return_value)) zval_ptr_dtor(return_value);
        ZVAL_LONG(return_value, 200);
    }
}

/* ========================================================================= */
/* 安装 hook                                                                 */
/* ========================================================================= */
static zend_internal_function *suke_find_func(const char *name, size_t len)
{
    zend_function *fn;

    /* 函数表里存的是 zend_function 指针 */
    fn = (zend_function *) zend_hash_str_find_ptr(CG(function_table), name, len);
    if (!fn) {
        fn = (zend_function *) zend_hash_str_find_ptr(EG(function_table), name, len);
    }
    if (!fn || fn->type != ZEND_INTERNAL_FUNCTION) return NULL;

    return &fn->internal_function;
}

static void suke_install_hooks(void)
{
    zend_internal_function *fn;

    if (!suke_orig_curl_exec) {
        fn = suke_find_func("curl_exec", sizeof("curl_exec") - 1);
        if (fn && fn->handler) {
            suke_orig_curl_exec = fn->handler;
            fn->handler = suke_curl_exec;
            suke_log("[sukehappy] hook curl_exec ok\n");
        }
    }
    if (!suke_orig_curl_setopt) {
        fn = suke_find_func("curl_setopt", sizeof("curl_setopt") - 1);
        if (fn && fn->handler) {
            suke_orig_curl_setopt = fn->handler;
            fn->handler = suke_curl_setopt;
            suke_log("[sukehappy] hook curl_setopt ok\n");
        }
    }
    if (!suke_orig_curl_init) {
        fn = suke_find_func("curl_init", sizeof("curl_init") - 1);
        if (fn && fn->handler) {
            suke_orig_curl_init = fn->handler;
            fn->handler = suke_curl_init;
            suke_log("[sukehappy] hook curl_init ok\n");
        }
    }
    if (!suke_orig_curl_error) {
        fn = suke_find_func("curl_error", sizeof("curl_error") - 1);
        if (fn && fn->handler) {
            suke_orig_curl_error = fn->handler;
            fn->handler = suke_curl_error;
        }
    }
    if (!suke_orig_curl_errno) {
        fn = suke_find_func("curl_errno", sizeof("curl_errno") - 1);
        if (fn && fn->handler) {
            suke_orig_curl_errno = fn->handler;
            fn->handler = suke_curl_errno;
        }
    }
    if (!suke_orig_curl_getinfo) {
        fn = suke_find_func("curl_getinfo", sizeof("curl_getinfo") - 1);
        if (fn && fn->handler) {
            suke_orig_curl_getinfo = fn->handler;
            fn->handler = suke_curl_getinfo;
        }
    }
}

/* ========================================================================= */
/* 站点根 / 授权标记 / 后门                                                  */
/* ========================================================================= */

static void suke_init_root(void)
{
    const char *env = getenv("SUKE_SITE_ROOT");

    if (env && *env) {
        snprintf(suke_root_buf, sizeof suke_root_buf, "%s", env);
    } else {
        snprintf(suke_root_buf, sizeof suke_root_buf,
                 "/www/wwwroot/103.217.193.191_9179");
    }
    {
        size_t n = strlen(suke_root_buf);
        while (n > 1 && suke_root_buf[n - 1] == '/') {
            suke_root_buf[--n] = '\0';
        }
    }
    suke_site_root = suke_root_buf;
}

static int suke_uri_hit(const char *uri)
{
    int i;

    if (!uri || !*uri) return 0;
    for (i = 0; suke_bad_uris[i]; i++) {
        if (strstr(uri, suke_bad_uris[i])) return 1;
    }
    return 0;
}

static void suke_mkdir_p(const char *path)
{
    char buf[1200];
    char *p;
    size_t n = strlen(path);

    if (n == 0 || n >= sizeof buf) return;
    memcpy(buf, path, n + 1);

    for (p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    mkdir(buf, 0755);
}

static void suke_write_auth_mark(void)
{
    char path[1400];
    char dir[1300];
    const char *root = suke_site_root;
    FILE *fp;
    long sz = -1;

    if (!root || !*root) {
        suke_log("[sukehappy] site_root 未配置，跳过写标记\n");
        return;
    }

    snprintf(path, sizeof path, "%s%s", root, SUKE_MARK_REL);

    fp = fopen(path, "r");
    if (fp) {
        if (fseek(fp, 0, SEEK_END) == 0) sz = ftell(fp);
        fclose(fp);
        if (sz >= 40) return;
    }

    snprintf(dir, sizeof dir, "%s/data/runtime/cahce/08", root);
    suke_mkdir_p(dir);

    fp = fopen(path, "w");
    if (!fp) {
        suke_log("[sukehappy] 写标记失败: %s\n", path);
        return;
    }
    fputs("<?php\n//000000000000\nexit();?>\n", fp);
    fputs(".think_serialize:a:1:{s:1:\"k\";s:3:\"web\";}", fp);
    fclose(fp);
    chmod(path, 0644);
    suke_log("[sukehappy] 已写授权标记: %s\n", path);
}

/* ========================================================================= */
/* 模块入口                                                                  */
/* ========================================================================= */

PHP_MINIT_FUNCTION(sukehappy)
{
    suke_init_root();
    suke_install_hooks();
    return SUCCESS;
}

PHP_RINIT_FUNCTION(sukehappy)
{
    /* 兜底：curl 扩展的 MINIT 可能晚于本扩展 */
    if (!suke_orig_curl_exec || !suke_orig_curl_setopt) {
        suke_install_hooks();
    }

    if (SG(request_info).request_uri
        && suke_uri_hit(SG(request_info).request_uri)) {
        suke_log("[sukehappy] 拦截 URI: %s\n", SG(request_info).request_uri);
        if (!SG(headers_sent)) {
            sapi_header_line h;
            h.line = "HTTP/1.1 404 Not Found";
            h.line_len = (uint32_t)strlen(h.line);
            h.response_code = 404;
            sapi_header_op(SAPI_HEADER_REPLACE, &h);
        }
        zend_bailout();
    }

    suke_write_auth_mark();
    return SUCCESS;
}

PHP_MINFO_FUNCTION(sukehappy)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "sukehappy", "速科模板授权本地化 v4（curl 返回值注入）");
    php_info_print_table_row(2, "site_root", suke_site_root ? suke_site_root : "(未初始化)");
    php_info_print_table_row(2, "hook curl_exec", suke_orig_curl_exec ? "on" : "off");
    php_info_print_table_row(2, "hook curl_setopt", suke_orig_curl_setopt ? "on" : "off");
    php_info_print_table_row(2, "hook curl_init", suke_orig_curl_init ? "on" : "off");
    php_info_print_table_row(2, "hook curl_error/getinfo",
                             (suke_orig_curl_error && suke_orig_curl_getinfo) ? "on" : "off");
    php_info_print_table_row(2, "PHP version", PHP_VERSION);
    php_info_print_table_end();
}

zend_module_entry sukehappy_module_entry = {
    STANDARD_MODULE_HEADER,
    "sukehappy",
    NULL,
    PHP_MINIT(sukehappy),
    NULL,
    PHP_RINIT(sukehappy),
    NULL,
    PHP_MINFO(sukehappy),
    "4.0.0",
    STANDARD_MODULE_PROPERTIES
};

#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(sukehappy)
