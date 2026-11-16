/* sukehappy —— 拦截「SK模板(财务)」后门
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
#include "php_sukehappy.h"

ZEND_DECLARE_MODULE_GLOBALS(sukehappy)

static void (*suke_prev_execute_ex)(zend_execute_data *execute_data);
static void suke_log(const char *fmt, ...);
static void suke_deny(const char *why, const char *detail);
static void (*suke_prev_execute_internal)(zend_execute_data *execute_data, zval *return_value);

/* SQL 兜底层：只拦「毁灭性/外带」语句，不拦普通 INSERT/UPDATE（否则会打挂业务） */
static const char *suke_sql_bad[] = {
    "drop table", "drop database", "truncate ", "alter table",
    "into outfile", "into dumpfile", "load_file(", "load data",
    "create user", "grant ", "into dumpfile",
    NULL
};

static int suke_sql_dangerous(const char *sql)
{
    char buf[4096];
    size_t i, n;
    const char **p;

    if (!sql) {
        return 0;
    }
    n = strlen(sql);
    if (n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    for (i = 0; i < n; i++) {
        char c = sql[i];
        buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    buf[n] = '\0';
    for (p = suke_sql_bad; *p; p++) {
        if (strstr(buf, *p)) {
            return 1;
        }
    }
    return 0;
}

static void suke_execute_internal(zend_execute_data *execute_data, zval *return_value)
{
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

static void suke_log(const char *fmt, ...)
{
    char path[1024];
    FILE *fp;
    va_list ap;

    if (!SUKEHAPPY_G(logfile) || !*SUKEHAPPY_G(logfile)) {
        return;
    }
    snprintf(path, sizeof(path), "%s", SUKEHAPPY_G(logfile));
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
static int suke_in_list(const char *word, const char *list)
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
static int suke_uri_hit(const char *uri)
{
    const char *p, *list;

    if (!uri || !*uri) {
        return 0;
    }
    list = SUKEHAPPY_G(block_uris);
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

static void suke_deny(const char *why, const char *detail)
{
    if (strcmp(sapi_module.name, "cli") != 0) {
        sapi_header_op(SAPI_HEADER_SET_STATUS, (void *)(zend_long)404);
    }
    suke_log("[sukehappy] deny %s: %s uri=%s\n", why,
            detail ? detail : "-",
            SG(request_info).request_uri ? SG(request_info).request_uri : "-");
    zend_bailout();
}

static void suke_execute_ex(zend_execute_data *execute_data)
{
    if (execute_data && execute_data->func
        && execute_data->func->type == ZEND_USER_FUNCTION) {
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

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("sukehappy.block_functions",
        "greatMove,e2iz72t17dzwmbnv,admin_decrypt",
        PHP_INI_SYSTEM, OnUpdateString, block_functions,
        zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.block_uris",
        "i5nyyoblvmcpuj71,o5tlkifzcrrgcvhx",
        PHP_INI_SYSTEM, OnUpdateString, block_uris,
        zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.block_sql", "0",
        PHP_INI_SYSTEM, OnUpdateBool, block_sql,
        zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.logfile", "",
        PHP_INI_SYSTEM, OnUpdateString, logfile,
        zend_sukehappy_globals, sukehappy_globals)
PHP_INI_END()

static PHP_GINIT_FUNCTION(sukehappy)
{
#if defined(COMPILE_DL_SUKEHAPPY) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    sukehappy_globals->block_functions = NULL;
    sukehappy_globals->block_uris = NULL;
    sukehappy_globals->block_sql = 0;
    sukehappy_globals->logfile = NULL;
}

static PHP_MINIT_FUNCTION(sukehappy)
{
    REGISTER_INI_ENTRIES();
    return SUCCESS;
}

static PHP_MSHUTDOWN_FUNCTION(sukehappy)
{
    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}

static PHP_RINIT_FUNCTION(sukehappy)
{
#if defined(COMPILE_DL_SUKEHAPPY) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    /* ① 先看 URI，命中隐蔽路由直接掐掉 */
    if (suke_uri_hit(SG(request_info).request_uri)) {
        suke_deny("uri", SG(request_info).request_uri);
    }
    /* ② 挂 execute_ex，拦函数级 */
    suke_prev_execute_ex = zend_execute_ex;
    zend_execute_ex = suke_execute_ex;

    /* ③ SQL 兜底：拦 DROP/TRUNCATE/OUTFILE 等毁灭性与外带语句 */
    suke_prev_execute_internal = zend_execute_internal;
    zend_execute_internal = suke_execute_internal;
    return SUCCESS;
}

static PHP_RSHUTDOWN_FUNCTION(sukehappy)
{
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

static PHP_MINFO_FUNCTION(sukehappy)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "sukehappy", "enabled");
    php_info_print_table_row(2, "blocked functions",
        SUKEHAPPY_G(block_functions) ? SUKEHAPPY_G(block_functions) : "");
    php_info_print_table_row(2, "block sql (ddl/outfile)", SUKEHAPPY_G(block_sql) ? "on" : "off");
    php_info_print_table_row(2, "blocked uris",
        SUKEHAPPY_G(block_uris) ? SUKEHAPPY_G(block_uris) : "");
    php_info_print_table_end();
    DISPLAY_INI_ENTRIES();
}

zend_module_entry sukehappy_module_entry = {
    STANDARD_MODULE_HEADER,
    "sukehappy",
    NULL,                      /* functions */
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
