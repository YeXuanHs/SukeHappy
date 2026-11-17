/*
 * sukehappy —— 速科模板授权本地化（op_array 方案，验证通过版）
 *
 * 做法：把模板授权函数的 zend_op_array 第 0 条指令改写成 `ZEND_RETURN true`，
 *       它一被调用就直接返回 true，模板的授权校验通过。
 *       不需要网络 / 改 URL / mock 服务。
 *
 * 四个致命坑（改代码前必读）：
 *  1. 函数名长度必须 strlen("templateauthorization") == 21，硬编码 20 永远查不到。
 *  2. ZEND_RETURN 是 62，不是 60（60 是 ZEND_DO_FCALL）。
 *  3. op1.constant 是「相对 opline 的字节偏移」，不是 literal 索引：
 *       node.constant = (char*)&literals[lit] - (char*)opline
 *     用官方宏 ZEND_VM_SET_OPCODE_HANDLER 设 handler，别去借。
 *  4. 不要遍历整个函数表（会打断 loader 解密 → common.php Parse error）。
 *
 * 本文件以验证通过的探针版为骨架（MINIT 空 / RINIT 只装 execute_ex /
 * 无 module globals），改动请保持这个结构。
 */

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_compile.h"
#include "zend_execute.h"
#include "zend_vm.h"

static FILE *g_log = NULL;
static void plog(const char *fmt, ...) {
    va_list a;
    if (!g_log) { g_log = fopen("/tmp/sukehappy.log", "a"); if (!g_log) return; }
    va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a); fflush(g_log);
}

#define ZEND_RETURN_OPC 62

/* 不借 handler：用 PHP 官方宏 ZEND_VM_SET_OPCODE_HANDLER 自动算。
 * （借 handler 需要锚点函数，且借错特化变体会导致返回值错误；
 *   遍历函数表找还会打断 loader 解密。） */

/* 把函数的函数体改成 return true */
static int patch_fn(zend_function *f, const char *tag) {
    zend_op_array *oa;
    int i, lit = -1;

    if (!f || f->type != ZEND_USER_FUNCTION) return 0;
    oa = &f->op_array;
    if (!oa->opcodes || !oa->literals) return 0;

    for (i = 0; i < oa->last_literal; i++) {
        if (Z_TYPE_P(&oa->literals[i]) == IS_TRUE) { lit = i; break; }
    }
    if (lit < 0) { plog("[patch:%s] no IS_TRUE literal\n", tag); return 0; }

    /* 直接改第 0 条 —— 不猜哪条是"函数体第一条" */
    i = 0;
    if (oa->last < 0) { plog("[patch:%s] empty\n", tag); return 0; }
    plog("[patch:%s] literals[%d]: type=%d (IS_TRUE=%d)\n",
         tag, lit, (int)Z_TYPE_P(&oa->literals[lit]), (int)IS_TRUE);

    plog("[patch:%s] op_array=%p target op[%d] opcode=%d -> 62 lit=%d\n",
         tag, (void *)oa, i, (int)oa->opcodes[i].opcode, lit);

    oa->opcodes[i].opcode        = ZEND_RETURN_OPC;
        oa->opcodes[i].op1_type      = IS_CONST;
    /* PHP 7.4 的 RT_CONSTANT 是「相对当前 opline 的字节偏移」：
     *   RT_CONSTANT(opline,node) = (zval*)((char*)opline + (int32_t)node.constant)
     * 正常由 pass_two 的 ZEND_PASS_TWO_UPDATE_CONSTANT 完成转换，
     * 我们手工改写指令就得自己算：
     *   node.constant = (char*)&literals[lit] - (char*)opline
     * 写成 literal 索引会被当成指针偏移，读到垃圾 → 返回 NULL。 */
    oa->opcodes[i].op1.constant =
        (uint32_t)((char *)&oa->literals[lit] - (char *)&oa->opcodes[i]);
    oa->opcodes[i].op2_type      = IS_UNUSED;
    oa->opcodes[i].op2.var       = 0;
    oa->opcodes[i].result_type   = IS_UNUSED;
    oa->opcodes[i].result.var    = 0;
    oa->opcodes[i].extended_value = 0;
    /* 用函数而不是 ZEND_VM_SET_OPCODE_HANDLER 宏（那个宏在
     * zend_vm_execute.h 里，那个文件太大不能 include）。
     * zend_vm_set_opcode_handler 是 ZEND_API 导出函数，会自动按
     * opcode + op1_type/op2_type 选正确的特化 handler。 */
    zend_vm_set_opcode_handler(&oa->opcodes[i]);

    plog("[patch:%s] verify op[%d]: opcode=%d handler=%p op1_type=%d\n",
         tag, i, (int)oa->opcodes[i].opcode, oa->opcodes[i].handler,
         (int)oa->opcodes[i].op1_type);
    return 1;
}

static void try_dump(void) {
    static int done = 0;
    zend_function *eg, *cg;

    if (done) return;
    eg = zend_hash_str_find_ptr(EG(function_table), "templateauthorization",
                                strlen("templateauthorization"));
    if (!eg) return;                      /* 还没注册，下次再试 */
    done = 1;

    cg = CG(function_table)
       ? zend_hash_str_find_ptr(CG(function_table), "templateauthorization",
                                strlen("templateauthorization")) : NULL;
    plog("[try] eg=%p cg=%p same=%d\n", (void *)eg, (void *)cg, (int)(eg == cg));
    patch_fn(eg, "EG");
    if (cg) patch_fn(cg, "CG");
    {
        zend_function *f2 = zend_hash_str_find_ptr(EG(function_table), "verifysecuritycode",
                                                  strlen("verifysecuritycode"));
        if (f2) patch_fn(f2, "verify-EG");
    }
}

static void probe_exec(zend_execute_data *execute_data) {
    try_dump();
    execute_ex(execute_data);
}

PHP_MINIT_FUNCTION(sukehappy) { return SUCCESS; }
PHP_RINIT_FUNCTION(sukehappy) { zend_execute_ex = probe_exec; return SUCCESS; }
PHP_MINFO_FUNCTION(sukehappy) { php_info_print_table_start(); php_info_print_table_row(2,"sukehappy","x"); php_info_print_table_end(); }

zend_module_entry sukehappy_module_entry = {
    STANDARD_MODULE_HEADER, "sukehappy", NULL,
    PHP_MINIT(sukehappy), NULL, PHP_RINIT(sukehappy), NULL, PHP_MINFO(sukehappy),
    "1.0", STANDARD_MODULE_PROPERTIES
};
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(sukehappy)
