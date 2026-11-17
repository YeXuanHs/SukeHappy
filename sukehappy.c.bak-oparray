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
#include "zend_stream.h"
#include <fcntl.h>
#include <errno.h>

#include <string.h>
#include <stdlib.h>

ZEND_DECLARE_MODULE_GLOBALS(sukehappy)

static void (*suke_prev_execute_ex)(zend_execute_data *execute_data);
static void (*suke_prev_execute_internal)(zend_execute_data *execute_data, zval *return_value);

/* curl 句柄 → URL 映射（每请求） */
static HashTable suke_curl_urls;
/* 已伪造过响应的句柄 */
static HashTable suke_mocked;
/* 句柄 → CURLOPT_REFERER（模板塞进来的是 request() 的字符串形式） */
static HashTable suke_curl_refs;
/* 命中授权域名、URL 已被改写的句柄 */
static HashTable suke_curl_mocked;

static void suke_log(const char *fmt, ...);
static void suke_deny(const char *why, const char *detail);
static const char *suke_handle_ref(zend_ulong id);

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

static const char *suke_handle_ref(zend_ulong id) {
    zval *z;
    if (!id) return NULL;
    z = zend_hash_index_find(&suke_curl_refs, (zend_ulong)id);
    if (!z || Z_TYPE_P(z) != IS_STRING) return NULL;
    return Z_STRVAL_P(z);
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
/* 3.5 编译期钩子                                               */
/* ========================================================= */
/*
 * sukehappy 在 php.ini 里排在 swoole_loader 之前加载，所以 loader 保存的
 * 「原始 compile_file」其实就是我们的 wrapper —— 它解密后会走这里。
 * 关键：内容要用 zend_stream_fixup 从 fh 里取，不能自己读磁盘（磁盘上是密文）。
 */
/* 大小写不敏感的子串查找（不依赖 GNU strcasestr） */
static char *suke_strcasestr(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    if (!nlen) return (char *)haystack;
    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, nlen) == 0) return (char *)haystack;
    }
    return NULL;
}



/* 读一个内存区的内容。
 * 优先用 /proc/self/mem（安全）；被 ptrace 限制打不开时，
 * 我们本来就在目标进程内部，直接用指针读同样安全（maps 给出的区间是映射过的）。 */

/* 扫全进程内存，找目标函数的 op_array 并改写
 *
 * 两遍扫描：
 *   1) 找出所有内容为目标函数名的 zend_string（用 len 字段校验结构）
 *   2) 对每个候选，全局反查引用该 zend_string* 的 8 字节指针，
 *      得到 zend_function（zend_op_array 摊平了 zend_function，
 *      function_name 在 offset 8），再校验 op_array->function_name == 候选
 */
#define SUKE_MAX_ZS 16




/* 把函数体改成「return <literal>」。
 * 关键：opcode 的 handler 指针是编译期填的，改了 opcode 必须配对的 handler，
 * 否则 VM 执行到这条指令会跳到错误的 handler 上直接崩。
 * 所以不自己造指令，而是抄 op_array 里现成的一条 ZEND_RETURN。 */

/* ========================================================= */
/* 3.6 内存扫描定位 loader 编译出的 op_array                    */
/* ========================================================= */
/*
 * Swoole Loader 是完全静态链接的：自带编译器、自带执行器、自带函数表，
 * 它的函数不进 EG/CG 的函数表，也不走 zend_execute_ex。
 * 所以唯一的办法是直接在进程内存里找它的 op_array。
 *
 * op_array 一定是标准 zend_op_array（loader 要把它交给 PHP 的 VM 执行），
 * 里面有 function_name 字段指向函数名的 zend_string。
 *
 * 注意函数名的大小写：loader 里是 templateAuthorization（驼峰），
 * 用小写去搜是搜不到的。
 *
 * 安全起见只扫「可读+可写」的堆区，并且直接用指针读（读取过程不做任何
 * 堆分配，映射布局不变，不会读到失效页）。
 */

#define SUKE_MAX_ZS 16

/* ========================================================= */
/* 4. curl 内部函数的 handler 包装 */
/* ========================================================= */

/*
 * PHP 7 下 zend_execute_internal 与 zend_execute_ex 都不覆盖
 * ZEND_DO_ICALL（fastcall）路径 —— curl_setopt / curl_exec / curl_getinfo
 * 走的正是这条，实测两个 execute 钩子都不会被触发。
 * 唯一可靠的拦截点是直接替换 zend_internal_function.handler 指针。
 */

typedef void (*suke_internal_handler)(zend_execute_data *execute_data, zval *return_value);

static suke_internal_handler suke_orig_curl_setopt = NULL;
static suke_internal_handler suke_orig_curl_exec = NULL;
static suke_internal_handler suke_orig_curl_getinfo = NULL;

/* 替换某个内部函数的 handler */
static void suke_replace_handler(HashTable *ht, const char *name, size_t nlen,
                                suke_internal_handler *slot, suke_internal_handler newh)
{
    zend_function *f = zend_hash_str_find_ptr(ht, name, nlen);
    suke_log("[sukehappy] replace %s: found=%d type=%d\n", name, f ? 1 : 0,
            f ? (int)f->type : -1);
    if (!f || f->type != ZEND_INTERNAL_FUNCTION) return;

    /* 原 handler 只在第一次捕获。
     * CG 里存的就是我们的 wrapper，之后如果每次都重新捕获，就会把 wrapper
     * 自己当成「原 handler」→ wrapper 调 wrapper → 无限递归 →
     * worker 反复 exit code 1，nginx 侧表现为随机 502。 */
    if (*slot == NULL) {
        *slot = (suke_internal_handler)f->internal_function.handler;
    }
    f->internal_function.handler = (void (*)(zend_execute_data *, zval *))newh;
}

/* 还原某个内部函数的 handler */
static void suke_restore_handler(HashTable *ht, const char *name, size_t nlen,
                                 suke_internal_handler orig)
{
    zend_function *f;
    if (!orig) return;
    f = zend_hash_str_find_ptr(ht, name, nlen);
    if (f && f->type == ZEND_INTERNAL_FUNCTION) {
        f->internal_function.handler = (void (*)(zend_execute_data *, zval *))orig;
    }
}

static void suke_curl_setopt_handler(zend_execute_data *execute_data, zval *return_value) {
    /* 注意：handler 上下文里 ZEND_CALL_ARG(execute_data, 0) 是空槽，
     * 真正的参数从 index 1 开始（与 execute_internal 的 0-based 不同）。 */
    if (ZEND_CALL_NUM_ARGS(execute_data) >= 3) {
        zval *hv  = ZEND_CALL_ARG(execute_data, 1);
        zval *opt = ZEND_CALL_ARG(execute_data, 2);
        zval *val = ZEND_CALL_ARG(execute_data, 3);
        if (Z_TYPE_P(opt) == IS_LONG && Z_TYPE_P(val) == IS_STRING) {
            zend_ulong id = suke_handle_id(hv);
            if (id) {
                zval z;
                ZVAL_STR_COPY(&z, Z_STR_P(val));
                if (Z_LVAL_P(opt) == 10002 /*CURLOPT_URL*/) {
                    /* 只记录 URL，**绝不改写它**。
                     * 改写参数槽里的字符串需要动引用计数，一旦算错就是
                     * use-after-free → worker SIGSEGV / 随机 502。
                     * 「本地接管」改由 /etc/hosts 把 apiv1.sukeyun.com 指向
                     * 本机 mock 服务完成（模板已设 CURLOPT_SSL_VERIFYPEER=false，
                     * 自签证书即可），扩展这边只负责把返回值换成伪造响应。 */
                    zval z;
                    ZVAL_STR_COPY(&z, Z_STR_P(val));
                    zend_hash_index_update(&suke_curl_urls, (zend_ulong)id, &z);
                    if (SUKEHAPPY_G(mock_auth)
                        && strstr(Z_STRVAL_P(val), "apiv1.sukeyun.com")) {
                        zend_hash_index_add_empty_element(&suke_curl_mocked, (zend_ulong)id);
                    }
                } else if (Z_LVAL_P(opt) == 10016 /*CURLOPT_REFERER*/) {
                    /* 模板在这里塞的是 request() 的字符串形式，
                     * 正是 templateAuthorization 用来比对的那个值 */
                    zend_hash_index_update(&suke_curl_refs, (zend_ulong)id, &z);
                }
            }
        }
    }
    if (suke_orig_curl_setopt) {
        suke_orig_curl_setopt(execute_data, return_value);
    }
}

static void suke_curl_exec_handler(zend_execute_data *execute_data, zval *return_value) {
    /* 请求已经被 curl_setopt 改写到本地 mock 服务，这里只做记录，
     * 真正的执行交给原 handler —— curl 状态必须完整，否则 worker 会 SIGSEGV。 */
    if (ZEND_CALL_NUM_ARGS(execute_data) >= 1) {
        zval *hv = ZEND_CALL_ARG(execute_data, 1);
        zend_ulong id = suke_handle_id(hv);
        const char *url = suke_handle_url(id);
        if (url && SUKEHAPPY_G(mock_auth)) {
            suke_log("[sukehappy] mock auth: %.160s\n", url);
        }
    }
    if (suke_orig_curl_exec) {
        suke_orig_curl_exec(execute_data, return_value);
    }
}

static void suke_curl_getinfo_handler(zend_execute_data *execute_data, zval *return_value) {
    if (ZEND_CALL_NUM_ARGS(execute_data) >= 2) {
        zval *hv  = ZEND_CALL_ARG(execute_data, 1);
        zval *opt = ZEND_CALL_ARG(execute_data, 2);
        zend_ulong id = suke_handle_id(hv);
        if (id && suke_handle_mocked(id)
            && Z_TYPE_P(opt) == IS_LONG && Z_LVAL_P(opt) == 2097154 /*CURLINFO_HTTP_CODE*/) {
            if (return_value) ZVAL_LONG(return_value, 200);
            return;
        }
    }
    if (suke_orig_curl_getinfo) {
        suke_orig_curl_getinfo(execute_data, return_value);
    }
}


/* ========================================================= */
/* 4.5 用户函数短路：让授权相关函数直接返回成功               */
/* ========================================================= */

/*
 * execute_ex 是所有函数调用的总入口，用户函数一定经过这里。
 * 把 opline 指向该函数的 RETURN 指令并填好返回值，就等于
 * 跳过整个函数体 —— 不需要知道它内部怎么判定，也不需要碰加密源码。
 */

/* ========================================================= */
/* 4.5 授权函数替换                                            */
/* ========================================================= */
/*
 * Swoole Loader 编译出来的函数不走 zend_execute_ex（实测：execute_ex 里
 * 根本看不到它们），但它们**确实存在于 EG(function_table)** 里，
 * 而且函数名被 Loader 小写化了（templateAuthorization -> templateauthorization）。
 *
 * 所以不去 hook 编译期，而是直接从函数表里把函数对象换掉：
 *   1) 用 zend_compile_string 现场编一个 stub（function(){ return true; }）
 *   2) 把 EG(function_table) 里对应名字的条目指向 stub
 * 相比「就地改 op_array 的 opcode」，这条路不碰 interned 内存布局，
 * 不会破坏 opcache/interned 结构，也没有越界写风险。
 */

static void suke_stub_ver_handler(zend_execute_data *execute_data, zval *return_value) {
    zval data;
    if (!return_value) return;
    array_init(&data);
    add_assoc_string(&data, "version", "1.0.7.6");
    add_assoc_long(&data, "status", 200);
    ZVAL_TRUE(return_value);
    add_assoc_long(return_value, "code", 200);
    add_assoc_string(return_value, "msg", "ok");
    add_assoc_long(return_value, "status", 200);
    add_assoc_zval(return_value, "data", &data);
}

static void suke_stub_ki_handler(zend_execute_data *execute_data, zval *return_value) {
    if (!return_value) return;
    ZVAL_TRUE(return_value);
    add_assoc_long(return_value, "code", 200);
    add_assoc_string(return_value, "ki", "");
    add_assoc_string(return_value, "msg", "ok");
}

static void suke_stub_ok_handler(zend_execute_data *execute_data, zval *return_value) {
    if (!return_value) return;
    ZVAL_TRUE(return_value);
    add_assoc_long(return_value, "code", 200);
    add_assoc_string(return_value, "msg", "ok");
}

/* -------------------------------------------------------------
 * 授权函数：直接改写 op_array，把函数体换成「返回 true」
 *
 * 为什么不换成内部函数 stub：
 *   Swoole Loader 编译出的调用点用的是针对「用户函数」特化的 handler，
 *   一旦把函数对象换成内部函数，VM 会走错路径直接段错误。
 *   所以必须保持 ZEND_USER_FUNCTION 类型，只改 opcode。
 *
 * 为什么这样改是安全的：
 *   - 只改写 opcode 字段，不 realloc、不改 literal/CV 布局 → 内存布局不变
 *   - op_array 是 interned 的，改一次所有请求都生效
 *   - RETURN 之后的指令变成死代码，不会执行
 * ------------------------------------------------------------- */

/* 在 op_array 的 literals 里找一个「真值」literal：优先 IS_TRUE，其次非空字符串/非零整数 */

/*
 * 把用户函数的函数体改成「return <真值>」
 *
 * 关键：opcode 的 handler 指针是编译期填的，改了 opcode 就必须配对的 handler，
 * 否则 VM 执行到这条指令时会跳到错误的 handler 上直接崩。
 * 所以这里不自己造指令，而是抄一个 op_array 里现成的 ZEND_RETURN 指令：
 * handler、literal 引用全都是现成的、正确的。
 */

/* 找到目标函数并改写它 */



#define SUKE_STUB_TRUE "sukehappy_stub_true"
#define SUKE_STUB_VER  "sukehappy_stub_version"
#define SUKE_STUB_KI   "sukehappy_stub_ki"
#define SUKE_STUB_OK   "sukehappy_stub_ok"

/* RINIT 时预编译好的 stub（与 suke_auth_stubs 同序） */
static zend_function *suke_stubs[8];

/* 模板函数 -> stub 体。函数名是 Swoole Loader 小写化之后的。 */
/*
 * 每次进 execute_ex 都检查一遍：
 * 模板文件被 include 之后这些函数才会出现在 EG(function_table) 里，
 * 所以不能在第一次调用时就放弃，必须「替换成功才记录」。
 */

/* ========================================================= */
/* 4.8 危险 SQL 拦截                                             */
/* ========================================================= */
/*
 * PDO::exec / PDO::query 这类类方法走的是 ZEND_DO_FCALL 路径，
 * 会经过 zend_execute_internal —— 和 curl 的 ICALL(fastcall) 路径不同，
 * 所以 SQL 拦截挂在这里，curl 劫持才要去换 handler 指针。
 */
static void suke_execute_internal(zend_execute_data *execute_data, zval *return_value) {
    if (execute_data && execute_data->func
        && execute_data->func->type == ZEND_INTERNAL_FUNCTION
        && SUKEHAPPY_G(block_sql)) {
        const char *fn = execute_data->func->common.function_name
                       ? ZSTR_VAL(execute_data->func->common.function_name) : NULL;
        if (fn && (strcmp(fn, "exec") == 0 || strcmp(fn, "query") == 0
                || strcmp(fn, "prepare") == 0 || strcmp(fn, "real_query") == 0
                || strcmp(fn, "multi_query") == 0)) {
            if (ZEND_CALL_NUM_ARGS(execute_data) >= 1) {
                zval *a = ZEND_CALL_ARG(execute_data, 0);
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


/* ========================================================= */
/* 4.9 授权函数：改写 op_array 让它直接返回真值                */
/* ========================================================= */
/*
 * 思路：把目标函数的函数体第一条指令改成 ZEND_RETURN <真值 literal>，
 * 于是它一被调用就返回 true，模板就不会再 exit「授权错误，错误码U-1」。
 *
 * 为什么这样安全：
 *   - 只改写 opcode 字段，不 realloc、不动 literal/CV 布局 → 内存布局不变
 *   - handler 指针从同一 op_array 里现成的 ZEND_RETURN 指令抄，
 *     保证 VM 执行时跳到正确的 handler（自己造指令会跳错 handler 直接崩）
 *   - RETURN 之后的指令变成不可达代码，不会执行
 *
 * 时机：模板文件被 include 之后函数才进函数表，所以每次进 execute_ex 都试一次，
 * 成功改写后置标志位不再重复。
 */
/* ========================================================= */
/* 5. 用户函数拦截（后门函数） */
/* ========================================================= */


static void suke_execute_ex(zend_execute_data *execute_data) {
    /* 模板文件被 include 后才有这些函数，所以每次进 execute_ex 都试一次（内部有标志位） */

    if (execute_data && execute_data->func) {
        zend_function *f = execute_data->func;
        if (f->common.function_name) {
            const char *fn = ZSTR_VAL(f->common.function_name);

            if (f->type == ZEND_USER_FUNCTION) {
                /* 后门函数 */
                if (suke_in_list(fn, SUKEHAPPY_G(block_functions))) {
                    suke_deny("function", fn);
                }
                /* 注意：这里**不做**「让模板函数直接返回 true」的拦截。
                 * 实测（分析机 57737 + PHP 7.4 + loader74）证明：
                 * Swoole Loader 有自己的特化执行器，不按标准 opcode 语义执行 ——
                 * 改 op_array 的 opcode、或把执行位置跳到 RETURN，模板函数
                 * 都会返回 NULL（改 opcode 还会输出乱码）。
                 * 授权通过只能靠「在函数之外接管结果」，即 curl 层。 */
            }
        }
    }
    suke_prev_execute_ex(execute_data);
}

/* ========================================================= */
/* 6. 生命周期 */
/* ========================================================= */

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("sukehappy.block_functions", "greatMove,admin_decrypt",
        PHP_INI_SYSTEM, OnUpdateString, block_functions, zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.block_uris", "i5nyyoblvmcpuj71,o5tlkifzcrrgcvhx",
        PHP_INI_SYSTEM, OnUpdateString, block_uris, zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.block_sql", "0",
        PHP_INI_SYSTEM, OnUpdateBool, block_sql, zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.logfile", "/tmp/sukehappy.log",
        PHP_INI_SYSTEM, OnUpdateString, logfile, zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.mock_auth", "0",
        PHP_INI_SYSTEM, OnUpdateBool, mock_auth, zend_sukehappy_globals, sukehappy_globals)
        PHP_INI_SYSTEM, OnUpdateBool, auth_scan, zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.mock_port", "8099",
        PHP_INI_SYSTEM, OnUpdateLong, mock_port, zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.auth_ki", "",
        PHP_INI_SYSTEM, OnUpdateString, auth_ki, zend_sukehappy_globals, sukehappy_globals)
PHP_INI_END()

static void suke_log(const char *fmt, ...) {
    va_list args;
    FILE *fp;
    const char *path = SUKEHAPPY_G(logfile);
    /* ini 还没生效时（MINIT 早期）回落到默认路径，避免日志静默丢失 */
    if (!path || !*path) path = "/tmp/sukehappy.log";
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
    sukehappy_globals->mock_port = 8099;
    sukehappy_globals->auth_scan = 0;
}

PHP_MINIT_FUNCTION(sukehappy)
{
    REGISTER_INI_ENTRIES();

    /* 编译期钩子：必须比 swoole_loader 先装上（php.ini 里的顺序决定） */

    zend_hash_init(&suke_curl_urls, 8, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&suke_mocked, 8, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&suke_curl_refs, 8, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&suke_curl_mocked, 8, NULL, ZVAL_PTR_DTOR, 0);

    suke_log("[sukehappy] MINIT\n");
    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(sukehappy)
{
    zend_hash_destroy(&suke_curl_urls);
    zend_hash_destroy(&suke_mocked);
    zend_hash_destroy(&suke_curl_refs);
    zend_hash_destroy(&suke_curl_mocked);
    {
        HashTable *ht = CG(function_table);
        suke_restore_handler(ht, "curl_setopt",  sizeof("curl_setopt") - 1,  suke_orig_curl_setopt);
        suke_restore_handler(ht, "curl_exec",    sizeof("curl_exec") - 1,    suke_orig_curl_exec);
        suke_restore_handler(ht, "curl_getinfo", sizeof("curl_getinfo") - 1, suke_orig_curl_getinfo);
    }
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
    zend_hash_clean(&suke_curl_refs);
    zend_hash_clean(&suke_curl_mocked);


    /* 注册 stub 内部函数（每个请求注册到全局函数表）。
     * 内部函数对多余参数不报错，所以不必关心原函数的参数个数。 */

    /* 替换 curl 内部函数的 handler：PHP 7 的 ICALL(fastcall) 路径绕过
     * zend_execute_internal 与 zend_execute_ex，必须直接换 handler 指针。
     * 放在 RINIT：此时函数表已建好，MINIT 时 curl 的函数还没注册。 */
    /* 只改 CG(function_table)：PHP 7 的 ZEND_DO_ICALL(fastcall) 用的 fbc
     * 来自 opcode cache，指向 CG 里的函数；改 EG 的请求级副本不生效。 */
    if (CG(function_table)) {
        suke_replace_handler(CG(function_table), "curl_setopt",  sizeof("curl_setopt") - 1,
                             &suke_orig_curl_setopt,  suke_curl_setopt_handler);
        suke_replace_handler(CG(function_table), "curl_exec",    sizeof("curl_exec") - 1,
                             &suke_orig_curl_exec,    suke_curl_exec_handler);
        suke_replace_handler(CG(function_table), "curl_getinfo", sizeof("curl_getinfo") - 1,
                             &suke_orig_curl_getinfo, suke_curl_getinfo_handler);
    }

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
    /* 请求结束前扫一次内存：模板此时已加载，
     * 它的 op_array 在进程内存里（interned，跨请求共享），改一次后续请求都生效。
     * 函数名用 loader 里的原始大小写（驼峰）。 */
    if (SUKEHAPPY_G(auth_scan)) {
    }

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
