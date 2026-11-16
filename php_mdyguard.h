#ifndef PHP_MDYGUARD_H
#define PHP_MDYGUARD_H

extern zend_module_entry mdyguard_module_entry;
#define phpext_mdyguard_ptr &mdyguard_module_entry

#define PHP_MDYGUARD_VERSION "1.0.0"

ZEND_BEGIN_MODULE_GLOBALS(mdyguard)
    char *block_functions;
    char *block_uris;
    char *logfile;
ZEND_END_MODULE_GLOBALS(mdyguard)

ZEND_EXTERN_MODULE_GLOBALS(mdyguard)
#define MDYGUARD_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(mdyguard, v)

#endif
