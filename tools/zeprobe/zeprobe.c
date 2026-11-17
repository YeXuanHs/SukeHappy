/* 最小 zend_extension 探针：验证 op_array_handler 是否被 loader 触发 */
#include "php.h"
#include "zend_extensions.h"

static void ze_log(const char *fmt, ...) {
    FILE *f = fopen("/tmp/ze.log", "a");
    va_list a;
    if (!f) return;
    va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fclose(f);
}

static void my_op_array_handler(zend_op_array *oa) {
    if (!oa) return;
    if (oa->function_name) {
        ze_log("OP_ARRAY fn=%s last=%d\n", ZSTR_VAL(oa->function_name), (int)oa->last);
    } else {
        ze_log("OP_ARRAY (file) last=%d\n", (int)oa->last);
    }
}

static void my_statement_handler(zend_execute_data *frame) {
    static int n = 0;
    if (n < 5 && frame && frame->func && frame->func->common.function_name) {
        n++;
        ze_log("STMT in %s\n", ZSTR_VAL(frame->func->common.function_name));
    }
}

/* 自己实现 API 版本检查：总是通过。
 * 传 NULL 会让 PHP 走默认实现，它在某些结构下会读到垃圾值
 * （报 "requires Zend Engine API version -135446428"）。 */
static int my_api_no_check(int api_no) {
    ze_log("api_no_check called: %d (ours expects %d)\n", api_no, ZEND_EXTENSION_API_NO);
    return SUCCESS;
}

static int my_startup(zend_extension *ze) {
    ze_log("ZE startup: %s\n", ze->name);
    return SUCCESS;
}

/* 老式版本信息（16 字节，和 ioncube 一样）。PHP 在没有
 * zend_extension_entry 时会回落到用它创建默认结构。加上无副作用。 */
ZEND_DLEXPORT char *extension_version_info = "1.0";

ZEND_DLEXPORT zend_extension zend_extension_entry = {
    "ze_probe",                 /* name */
    "1.0",                      /* version */
    "suke",                     /* author */
    "-",                        /* URL */
    "-",                        /* copyright */
    my_startup,                 /* startup */
    NULL,                       /* shutdown */
    NULL,                       /* activate */
    NULL,                       /* deactivate */
    NULL,                       /* message_handler */
    my_op_array_handler,        /* op_array_handler   <== 关键 */
    my_statement_handler,       /* statement_handler */
    NULL,                       /* fcall_begin_handler */
    NULL,                       /* fcall_end_handler */
    NULL,                       /* op_array_ctor */
    NULL,                       /* op_array_dtor */
    my_api_no_check,            /* api_no_check */
    NULL,                       /* build_id_check */
    NULL,                       /* op_array_persist_calc */
    NULL,                       /* op_array_persist */
    NULL, NULL, NULL, NULL,     /* reserved5-8 */
    0,                          /* handle */
    0                           /* resource_number */
};
