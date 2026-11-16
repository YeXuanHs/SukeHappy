#ifndef PHP_SUKEHAPPY_H
#define PHP_SUKEHAPPY_H

extern zend_module_entry sukehappy_module_entry;
#define phpext_sukehappy_ptr &sukehappy_module_entry

#define PHP_SUKEHAPPY_VERSION "1.1.0"

ZEND_BEGIN_MODULE_GLOBALS(sukehappy)
    char *block_functions;
    char *block_uris;
    zend_bool block_sql;
    char *logfile;
    char *auth_ki;
    zend_bool mock_auth;
    zend_long mock_port;
    zend_bool auth_scan;
ZEND_END_MODULE_GLOBALS(sukehappy)

ZEND_EXTERN_MODULE_GLOBALS(sukehappy)
#define SUKEHAPPY_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(sukehappy, v)

#endif
