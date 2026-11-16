/*
 * sukehappy 探针：dump loader 编译出的 op_array 真实结构
 *
 * 目的：确认 Swoole Loader 产出的 op_array 是不是标准 zend_op_array，
 *       以及 function_name / opcodes / literals 的实际布局。
 * 只有知道布局，才谈得上「改 op_array」或「二进制 patch loader」。
 *
 * 编译：phpize && ./configure --with-php-config=... && make
 */

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_compile.h"
#include "zend_execute.h"

static FILE *g_log = NULL;

static void probe_log(const char *fmt, ...) {
    va_list args;
    if (!g_log) {
        g_log = fopen("/tmp/probe.log", "a");
        if (!g_log) return;
    }
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fflush(g_log);
}

static void probe_dump_op(const char *label, zend_function *f) {
    zend_op_array *oa;
    int i;

    if (!f) { probe_log("%s: NOT FOUND\n", label); return; }
    oa = &f->op_array;

    probe_log("=== %s ===\n", label);
    probe_log("  type            = %d (USER=%d INTERNAL=%d)\n",
              (int)f->type, ZEND_USER_FUNCTION, ZEND_INTERNAL_FUNCTION);
    probe_log("  function_name   = %s\n",
              f->common.function_name ? ZSTR_VAL(f->common.function_name) : "(null)");
    probe_log("  filename        = %s\n",
              oa->filename ? (const char *)oa->filename : "(null)");
    probe_log("  last            = %d\n", (int)oa->last);
    probe_log("  T (临时变量数)   = %d\n", (int)oa->T);
    probe_log("  last_var        = %d\n", (int)oa->last_var);
    probe_log("  last_literal    = %d\n", (int)oa->last_literal);
    probe_log("  opcodes         = %p\n", (void *)oa->opcodes);
    probe_log("  literals        = %p\n", (void *)oa->literals);
    probe_log("  doc_comment     = %p\n", (void *)oa->doc_comment);
    probe_log("  prototype       = %p\n", (void *)oa->prototype);
    probe_log("  scope           = %p\n", (void *)oa->scope);

    /* 前 12 条指令 */
    for (i = 0; i <= (int)oa->last && i < 12; i++) {
        zend_op *op = &oa->opcodes[i];
        probe_log("  op[%2d] opcode=%3d handler=%p op1_type=%d op1.num=%u op2_type=%d\n",
                  i, (int)op->opcode, op->handler,
                  (int)op->op1_type, (unsigned)op->op1.num, (int)op->op2_type);
    }
    /* literal 列表 */
    for (i = 0; i < oa->last_literal && i < 12; i++) {
        zval *z = &oa->literals[i];
        probe_log("  lit[%2d] type=%d ", i, (int)Z_TYPE_P(z));
        switch (Z_TYPE_P(z)) {
            case IS_STRING:
                probe_log("str='%s'\n", Z_STRVAL_P(z));
                break;
            case IS_LONG:
                probe_log("long=%ld\n", Z_LVAL_P(z));
                break;
            case IS_TRUE:
                probe_log("bool=true\n");
                break;
            case IS_FALSE:
                probe_log("bool=false\n");
                break;
            case IS_NULL:
                probe_log("null\n");
                break;
            default:
                probe_log("(other)\n");
        }
    }
    probe_log("\n");
}

static void probe_scan(const char *name) {
    zend_function *f;
    size_t nlen = strlen(name);

    /* EG（请求级）与 CG（编译级）两张表都查 */
    f = zend_hash_str_find_ptr(EG(function_table), name, nlen);
    probe_dump_op("EG", f);
    f = CG(function_table) ? zend_hash_str_find_ptr(CG(function_table), name, nlen) : NULL;
    probe_dump_op("CG", f);
}


/* loader 的 RETURN opcode 编号（从 dump 出来的 op_array 里反查：
 * op[2]/op[5] 的 opcode=60 且紧跟 RECV 之后 → 60 是 loader 的 RETURN） */
#define SUKE_LOADER_OP_RETURN 60

/* 把目标函数的函数体改成「返回 true」
 * 做法：把 RECV 之后的第一条指令改成 RETURN，并抄一条现成 RETURN 的 handler
 *       （opcode 的 handler 指针是 loader 自己那张表里的，不能瞎填）。
 *       literal 用现成的 IS_TRUE（索引由扫描得到）。 */
static void probe_patch_true(zend_function *f) {
    zend_op_array *oa;
    int i, lit = -1;
    zend_op *ret = NULL, *op;

    if (!f || f->type != ZEND_USER_FUNCTION) return;
    oa = &f->op_array;

    /* 1) 找 IS_TRUE literal */
    for (i = 0; i < oa->last_literal; i++) {
        if (Z_TYPE_P(&oa->literals[i]) == IS_TRUE) { lit = i; break; }
    }
    if (lit < 0) { probe_log("no IS_TRUE literal\n"); return; }

    /* 2) 找一条现成的 RETURN 指令，抄它的 handler */
    for (i = 0; i <= (int)oa->last; i++) {
        if (oa->opcodes[i].opcode == SUKE_LOADER_OP_RETURN) { ret = &oa->opcodes[i]; break; }
    }
    if (!ret) { probe_log("no RETURN opcode found\n"); return; }

    /* 3) 改写：跳过所有 RECV，改第一条真正执行的指令 */
    for (i = 0; i <= (int)oa->last; i++) {
        op = &oa->opcodes[i];
        if (op->opcode == 64 || op->opcode == 65 || op->opcode == 68) continue;  /* RECV 系列 */
        break;
    }
    if (i > (int)oa->last) { probe_log("no target opcode\n"); return; }

    probe_log("patching op[%d] opcode %d -> RETURN(lit=%d), ret_handler=%p\n",
              i, (int)oa->opcodes[i].opcode, lit, ret->handler);

    op->opcode        = SUKE_LOADER_OP_RETURN;
    op->handler       = ret->handler;
    op->op1_type      = IS_CONST;
    op->op1.constant  = (uint32_t)lit;
    op->op2_type      = IS_UNUSED;
    op->op2.var       = 0;
    op->result_type   = IS_UNUSED;
    op->result.var    = 0;
    op->extended_value = 0;
}

/* loader 编译完模板后必然调用 zend_compile_file。
 * 在它返回之后查函数表 —— 那时 templateAuthorization 已经注册好。
 * 注意：**只查表、只改 op_array，绝不碰 fh 的内容**
 * （碰了会破坏 loader 自己的解密流程）。 */
static zend_op_array *(*suke_prev_compile_file)(zend_file_handle *fh, int type) = NULL;

static zend_op_array *probe_compile_file(zend_file_handle *fh, int type) {
    zend_op_array *op_array;

    if (suke_prev_compile_file) {
        op_array = suke_prev_compile_file(fh, type);
    } else {
        op_array = zend_compile_file(fh, type);
    }

    {
        zend_function *tf = zend_hash_str_find_ptr(EG(function_table), "templateAuthorization", 20);
        if (tf && tf->type == ZEND_USER_FUNCTION) {
            static int done = 0;
            if (!done) {
                done = 1;
                probe_log(">>> compile_file hook: found templateAuthorization, patching\n");
                probe_patch_true(tf);
                probe_log(">>> patched\n");
            }
        }
    }
    return op_array;
}

static void probe_execute_ex(zend_execute_data *execute_data) {
    static int done = 0;
    if (!done && execute_data && execute_data->func
        && execute_data->func->type == ZEND_USER_FUNCTION
        && execute_data->func->common.function_name) {
        const char *fn = ZSTR_VAL(execute_data->func->common.function_name);
        /* 第一次进用户函数时 dump 授权相关函数 */
        if (strcasecmp(fn, "templateAuthorization") == 0
            || strcasecmp(fn, "getAuthorizationCurl") == 0) {
            done = 1;
            probe_log("### caught user fn: %s\n", fn);
            probe_scan("templateauthorization");
            probe_scan("templateAuthorization");
            probe_scan("getauthorizationcurl");
            probe_scan("verifysecuritycode");
        }
    }
    execute_ex(execute_data);
}

PHP_MINIT_FUNCTION(probe) {
    suke_prev_compile_file = zend_compile_file;
    zend_compile_file = probe_compile_file;
    return SUCCESS;
}

PHP_RINIT_FUNCTION(probe) {
    /* 不替换 zend_execute_ex —— loader 自己也 hook 了它，抢过来会破坏
     * loader 的执行入口（表现为输出乱码）。改用 compile_file 作为时机。 */
    return SUCCESS;
}

PHP_MINFO_FUNCTION(probe) {
    php_info_print_table_start();
    php_info_print_table_row(2, "probe", "loader op_array 结构探针");
    php_info_print_table_end();
}

zend_module_entry probe_module_entry = {
    STANDARD_MODULE_HEADER,
    "probe",
    NULL,
    PHP_MINIT(probe),
    NULL,
    PHP_RINIT(probe),
    NULL,
    PHP_MINFO(probe),
    "1.0",
    STANDARD_MODULE_PROPERTIES
};

#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
/* 无条件导出 get_module（config.m4 的 COMPILE_DL_PROBE 在某些构建下没定义） */
ZEND_GET_MODULE(probe)
