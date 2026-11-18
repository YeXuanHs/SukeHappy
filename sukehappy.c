/*
 * 1.写授权标记文件，让后端检测授权通过
 * 2.hook curl_*，把授权接口的应答换成已授权
 * 3.拦截后门
 */

#include "php.h"
#include "php_sukehappy.h"
#include "ext/standard/info.h"
#include "SAPI.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <unistd.h>

#ifndef PHP_VERSION_ID
# error "PHP_VERSION_ID not defined"
#endif

#if PHP_VERSION_ID < 70200
# error "sukehappy 仅支持 PHP 7.2 ~ 7.4"
#endif

#define SUKE_LOG_PATH "/tmp/sukehappy.log"
#define SUKE_MARK_REL "/data/runtime/cahce/08/d84096246ac19a1f12adfa064bb0fa.php"
/* 授权密钥：与 jwt.php 里 SUKE_JWT_SECRET 必须一致 */
#define SUKE_KI     "k7Qm2Xp9Za4Lr8Tn6Vc3Wd1Yb5Hs0Jg_"
/* Cache("hzdtqmwbodere7d9") 的落盘文件（md5 前两位做子目录） */
#define SUKE_KI_REL "/data/runtime/cahce/31/1ccd705bdb0aa5ecc79f8d96274d43.php"

/* CURLOPT_URL */
#define SUKE_CURLOPT_URL 10002
/* CURLINFO_HTTP_CODE */
#define SUKE_CURLINFO_HTTP_CODE 2097154

/* 授权接口伪造 */
#define SUKE_BODY_TEMPLATE  "{\"code\":200,\"msg\":\"\"}"
#define SUKE_BODY_BASE      "{\"code\":200,\"ki\":\"k7Qm2Xp9Za4Lr8Tn6Vc3Wd1Yb5Hs0Jg_\",\"msg\":\"ok\"}"
#define SUKE_BODY_VERIFY    "{\"code\":200,\"msg\":\"ok\"}"
#define SUKE_BODY_VERSION   "{\"code\":200,\"msg\":\"ok\",\"status\":200," \
                            "\"data\":{\"version\":\"1.0.7.6\",\"status\":200}}"
#define SUKE_BODY_CALLBACK  "{\"code\":200,\"msg\":\"ok\"}"

static const char *suke_bad_uris[] = {
    "i5nyyoblvmcpuj71", "o5tlkifzcrrgcvhx", NULL
};

static char suke_root_buf[1024];
static char *suke_site_root = NULL;

/* 句柄 - URL 表 */
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

/* --- URL 记录 --- */

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

    /* 路径优先级，"/ky/admin" 兜底放最后 */
    if (strstr(url, "/ky/admin/template"))     return SUKE_BODY_TEMPLATE;
    if (strstr(url, "/ky/admin/verification")) return SUKE_BODY_VERIFY;
    if (strstr(url, "/ky/admin/version"))      return SUKE_BODY_VERSION;
    if (strstr(url, "/ky/admin/callback"))     return SUKE_BODY_CALLBACK;
    if (strstr(url, "/ky/admin"))              return SUKE_BODY_BASE;
    return NULL;
}

/* --- hook 包装 --- */

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

    /* 走原始 handler */
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

/* curl_error / curl_getinfo，一并伪装成功，否则走 CURL ERROR 分支 */
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

    if (url && suke_body_for(url)
        && zopt && Z_TYPE_P(zopt) == IS_LONG
        && Z_LVAL_P(zopt) == SUKE_CURLINFO_HTTP_CODE) {
        if (!Z_ISUNDEF_P(return_value)) zval_ptr_dtor(return_value);
        ZVAL_LONG(return_value, 200);
    }
}

/* --- 安装 hook --- */
static zend_internal_function *suke_find_func(const char *name, size_t len)
{
    zend_function *fn;

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

/* --- 站点根 / 授权标记 / 后门 --- */

static int suke_root_fixed = 0;

/* 试着把某个路径当作站点根：data/runtime 存在就算命中 */
static int suke_try_root(const char *cand)
{
    char probe[1500];
    if (!cand || !*cand) return 0;
    snprintf(probe, sizeof probe, "%s/data/runtime", cand);
    if (access(probe, F_OK) != 0) return 0;
    snprintf(suke_root_buf, sizeof suke_root_buf, "%s", cand);
    return 1;
}

/* 由当前脚本路径反推站点根：/site/public/xxx.php -> /site，/site/index.php -> /site */
static void suke_derive_root(void)
{
    char tmp[1400];
    char *slash;
    const char *pt = SG(request_info).path_translated;

    if (!pt || !*pt) return;
    snprintf(tmp, sizeof tmp, "%s", pt);

    slash = strrchr(tmp, '/');
    if (!slash) return;
    *slash = '\0';

    if (suke_try_root(tmp)) { suke_root_fixed = 1; return; }

    slash = strrchr(tmp, '/');
    if (!slash) return;
    *slash = '\0';

    if (suke_try_root(tmp)) suke_root_fixed = 1;
}

static void suke_init_root(void)
{
    const char *env = getenv("SUKE_SITE_ROOT");

    if (env && *env) {
        snprintf(suke_root_buf, sizeof suke_root_buf, "%s", env);
        suke_root_fixed = 1;
    } else {
        /* 未知站点根：先置空，RINIT 里按脚本路径反推 */
        suke_root_buf[0] = '\0';
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

/* 写授权密钥缓存：模板的 /v33 用它验 JWT，空着会一路返回「请求失败，-2」 */
static void suke_write_ki_cache(void)
{
    char path[1400];
    char dir[1300];
    const char *root = suke_site_root;
    FILE *fp;
    long sz = -1;
    const long want = (long) (strlen("<?php\n//000000000000\n exit();?>\n") + strlen(SUKE_KI));

    if (!root || !*root) return;

    snprintf(path, sizeof path, "%s%s", root, SUKE_KI_REL);

    fp = fopen(path, "r");
    if (fp) {
        if (fseek(fp, 0, SEEK_END) == 0) sz = ftell(fp);
        fclose(fp);
        if (sz == want) return;
    }

    snprintf(dir, sizeof dir, "%s/data/runtime/cahce/31", root);
    suke_mkdir_p(dir);

    fp = fopen(path, "w");
    if (!fp) {
        suke_log("[sukehappy] 写 ki 缓存失败: %s\n", path);
        return;
    }
    fputs("<?php\n//000000000000\n exit();?>\n", fp);
    fputs(SUKE_KI, fp);
    fclose(fp);
    chmod(path, 0644);
    suke_log("[sukehappy] 已写 ki 缓存: %s\n", path);
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

/* --- 模块入口 --- */

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

    if (!suke_root_fixed) {
        suke_derive_root();
    }

    suke_write_auth_mark();
    suke_write_ki_cache();
    return SUCCESS;
}

PHP_MINFO_FUNCTION(sukehappy)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "sukehappy", "速科模板授权本地化 " PHP_SUKEHAPPY_VERSION);
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
    "1.1.0",
    STANDARD_MODULE_PROPERTIES
};

#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(sukehappy)
