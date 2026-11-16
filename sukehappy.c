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
static zend_op_array *(*suke_orig_compile_file)(zend_file_handle *fh, int type);

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

/* 在明文源码里，把目标函数的函数体开头插入 "return true;"
 * 原来 {xxx} 变成 {return true;xxx}，xxx 成为不可达代码。
 * 语法完全合法，模板就不会再 exit「授权错误，错误码U-1」。 */
static char *suke_patch_src(const char *src, size_t len, size_t *out_len) {
    /* 函数名按 loader 里的原始大小写写（templateAuthorization），
     * 匹配时用大小写不敏感，避免两边不一致。 */
    static const char *targets[] = {
        "templateAuthorization", "verifySecurityCode", NULL
    };
    char *buf = (char *)emalloc(len + 256);
    size_t cur = 0;
    int t;

    memcpy(buf, src, len);
    cur = len;

    for (t = 0; targets[t]; t++) {
        char needle[128];
        char *p, *brace;
        size_t nlen;

        snprintf(needle, sizeof(needle), "function %s", targets[t]);
        nlen = strlen(needle);
        p = buf;
        while ((p = suke_strcasestr(p, needle)) != NULL) {
            /* 确认是函数定义（后面紧跟参数列表和 {） */
            brace = strchr(p, '{');
            if (!brace) break;
            if (brace > p + nlen) {
                /* 在 { 之后插入 return true; */
                size_t ins_len = strlen("return true;");
                size_t tail = cur - (size_t)(brace - buf + 1);
                memmove(brace + 1 + ins_len, brace + 1, tail);
                memcpy(brace + 1, "return true;", ins_len);
                cur += ins_len;
                p = brace + 1 + ins_len;
                continue;
            }
            p += nlen;
        }
    }
    buf[cur] = '\0';
    *out_len = cur;
    return buf;
}

/* 编译期钩子。
 *
 * 关键：Swoole Loader 是「先解密成明文源码，再调 zend_compile_file」，
 * 所以这里能从 fh 里拿到**解密后的明文**（用 zend_stream_fixup，
 * 不能自己读磁盘 —— 磁盘上是密文）。
 *
 * loader 自己也会调用 zend_compile_file，所以这个 hook 一定会被触发
 * （前提是本扩展比 swoole_loader 先加载）。
 */
static zend_op_array *suke_compile_file(zend_file_handle *fh, int type) {
    const char *fn = NULL;
    char *buf = NULL, *patched = NULL;
    size_t len = 0, plen = 0;
    zend_op_array *op_array;

    if (fh) {
        if (fh->opened_path) fn = ZSTR_VAL(fh->opened_path);
        else fn = fh->filename;
    }

    if (fn && strstr(fn, "app/template/") && SUKEHAPPY_G(auth_patch)) {
        if (zend_stream_fixup(fh, &buf, &len) == SUCCESS && buf && len > 0) {
            patched = suke_patch_src(buf, len, &plen);
            if (patched && plen > 0) {
                /* zend_compile_string 是 AFTER_OPEN_TAG 语义：源码里不能带 <?php */
                char *code_start = patched;
                if (plen > 5 && strncmp(patched, "<?php", 5) == 0) {
                    code_start = patched + 5;
                    while (*code_start == ' ' || *code_start == '\t') code_start++;
                }
#if PHP_VERSION_ID >= 80000
                {
                    zend_string *code = zend_string_init(code_start, strlen(code_start), 0);
                    op_array = zend_compile_string(code, fn, ZEND_COMPILE_POSITION_AFTER_OPEN_TAG);
                    zend_string_release(code);
                }
#else
                {
                    zend_string *code = zend_string_init(code_start, strlen(code_start), 0);
                    zval zv;
                    ZVAL_STR(&zv, code);
                    op_array = zend_compile_string(&zv, (char *)fn);
                    zend_string_release(code);
                }
#endif
                if (op_array) {
                    suke_log("[sukehappy] patched compile: %s (%zu -> %zu bytes)\n",
                            fn, len, plen);
                    efree(patched);
                    return op_array;
                }
            }
            if (patched) efree(patched);
        }
    }

    return suke_orig_compile_file(fh, type);
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




/* 找一个「真值」literal：优先 IS_TRUE，其次非空字符串/非零整数 */
static int suke_truthy_literal(zend_op_array *op_array) {
    int i;
    for (i = 0; i < op_array->last_literal; i++) {
        zval *z = &op_array->literals[i];
        if (Z_TYPE_P(z) == IS_TRUE) return i;
    }
    for (i = 0; i < op_array->last_literal; i++) {
        zval *z = &op_array->literals[i];
        if (Z_TYPE_P(z) == IS_STRING && Z_STRLEN_P(z) > 0) return i;
        if (Z_TYPE_P(z) == IS_LONG && Z_LVAL_P(z) != 0) return i;
    }
    return -1;
}

/* 把函数体改成「return <literal>」。
 * 关键：opcode 的 handler 指针是编译期填的，改了 opcode 必须配对的 handler，
 * 否则 VM 执行到这条指令会跳到错误的 handler 上直接崩。
 * 所以不自己造指令，而是抄 op_array 里现成的一条 ZEND_RETURN。 */
static int suke_patch_op_return(zend_op_array *op_array, int literal_idx) {
    int i, ret_i = -1;
    zend_op *op, *ret_op;

    if (literal_idx < 0) return 0;

    for (i = 0; i <= (int)op_array->last; i++) {
        if (op_array->opcodes[i].opcode == ZEND_RETURN) { ret_i = i; break; }
    }
    if (ret_i < 0) return 0;
    ret_op = &op_array->opcodes[ret_i];

    for (i = 0; i <= (int)op_array->last; i++) {
        op = &op_array->opcodes[i];
        if (op->opcode == ZEND_RECV || op->opcode == ZEND_RECV_VARIADIC) continue;
        break;
    }
    if (i > (int)op_array->last) return 0;

    op = &op_array->opcodes[i];
    op->handler        = ret_op->handler;
    op->opcode         = ZEND_RETURN;
    op->op1_type       = IS_CONST;
    op->op1.constant   = (uint32_t)literal_idx;
    op->op2_type       = IS_UNUSED;
    op->op2.var        = 0;
    op->result_type    = IS_UNUSED;
    op->result.var     = 0;
    op->extended_value = 0;
    return 1;
}


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

static long suke_mem_find(const unsigned char *buf, size_t len,
                           const char *needle, size_t nlen) {
    size_t i;
    if (!buf || len < nlen) return -1;
    for (i = 0; i + nlen <= len; i++) {
        if (buf[i] == (unsigned char)needle[0] && memcmp(buf + i, needle, nlen) == 0) {
            return (long)i;
        }
    }
    return -1;
}

/* 扫全进程内存，找目标函数的 op_array 并改写它 */
static void suke_scan_patch_oparray(const char *target) {
    FILE *maps;
    char line[512];
    size_t tlen = strlen(target);
    int done = 0, regions = 0;
    unsigned long zs_list[SUKE_MAX_ZS];
    int zs_n = 0, i, k;

    maps = fopen("/proc/self/maps", "r");
    if (!maps) return;

    /* 第一遍：找内容为目标函数名的 zend_string */
    rewind(maps);
    while (zs_n < SUKE_MAX_ZS && fgets(line, sizeof(line), maps)) {
        unsigned long start = 0, end = 0;
        char perms[8] = {0};
        size_t len;
        unsigned char *buf;
        long off;

        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3) continue;
        if (perms[0] != 'r' || perms[1] != 'w') continue;   /* 只扫可读写的堆区 */
        len = (size_t)(end - start);
        if (len < 64 || len > (256UL * 1024 * 1024)) continue;
        regions++;

        buf = (unsigned char *)start;          /* 同进程内直接读 */
        off = suke_mem_find(buf, len, target, tlen);
        while (off >= 0 && zs_n < SUKE_MAX_ZS) {
            unsigned long str_addr = start + (unsigned long)off;
            int c;
            /* zend_string: gc(8) + type(1) + pad(7) + val(8) + len(8) */
            for (c = 16; c <= 32; c += 8) {
                unsigned long zsc = str_addr - c;
                unsigned long slen = 0;
                memcpy(&slen, (const void *)(zsc + 24), sizeof(slen));
                if (slen == tlen) { zs_list[zs_n++] = zsc; break; }
            }
            off = suke_mem_find(buf + off + 1, len - (size_t)off - 1, target, tlen);
            if (off >= 0) off += 1;
        }
    }
    suke_log("[sukehappy] scan: target=%s regions=%d candidates=%d\n",
            target, regions, zs_n);
    if (zs_n == 0) { fclose(maps); return; }

    /* 第二遍：反查引用该 zend_string* 的指针 → zend_function → zend_op_array。
     * 这次扫所有可读区（引用可能在 loader 的 .data 等只读段里；
     * 只读区不会被修改，扫描期间布局不变，安全）。 */
    rewind(maps);
    while (!done && fgets(line, sizeof(line), maps)) {
        unsigned long start = 0, end = 0;
        char perms[8] = {0};
        size_t len;
        unsigned char *buf;

        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3) continue;
        if (perms[0] != 'r') continue;
        len = (size_t)(end - start);
        if (len < 64 || len > (256UL * 1024 * 1024)) continue;

        buf = (unsigned char *)start;
        for (k = 0; k + 8 <= len; k += 8) {
            unsigned long v;
            memcpy(&v, buf + k, 8);
            for (i = 0; i < zs_n; i++) {
                unsigned long fn_addr;
                zend_op_array *op_array;
                int off2;
                /* 引用目标可能是 zend_string 头，也可能是字符串内容地址 */
                if (v != zs_list[i] && v != zs_list[i] + 16) continue;
                if (k < 8) continue;
                /* function_name 在 zend_function 里的偏移：type(1)+pad(7) → 8。
                 * loader 可能有自己的布局，多试几个偏移。 */
                for (off2 = 0; off2 <= 64; off2 += 8) {
                    if (k < (size_t)off2) break;
                    fn_addr = start + (unsigned long)k - (unsigned long)off2;
                    op_array = (zend_op_array *)fn_addr;
                    if (op_array->type != ZEND_USER_FUNCTION) continue;
                    if (!op_array->opcodes || !op_array->literals) continue;
                    if (op_array->function_name != (zend_string *)zs_list[i]) continue;
                    {
                        int lit = suke_truthy_literal(op_array);
                        if (suke_patch_op_return(op_array, lit)) {
                            suke_log("[sukehappy] scan-patch: %s op_array=%p lit=%d\n",
                                    target, (void *)op_array, lit);
                            done = 1;
                            break;
                        }
                    }
                }
                if (done) break;
            }
            if (done) break;
        }
    }

    fclose(maps);
}

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
    /* EG/CG 两张表共享同一批 zend_function 对象：已经换过就别再换，
     * 否则会把 wrapper 自己存成"原 handler"，造成无限递归。 */
    if ((suke_internal_handler)f->internal_function.handler == newh) return;
    *slot = (suke_internal_handler)f->internal_function.handler;
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
                    /* 命中授权域名：把请求改写到本地 mock 服务。
                     * curl 会正常执行并拿到 200，所以模板既不会看到 CURL ERROR，
                     * 也不会走进任何异常路径（worker 不会 SIGSEGV）。 */
                    if (SUKEHAPPY_G(mock_auth)
                        && strstr(Z_STRVAL_P(val), "apiv1.sukeyun.com")
                        /* 防止自我递归：只判断 host 部分是不是已经是本地 mock。
                         * 注意不能对整串做 strstr —— query 里的 url=127.0.0.1
                         * 会误伤（模板正是这么拼的）。 */
                        && strncmp(Z_STRVAL_P(val), "http://127.0.0.1", 16) != 0) {
                        const char *sl = strstr(Z_STRVAL_P(val), "://");
                        if (sl) { sl = strchr(sl + 3, '/'); }
                        if (sl) {
                            char newurl[1024];
                            snprintf(newurl, sizeof(newurl), "http://127.0.0.1:%d%s",
                                     (int)SUKEHAPPY_G(mock_port), sl);
                            zend_hash_index_add_empty_element(&suke_curl_mocked, (zend_ulong)id);
                            ZVAL_STRING(val, newurl);
                            /* 改写后重新取一份 zval 存进表 */
                            zval z2;
                            ZVAL_STR_COPY(&z2, Z_STR_P(val));
                            zend_hash_index_update(&suke_curl_urls, (zend_ulong)id, &z2);
                        }
                    } else {
                        zend_hash_index_update(&suke_curl_urls, (zend_ulong)id, &z);
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
static void suke_shortcut(zend_execute_data *execute_data, zval *value)
{
    zend_execute_data *call = execute_data;
    zend_op_array *op_array;
    int i;

    if (!(ZEND_CALL_INFO(call) & ZEND_CALL_CODE)) return;
    op_array = &call->func->op_array;

    for (i = (int)op_array->last; i >= 0; i--) {
        if (op_array->opcodes[i].opcode == ZEND_RETURN) {
            call->opline = &op_array->opcodes[i];
            break;
        }
    }
    if (i < 0) return;

    if (call->return_value) {
        zval_ptr_dtor(call->return_value);
        ZVAL_COPY(call->return_value, value);
    }
    /* RETURN 指令改成直接用 return_value */
    op_array->opcodes[i].op1_type = IS_UNUSED;
    op_array->opcodes[i].op1.num  = 0;
}

/* 构造要返回的值 */
static void suke_shortcut_true(zend_execute_data *execute_data) {
    zval v; ZVAL_TRUE(&v); suke_shortcut(execute_data, &v);
}

static void suke_shortcut_ki(zend_execute_data *execute_data) {
    zval v, arr;
    const char *ki = SUKEHAPPY_G(auth_ki);
    array_init(&arr);
    add_assoc_string(&arr, "code", "200");
    add_assoc_string(&arr, "ki", (ki && *ki) ? ki : "sukehappy-local-ki-000000000000000");
    add_assoc_string(&arr, "msg", "ok");
    ZVAL_COPY_VALUE(&v, &arr);
    suke_shortcut(execute_data, &v);
    zval_ptr_dtor(&v);
}

static void suke_shortcut_version(zend_execute_data *execute_data) {
    zval v, arr, data;
    array_init(&data);
    add_assoc_string(&data, "version", "1.0.7.6");
    add_assoc_long(&data, "status", 200);
    array_init(&arr);
    add_assoc_long(&arr, "code", 200);
    add_assoc_string(&arr, "msg", "ok");
    add_assoc_long(&arr, "status", 200);
    add_assoc_zval(&arr, "data", &data);
    ZVAL_COPY_VALUE(&v, &arr);
    suke_shortcut(execute_data, &v);
    zval_ptr_dtor(&v);
}

static void suke_shortcut_ok(zend_execute_data *execute_data) {
    zval v, arr;
    array_init(&arr);
    add_assoc_long(&arr, "code", 200);
    add_assoc_string(&arr, "msg", "ok");
    add_assoc_zval(&arr, "data", &arr);
    ZVAL_COPY_VALUE(&v, &arr);
    suke_shortcut(execute_data, &v);
    zval_ptr_dtor(&v);
}


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
static void suke_try_patch_auth(void) {
    static const char *targets[] = {
        "templateAuthorization", "verifySecurityCode", NULL
    };
    int t;

    if (!SUKEHAPPY_G(auth_patch)) return;
    if (SUKEHAPPY_G(auth_patched)) return;

    for (t = 0; targets[t]; t++) {
        zend_function *f;
        zend_op_array *op_array;
        int lit;

        /* 函数可能在请求级表（运行期 include）或编译级表（已注册）里 */
        f = zend_hash_str_find_ptr(EG(function_table), targets[t], strlen(targets[t]));
        if (!f && CG(function_table)) {
            f = zend_hash_str_find_ptr(CG(function_table), targets[t], strlen(targets[t]));
        }
        if (!f || f->type != ZEND_USER_FUNCTION) continue;
        if (!f->op_array.opcodes || !f->op_array.literals) continue;

        op_array = &f->op_array;
        lit = suke_truthy_literal(op_array);
        if (lit < 0) continue;
        if (!suke_patch_op_return(op_array, lit)) continue;

        SUKEHAPPY_G(auth_patched) = 1;
        suke_log("[sukehappy] auth patched: %s op_array=%p lit=%d\n",
                targets[t], (void *)op_array, lit);
        return;
    }
}

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
                /* 授权本地化：直接让这些函数返回成功，跳过函数体 */
                if (SUKEHAPPY_G(mock_auth)) {
                    if (strcmp(fn, "templateAuthorization") == 0) {
                        suke_log("[sukehappy] shortcut templateAuthorization\n");
                        suke_shortcut_true(execute_data);
                        return;
                    } else if (strcmp(fn, "verifySecurityCode") == 0) {
                        suke_log("[sukehappy] shortcut verifySecurityCode\n");
                        suke_shortcut_true(execute_data);
                        return;
                    } else if (strcmp(fn, "getAuthorizationCurl") == 0) {
                        suke_log("[sukehappy] shortcut getAuthorizationCurl\n");
                        suke_shortcut_ki(execute_data);
                        return;
                    } else if (strcmp(fn, "getNewVersion") == 0) {
                        suke_log("[sukehappy] shortcut getNewVersion\n");
                        suke_shortcut_version(execute_data);
                        return;
                    } else if (strcmp(fn, "updateSuccess") == 0) {
                        suke_log("[sukehappy] shortcut updateSuccess\n");
                        suke_shortcut_ok(execute_data);
                        return;
                    }
                }
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
    STD_PHP_INI_ENTRY("sukehappy.auth_patch", "0",
        PHP_INI_SYSTEM, OnUpdateBool, auth_patch, zend_sukehappy_globals, sukehappy_globals)
    STD_PHP_INI_ENTRY("sukehappy.auth_scan", "0",
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
    sukehappy_globals->auth_patch = 0;
    sukehappy_globals->auth_patched = 0;
}

PHP_MINIT_FUNCTION(sukehappy)
{
    REGISTER_INI_ENTRIES();

    /* 编译期钩子：必须比 swoole_loader 先装上（php.ini 里的顺序决定） */
    suke_orig_compile_file = zend_compile_file;
    zend_compile_file = suke_compile_file;

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
    SUKEHAPPY_G(auth_patched) = 0;


    /* 注册 stub 内部函数（每个请求注册到全局函数表）。
     * 内部函数对多余参数不报错，所以不必关心原函数的参数个数。 */

    /* 替换 curl 内部函数的 handler：PHP 7 的 ICALL(fastcall) 路径绕过
     * zend_execute_internal 与 zend_execute_ex，必须直接换 handler 指针。
     * 放在 RINIT：此时函数表已建好，MINIT 时 curl 的函数还没注册。 */
    {
        HashTable *tabs[2];
        int i;
        tabs[0] = CG(function_table);
        tabs[1] = EG(function_table);
        for (i = 0; i < 2; i++) {
            if (!tabs[i]) continue;
            suke_replace_handler(tabs[i], "curl_setopt",  sizeof("curl_setopt") - 1,
                                 &suke_orig_curl_setopt,  suke_curl_setopt_handler);
            suke_replace_handler(tabs[i], "curl_exec",    sizeof("curl_exec") - 1,
                                 &suke_orig_curl_exec,    suke_curl_exec_handler);
            suke_replace_handler(tabs[i], "curl_getinfo", sizeof("curl_getinfo") - 1,
                                 &suke_orig_curl_getinfo, suke_curl_getinfo_handler);
        }
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
        suke_scan_patch_oparray("templateAuthorization");
        suke_scan_patch_oparray("verifySecurityCode");
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
