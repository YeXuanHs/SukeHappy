/*
 * sukehappy —— 速科模板授权本地化（PHP 7.4）
 *
 * ============================================================================
 * 为什么不 hook zend_execute_ex
 * ============================================================================
 * 实测：PHP 7.4 + ionCube + swoole_loader + 本站模板下，把 zend_execute_ex
 * 换成自己的 wrapper 会导致 worker 持续 SIGSEGV（RSHUTDOWN 还原也没用，
 * 因为 loader/ioncube 自己也会在同一指针上叠加）。独立 php-cgi 上跑得通，
 * FPM 常驻进程下必崩。
 *
 *
 * ============================================================================
 * 完整方案（服务端 + 前端两层，缺一不可）
 * ============================================================================
 * 1) 本扩展（服务端）：
 *    - 写授权标记 —— 让 TemplateBaseController 的检查通过，页面才有内容
 *    - 拦截后门 URI
 * 2) 模板前端 JS（themes 下的明文 js，已修改）：
 *    - themes/web/www/static/script/common.js
 *        去掉 UA 白名单检测（_0x36b13a 直接 return true）
 *        + 去掉动态加载 search.js 的入口（_0x2cae23 改空函数）
 *    - themes/clientarea/userCenter/static/script/common.js
 *        去掉动态加载 search.js 的入口（_0x20e747 改空函数）
 *
 *    为什么：7.4 模板包把「授权校验 + 黑色遮罩 #bodyky + 提示框 #myModal +
 *    "该站点将在 N 秒后自动关闭" + 跳转」整条链都放在**动态加载的 search.js** 里
 *    （jsjiami.com.v7 混淆：字符串 = base64 + percent-decode + RC4(key)）。
 *    把「加载 search.js」这一个入口改成空函数，整条链就断了 ——
 *    不弹窗、不遮罩、不跳转，而且不碰任何加密的模板文件。
 *
 * 所以本扩展只做两件事，完全不碰 Zend VM：
 *   1) 写模板授权标记（让 TemplateBaseController 的检查通过）
 *   2) 拦截后门 URI
 *
 * 授权标记是什么：
 *   TemplateBaseController::__construct() 每个请求都检查
 *     Cache("8ue6hpsuffpheaaj")
 *   为空时 → header 跳百度 + include common.php → 授权校验失败 exit
 *   → 页面 HTTP 200 但 body 0 字节（这就是"首页没内容"的真正原因）。
 *   正常由 getAuthorizationCurl() 取到 ki 后写入；本机无授权服务，直接补。
 *
 * 文件格式（ThinkPHP File 缓存 + .think_serialize 前缀，expire=0 永不过期）：
 *   <?php\n//000000000000\nexit();?>\n.think_serialize:a:1:{s:1:"k";s:3:"web";}
 * 路径：{站点根}/data/runtime/cahce/08/d84096246ac19a1f12adfa064bb0fa.php
 *   （08 = md5("8ue6hpsuffpheaaj") 的前两位）
 */

#include "php.h"
#include "ext/standard/info.h"
#include "SAPI.h"
#include <sys/stat.h>
#include <sys/types.h>

#ifndef PHP_VERSION_ID
# error "PHP_VERSION_ID not defined"
#endif

#if PHP_VERSION_ID < 70400
# error "sukehappy 只支持 PHP >= 7.4"
#endif

#define SUKE_LOG_PATH "/tmp/sukehappy.log"

/* 授权标记缓存文件（相对站点根） */
#define SUKE_MARK_REL "/data/runtime/cahce/08/d84096246ac19a1f12adfa064bb0fa.php"

/* 后门 URI 黑名单 */
static const char *suke_bad_uris[] = {
    "i5nyyoblvmcpuj71", "o5tlkifzcrrgcvhx", NULL
};

/* 站点根：优先环境变量 SUKE_SITE_ROOT，否则用默认值 */
static char suke_root_buf[1024];
static char *suke_site_root = NULL;

static void suke_init_root(void)
{
    const char *env = getenv("SUKE_SITE_ROOT");

    if (env && *env) {
        snprintf(suke_root_buf, sizeof suke_root_buf, "%s", env);
    } else {
        snprintf(suke_root_buf, sizeof suke_root_buf,
                 "/www/wwwroot/103.217.193.191_9179");
    }
    /* 去掉末尾斜杠 */
    {
        size_t n = strlen(suke_root_buf);
        while (n > 1 && suke_root_buf[n - 1] == '/') {
            suke_root_buf[--n] = '\0';
        }
    }
    suke_site_root = suke_root_buf;
}

/* ========================================================================= */
/* 工具                                                                      */
/* ========================================================================= */

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

static int suke_uri_hit(const char *uri)
{
    int i;

    if (!uri || !*uri) return 0;
    for (i = 0; suke_bad_uris[i]; i++) {
        if (strstr(uri, suke_bad_uris[i])) return 1;
    }
    return 0;
}

/* 递归建目录（只处理站点内相对路径，段数很少） */
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

/* 写授权标记：存在且大小正常就不动 */
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
        if (sz >= 40) return;          /* 已存在且正常 */
    }

    /* 建目录 */
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


/* ========================================================================= */
/* 输出过滤：注入 CSS 隐藏模板授权遮罩/弹窗                                    */


PHP_MINIT_FUNCTION(sukehappy)
{
    suke_init_root();
    return SUCCESS;
}

PHP_RINIT_FUNCTION(sukehappy)
{
    /* 后门 URI 拦截 */
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

    /* 确保授权标记存在 */
    suke_write_auth_mark();

    /* 注意：不要再开 ob_start 偷 CSS —— 模板的授权遮罩/弹窗/跳转已经通过
     * 修改 themes 下的 common.js（去掉动态加载 search.js）彻底消除，
     * 不需要在输出层做任何替换，避免 ob 回调带来的 refcount/空输出问题。 */

    return SUCCESS;
}

PHP_MINFO_FUNCTION(sukehappy)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "sukehappy", "速科模板授权本地化（写授权标记）");
    php_info_print_table_row(2, "site_root", suke_site_root ? suke_site_root : "(未初始化)");
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
    "3.0.0",
    STANDARD_MODULE_PROPERTIES
};

#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(sukehappy)
