#ifndef PHP_SUKEHAPPY_H
#define PHP_SUKEHAPPY_H

extern zend_module_entry sukehappy_module_entry;
#define phpext_sukehappy_ptr &sukehappy_module_entry

#ifdef PHP_WIN32
#	define PHP_SUKEHAPPY_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#	define PHP_SUKEHAPPY_API __attribute__ ((visibility("default")))
#else
#	define PHP_SUKEHAPPY_API
#endif

#ifdef ZTS
#	define SUKEHAPPY_TSRMLS_CACHE_DEFINE() ZEND_TSRMLS_CACHE_DEFINE()
#else
#	define SUKEHAPPY_TSRMLS_CACHE_DEFINE()
#endif

ZEND_BEGIN_MODULE_GLOBALS(sukehappy)
    char *logfile;
    zend_bool mock_auth;
    zend_long mock_port;
    zend_bool block_sql;
    char *block_functions;
    char *block_uris;
    HashTable curl_urls;
    HashTable curl_mocked;
ZEND_END_MODULE_GLOBALS(sukehappy)

ZEND_EXTERN_MODULE_GLOBALS(sukehappy)

#define SUKEHAPPY_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(sukehappy, v)

#endif /* PHP_SUKEHAPPY_H */
