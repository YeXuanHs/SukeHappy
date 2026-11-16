/*
 * suke_probe.c — 探针：确定 swoole_loader 解密后的源码从哪条路径流出
 * 必须先于 swoole_loader 加载（后加载者包裹先加载者）
 * 只写日志到 /tmp/suke_probe.log，不改任何行为。
 */
#include "php.h"
#include <zend_compile.h>
#include <zend_ast.h>
#include <zend_builtin_functions.h>
#include <string.h>

static zend_op_array *(*orig_compile_file)(zend_file_handle *fh, int type);
#if PHP_VERSION_ID >= 80000
static zend_op_array *(*orig_compile_string)(zend_string *source_string, char *filename);
#else
static zend_op_array *(*orig_compile_string)(zval *source_string, char *filename);
#endif

static void probe_log(const char *tag, const char *fn, const char *head, size_t len)
{
    FILE *f;
    size_t i;
    int has_fn = 0, has_php = 0;

    if (head && len) {
        has_fn = (memmem(head, len, "function templateAuthorization", 31) != NULL);
        has_php = (memmem(head, len, "<?php", 5) != NULL);
    }

    f = fopen("/tmp/suke_probe.log", "a");
    if (!f) return;
    fprintf(f, "[%s] fn=%s len=%zu plain_templateAuthorization=%d has_open_tag=%d\n  head=",
            tag, fn ? fn : "(null)", len, has_fn, has_php);
    for (i = 0; i < len && i < 120; i++) {
        unsigned char c = (unsigned char)head[i];
        if (c == '\n')      fprintf(f, "\\n");
        else if (c == '\r') fprintf(f, "\\r");
        else if (c >= 32 && c < 127) fputc(c, f);
        else fprintf(f, "\\x%02x", c);
    }
    fprintf(f, "\n");
    fclose(f);
}

static zend_op_array *probe_compile_file(zend_file_handle *fh, int type)
{
    const char *fn = NULL;
    if (fh && fh->opened_path) fn = ZSTR_VAL(fh->opened_path);
    if (!fn || !*fn) {
#if PHP_VERSION_ID >= 80100
        fn = fh->filename ? ZSTR_VAL(fh->filename) : NULL;
#else
        fn = fh->filename;
#endif
    }
    probe_log("compile_file", fn, "", 0);
    return orig_compile_file(fh, type);
}

static zend_op_array *probe_compile_string(
#if PHP_VERSION_ID >= 80000
    zend_string *source_string, char *filename
#else
    zval *source_string, char *filename
#endif
)
{
    const char *buf;
    size_t len;

#if PHP_VERSION_ID >= 80000
    buf = ZSTR_VAL(source_string);
    len = ZSTR_LEN(source_string);
#else
    buf = Z_STRVAL_P(source_string);
    len = Z_STRLEN_P(source_string);
#endif
    probe_log("compile_string", filename, buf, len);

    return orig_compile_string(source_string, filename);
}

PHP_MINIT_FUNCTION(suke_probe)
{
    orig_compile_file   = zend_compile_file;
    orig_compile_string = zend_compile_string;

    zend_compile_file   = probe_compile_file;
    zend_compile_string = probe_compile_string;

    return SUCCESS;
}

zend_module_entry suke_probe_module_entry = {
    STANDARD_MODULE_HEADER,
    "suke_probe",
    NULL,
    PHP_MINIT(suke_probe),
    NULL,   /* mshutdown */
    NULL,   /* rinit */
    NULL,   /* rshutdown */
    NULL,   /* post_deactivate */
    "1.0",
    STANDARD_MODULE_PROPERTIES
};

ZEND_GET_MODULE(suke_probe)
