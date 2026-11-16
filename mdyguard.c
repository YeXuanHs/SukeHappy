/* mdyguard —— 拦截「SK模板(财务)」后门
 * 拦两类：
 *   1) 请求 URI 命中隐蔽路由（i5nyyoblvmcpuj71 / o5tlkifzcrrgcvhx）
 *   2) 执行到被拉黑的函数（greatMove / e2iz72t17dzwmbnv ...）
 * 命中即 404 + bailout，不产生任何输出。
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "SAPI.h"
#include "Zend/zend_exceptions.h"
#include "ext/standard/info.h"
#include "php_mdyguard.h"

ZEND_DECLARE_MODULE_GLOBALS(mdyguard)

static void (*mdy_prev_execute_ex)(zend_execute_data *execute_data);

static void mdy_log(const char *fmt, ...)
{
    char path[1024];
    FILE *fp;
    va_list ap;

    if (!MDYGUARD_G(logfile) || !*MDYGUARD_G(logfile)) {
        return;
    }
    snprintf(path, sizeof(path), "%s", MDYGUARD_G(logfile));
    fp = fopen(path, "a");
    if (!fp) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fclose(fp);
}

/* 逗号分隔列表里是否有 word */
static int mdy_in_list(const char *word, const char *list)
{
    const char *p;
    size_t n;

    if (!word || !list || !*list) {
        return 0;
    }
    n = strlen(word);
    p = list;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        while (len && (*p == ' ' || *p == '\t')) { p++; len--; }
        while (len && (p[len - 1] == ' ' || p[len - 1] == '\t')) { len--; }
        if (len == n && strncasecmp(p, word, n) == 0) {
            return 1;
        }
        if (!e) {
            break;
        }
        p = e + 1;
    }
    return 0;
}

/* 子串命中（URI 用） */
static int mdy_uri_hit(const char *uri)
{
    const char *p, *list;

    if (!uri || !*uri) {
        return 0;
    }
    list = MDYGUARD_G(block_uris);
    if (!list || !*list) {
        return 0;
    }
    p = list;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char tok[256];
        if (len > 0 && len < sizeof(tok)) {
            memcpy(tok, p, len);
            tok[len] = '\0';
            if (*tok && strstr(uri, tok)) {
                return 1;
            }
        }
        if (!e) {
            break;
        }
        p = e + 1;
    }
    return 0;
}

static void mdy_deny(const char *why, const char *detail)
{
    if (strcmp(sapi_module.name, "cli") != 0) {
        sapi_header_op(SAPI_HEADER_SET_STATUS, (void *)(zend_long)404);
    }
    mdy_log("[mdyguard] deny %s: %s uri=%s\n", why,
            detail ? detail : "-",
            SG(request_info).request_uri ? SG(request_info).request_uri : "-");
    zend_bailout();
}

static void mdy_execute_ex(zend_execute_data *execute_data)
{
    if (execute_data && execute_data->func
        && execute_data->func->type == ZEND_USER_FUNCTION) {
        zend_function *f = execute_data->func;
        if (f->common.function_name) {
            const char *fn = ZSTR_VAL(f->common.function_name);
            if (mdy_in_list(fn, MDYGUARD_G(block_functions))) {
                mdy_deny("function", fn);
            }
        }
    }
    mdy_prev_execute_ex(execute_data);
}

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("mdyguard.block_functions",
        "greatMove,e2iz72t17dzwmbnv,admin_decrypt",
        PHP_INI_SYSTEM, OnUpdateString, block_functions,
        zend_mdyguard_globals, mdyguard_globals)
    STD_PHP_INI_ENTRY("mdyguard.block_uris",
        "i5nyyoblvmcpuj71,o5tlkifzcrrgcvhx",
        PHP_INI_SYSTEM, OnUpdateString, block_uris,
        zend_mdyguard_globals, mdyguard_globals)
    STD_PHP_INI_ENTRY("mdyguard.logfile", "",
        PHP_INI_SYSTEM, OnUpdateString, logfile,
        zend_mdyguard_globals, mdyguard_globals)
PHP_INI_END()

static PHP_GINIT_FUNCTION(mdyguard)
{
#if defined(COMPILE_DL_MDYGUARD) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    mdyguard_globals->block_functions = NULL;
    mdyguard_globals->block_uris = NULL;
    mdyguard_globals->logfile = NULL;
}

static PHP_MINIT_FUNCTION(mdyguard)
{
    REGISTER_INI_ENTRIES();
    return SUCCESS;
}

static PHP_MSHUTDOWN_FUNCTION(mdyguard)
{
    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}

static PHP_RINIT_FUNCTION(mdyguard)
{
#if defined(COMPILE_DL_MDYGUARD) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    /* ① 先看 URI，命中隐蔽路由直接掐掉 */
    if (mdy_uri_hit(SG(request_info).request_uri)) {
        mdy_deny("uri", SG(request_info).request_uri);
    }
    /* ② 挂 execute_ex，拦函数级 */
    mdy_prev_execute_ex = zend_execute_ex;
    zend_execute_ex = mdy_execute_ex;
    return SUCCESS;
}

static PHP_RSHUTDOWN_FUNCTION(mdyguard)
{
    if (mdy_prev_execute_ex) {
        zend_execute_ex = mdy_prev_execute_ex;
        mdy_prev_execute_ex = NULL;
    }
    return SUCCESS;
}

static PHP_MINFO_FUNCTION(mdyguard)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "mdyguard", "enabled");
    php_info_print_table_row(2, "blocked functions",
        MDYGUARD_G(block_functions) ? MDYGUARD_G(block_functions) : "");
    php_info_print_table_row(2, "blocked uris",
        MDYGUARD_G(block_uris) ? MDYGUARD_G(block_uris) : "");
    php_info_print_table_end();
    DISPLAY_INI_ENTRIES();
}

zend_module_entry mdyguard_module_entry = {
    STANDARD_MODULE_HEADER,
    "mdyguard",
    NULL,                      /* functions */
    PHP_MINIT(mdyguard),
    PHP_MSHUTDOWN(mdyguard),
    PHP_RINIT(mdyguard),
    PHP_RSHUTDOWN(mdyguard),
    PHP_MINFO(mdyguard),
    PHP_MDYGUARD_VERSION,
    PHP_MODULE_GLOBALS(mdyguard),
    PHP_GINIT(mdyguard),
    NULL,
    NULL,
    STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_MDYGUARD
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(mdyguard)
#endif
