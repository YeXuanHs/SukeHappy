/* 最小 zend_extension 探针：验证 op_array_handler 是否被 loader 触发 */
#include "php.h"
#include "zend_extensions.h"
#include <unistd.h>

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
extern zend_extension zend_extension_entry;

/* 函数调用时触发（每次调用前）。如果这里能看到 templateauthorization，
 * 就说明执行阶段走的是标准 VM —— 那它就是可用的注入点。 */
static void my_fcall_begin(zend_execute_data *frame) {
    const char *fn;
    FILE *f;
    if (!frame || !frame->func || !frame->func->common.function_name) return;
    fn = ZSTR_VAL(frame->func->common.function_name);
    f = fopen("/tmp/ze.log", "a");
    if (!f) return;
    fprintf(f, "FCALL %s\n", fn);
    fclose(f);
}

/* build_id_check 和 api_no_check 一样：传 NULL 会让 PHP 走默认实现，
 * 读到垃圾值后**静默丢弃扩展**（表现为脚本不执行、无任何输出）。 */
static int my_build_id_check(const char *build_id) {
    const char *m = "[ze_probe] build_id_check CALLED\n";
    if (write(2, m, strlen(m)) < 0) { /* ignore */ }
    return SUCCESS;
}

static int my_api_no_check(int api_no) {
    void **f = (void **)&zend_extension_entry;
    int i;
    ze_log("api_no_check: %d (expect %d)\n", api_no, ZEND_EXTENSION_API_NO);
    /* dump 整个结构，看 PHP 看到的布局 */
    for (i = 0; i < 26; i++) {
        ze_log("  entry[%2d] = %p\n", i, f[i]);
    }
    /* 前 5 个当字符串打印（验证 name/version/... 没被错位） */
    for (i = 0; i < 5; i++) {
        char *str = (char *)f[i];
        ze_log("  str[%d] = %s\n", i, str ? str : "(null)");
    }
    return SUCCESS;
}

static int my_startup(zend_extension *ze) {
    /* 用 write(2) 直写 stderr，绕开 fopen 可能的权限/路径问题 */
    const char *m = "[ze_probe] STARTUP CALLED\n";
    if (write(2, m, strlen(m)) < 0) { /* ignore */ }
    ze_log("ZE startup: %s\n", ze ? ze->name : "?");
    return SUCCESS;
}

/* 关键：extension_version_info 是**结构体**不是字符串！
 * 源码 Zend/zend_extensions.h:
 *   typedef struct _zend_extension_version_info {
 *       int zend_extension_api_no;
 *       char *build_id;
 *   } zend_extension_version_info;
 * 写成 char* 会让 PHP 把字符串后的垃圾当 build_id →
 * zend_load_extension_handle 里 strcmp(ZEND_EXTENSION_BUILD_ID, build_id) 直接段错误。 */
ZEND_DLEXPORT zend_extension_version_info extension_version_info = {
    ZEND_EXTENSION_API_NO,
    ZEND_EXTENSION_BUILD_ID
};

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
    my_fcall_begin,             /* fcall_begin_handler */
    NULL,                       /* fcall_end_handler */
    NULL,                       /* op_array_ctor */
    NULL,                       /* op_array_dtor */
    my_api_no_check,            /* api_no_check */
    my_build_id_check,          /* build_id_check */
    NULL,                       /* op_array_persist_calc */
    NULL,                       /* op_array_persist */
    /* 实测：PHP 7.3 在 zend_load_extension_handle 里会拿 ZEND_BUILD_ID 和
     * 结构里某个 reserved 字段做 strcmp，该字段为 NULL 时直接崩。
     * 四个 reserved 全部填上 ZEND_BUILD_ID（无法确定具体是哪个，全填最保险）。 */
    (void *)"API320180731,NTS", (void *)"API320180731,NTS",
    (void *)"API320180731,NTS", (void *)"API320180731,NTS",
    0,                          /* handle */
    -1                          /* resource_number（官方宏里就是 -1） */
};
