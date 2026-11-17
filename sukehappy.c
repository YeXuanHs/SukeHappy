/*
 * sukehappy —— 速科模板授权本地化
 *
 * 支持：PHP 7.4
 * 原理：把模板的授权校验函数（templateauthorization / verifysecuritycode）
 *       的 zend_op_array 第 0 条指令改写成 `ZEND_RETURN true`，
 *       函数一被调用就直接返回 true，模板的授权判断通过。
 *       不需要网络、不需要改 URL、不需要 mock 服务。
 *
 * 为什么只支持 7.4：7.4 的 execute_ex 内部不回调 zend_execute_ex 指针，
 * 所以 hook 它不会递归。7.3 及更早的 execute_ex 会绕回全局指针，
 * hook 它必然无限递归（实测 gdb 栈里 execute_ex 叠 5 层）。
 *
 * ============================================================================
 * 四个致命坑（改这段代码前务必读完）
 * ============================================================================
 *  坑1：函数名长度必须用 strlen()。
 *       "templateauthorization" 是 **21** 个字符。硬编码成 20 时
 *       zend_hash_str_find_ptr 按长度匹配会永远查不到，
 *       表现为「拿不到 op_array」的假象。
 *
 *  坑2：ZEND_RETURN 是 **62**，不是 60（60 是 ZEND_DO_FCALL）。
 *       抄错 handler 会造出畸形指令，表现为返回 NULL 或整页输出乱码。
 *
 *  坑3：op1.constant 不是 literal 索引，是「相对当前 opline 的字节偏移」。
 *       PHP 7.x 的 ZEND_USE_ABS_CONST_ADDR 为 0，RT_CONSTANT 定义是
 *         (zval*)((char*)opline + (int32_t)node.constant)
 *       正常由 pass_two 的 ZEND_PASS_TWO_UPDATE_CONSTANT 完成转换，
 *       手工改指令就得自己算：
 *         node.constant = (char*)&literals[lit] - (char*)opline
 *       写成索引会被当成指针偏移 → 读到垃圾 → 函数返回 NULL。
 *
 *  坑4：handler 是「特化」的，与 op1_type 绑定。用
 *       zend_vm_set_opcode_handler() **函数**自动算。
 *       不要用 ZEND_VM_SET_OPCODE_HANDLER 宏 —— 它在 zend_vm_execute.h 里，
 *       那个文件太大不能 include，直接写会 undefined symbol；
 *       也不要从别的函数借 handler（特化变体不匹配会返回 NULL，
 *       而且遍历函数表会打断 loader 的解密流程）。
 *
 * ============================================================================
 * 模块结构的两个坑（会让 loader 解密失效）
 * ============================================================================
 *  - 不要用 ZEND_MODULE_GLOBALS + PHP_GINIT + STANDARD_MODULE_PROPERTIES_EX
 *    （PHP 7.4 上 globals 指针错乱，编译报 "xxx_globals is a pointer"）。
 *    全部用 static 变量。
 *  - MINIT 保持为空，不要 REGISTER_INI_ENTRIES()。
 *  - 不要无条件 hook zend_execute_internal —— loader 解密时会调用内部函数，
 *    截住它会打断解密流程（症状：common.php 报 Parse Error）。
 */

#include "php.h"
#include "ext/standard/info.h"
#include "zend_compile.h"
#include "zend_execute.h"
#include "zend_vm.h"
#include "SAPI.h"

#ifndef PHP_VERSION_ID
# error "PHP_VERSION_ID not defined"
#endif

#if PHP_VERSION_ID < 70400
# error "sukehappy 只支持 PHP >= 7.4（7.3 及更早的 execute_ex 会递归，见文件头说明）"
#endif

#define SUKE_LOG_PATH "/tmp/sukehappy.log"

/* 后门 URI 黑名单（nginx 层也会拦，这里是第二道防线） */
static const char *suke_bad_uris[] = {
    "i5nyyoblvmcpuj71", "o5tlkifzrcrgcvhx", NULL
};

/* ========================================================================= */
/* 工具                                                                      */
/* ========================================================================= */

static void suke_log(const char *fmt, ...) {
    va_list args;
    FILE *fp = fopen(SUKE_LOG_PATH, "a");
    if (!fp) return;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);
    fclose(fp);
}

static int suke_uri_hit(const char *uri) {
    int i;
    if (!uri || !*uri) return 0;
    for (i = 0; suke_bad_uris[i]; i++) {
        if (strstr(uri, suke_bad_uris[i])) return 1;
    }
    return 0;
}

static void suke_deny(const char *why, const char *detail) {
    suke_log("[sukehappy] deny %s: %s\n", why, detail ? detail : "");
    if (SG(headers_sent) == 0) {
        sapi_header_line h;
        h.line = "HTTP/1.1 403 Forbidden";
        h.line_len = (uint32_t)strlen(h.line);
        h.response_code = 403;
        sapi_header_op(SAPI_HEADER_REPLACE, &h);
    }
    zend_bailout();
}

/* ========================================================================= */
/* op_array 改写                                                             */
/* ========================================================================= */

#define SUKE_OP_RETURN 62   /* ZEND_RETURN（坑2） */

static int suke_patch_one(zend_function *f) {
    zend_op_array *oa;
    int i, lit = -1;

    if (!f || f->type != ZEND_USER_FUNCTION) return 0;
    oa = &f->op_array;
    if (!oa->opcodes || !oa->literals || oa->last < 0) return 0;

    /* 幂等：已经改过就直接返回 */
    if (oa->opcodes[0].opcode == SUKE_OP_RETURN && oa->opcodes[0].op1_type == IS_CONST) {
        return 1;
    }

    /* 找一个现成的 IS_TRUE literal */
    for (i = 0; i < oa->last_literal; i++) {
        if (Z_TYPE_P(&oa->literals[i]) == IS_TRUE) { lit = i; break; }
    }
    if (lit < 0) return 0;

    /* 改第 0 条指令 —— 不猜哪条是「函数体第一条」，避免 RECV 判断出错 */
    oa->opcodes[0].opcode         = SUKE_OP_RETURN;
    oa->opcodes[0].op1_type       = IS_CONST;
    oa->opcodes[0].op2_type       = IS_UNUSED;
    oa->opcodes[0].op2.var        = 0;
    oa->opcodes[0].result_type    = IS_UNUSED;
    oa->opcodes[0].result.var     = 0;
    oa->opcodes[0].extended_value = 0;
#if ZEND_USE_ABS_CONST_ADDR
    oa->opcodes[0].op1.zv         = &oa->literals[lit];
#else
    oa->opcodes[0].op1.constant =
        (uint32_t)((char *)&oa->literals[lit] - (char *)&oa->opcodes[0]);   /* 坑3 */
#endif
    /* 坑4：用函数自动算特化 handler */
    zend_vm_set_opcode_handler(&oa->opcodes[0]);
    return 1;
}

static void suke_patch_auth(void) {
    static int done = 0;
    static const char *names[] = { "templateauthorization", "verifysecuritycode", NULL };
    int n, ok = 0, total = 0;

    if (done) return;

    for (n = 0; names[n]; n++) {
        size_t nlen = strlen(names[n]);            /* 坑1 */
        zend_function *f, *g = NULL;

        total++;
        f = zend_hash_str_find_ptr(EG(function_table), names[n], nlen);
        if (f && suke_patch_one(f)) ok++;
        /* EG 和 CG 各有一份 zend_function，两份都要改 */
        if (CG(function_table)) {
            g = zend_hash_str_find_ptr(CG(function_table), names[n], nlen);
            if (g && g != f && suke_patch_one(g)) ok++;
        }
    }

    if (ok >= total) {
        done = 1;
        suke_log("[sukehappy] auth op_array patched (%d)\n", ok);
    }
}

/* ========================================================================= */
/* 执行钩子                                                                  */
/* ========================================================================= */

static void suke_execute_ex(zend_execute_data *execute_data) {
    /* 模板函数 require 之后才进函数表，所以每次进 execute_ex 都试一次 */
    suke_patch_auth();
    /* 调原始 execute_ex：7.4 的实现不回调全局指针，安全 */
    execute_ex(execute_data);
}

/* ========================================================================= */
/* 模块入口                                                                  */
/* ========================================================================= */

PHP_MINIT_FUNCTION(sukehappy) { return SUCCESS; }   /* 保持为空 */

PHP_RINIT_FUNCTION(sukehappy) {
#if defined(COMPILE_DL_SUKEHAPPY) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    /* 拦后门 URI（nginx 层也会拦） */
    if (suke_uri_hit(SG(request_info).request_uri)) {
        suke_deny("uri", SG(request_info).request_uri);
    }

    zend_execute_ex = suke_execute_ex;
    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(sukehappy) {
    /* 不还原：suke_execute_ex 是幂等的（每请求 RINIT 重设一次即可），
     * 且还原会与 loader/ioncube 的 wrapper 冲突。 */
    return SUCCESS;
}

PHP_MINFO_FUNCTION(sukehappy) {
    php_info_print_table_start();
    php_info_print_table_row(2, "sukehappy", "速科模板授权本地化（op_array）");
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
    PHP_RSHUTDOWN(sukehappy),
    PHP_MINFO(sukehappy),
    "2.0.0",
    STANDARD_MODULE_PROPERTIES
};

/* 无条件导出 get_module：config.m4 的 COMPILE_DL_SUKEHAPPY 在某些
 * 构建下不会定义，用 #ifdef 包起来会导致 .so 缺少 get_module 符号，
 * PHP 加载时报 "Invalid library (maybe not a PHP library)"。 */
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(sukehappy)
