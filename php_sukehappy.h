#ifndef PHP_SUKEHAPPY_H
#define PHP_SUKEHAPPY_H

extern zend_module_entry sukehappy_module_entry;
#define phpext_sukehappy_ptr &sukehappy_module_entry

#define PHP_SUKEHAPPY_VERSION "1.1.0"

ZEND_BEGIN_MODULE_GLOBALS(sukehappy)
    char *logfile;
    zend_bool auth_patch;
    zend_bool auth_patched;
    zend_bool block_sql;
    char *block_functions;
    char *block_uris;
ZEND_END_MODULE_GLOBALS(sukehappy)

ZEND_EXTERN_MODULE_GLOBALS(sukehappy)
#define SUKE_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(sukehappy, v)

#endif /* PHP_SUKEHAPPY_H */
