/*
  +----------------------------------------------------------------------+
  | PHP Version 5.3                                                      |
  +----------------------------------------------------------------------+
  | Copyright (c) 2002-2011 Dmitri Vinogradov                            |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Dmitri Vinogradov <dmitri.vinogradov@gmail.com>             |
  +----------------------------------------------------------------------+
*/

#include "Server.h"

zend_class_entry *ce_can_server_route;
static zend_object_handlers server_route_obj_handlers;

static void server_route_dtor(void *object TSRMLS_DC);

static zend_object_value server_route_ctor(zend_class_entry *ce TSRMLS_DC)
{
    struct php_can_server_route *route;
    zend_object_value retval;

    route = ecalloc(1, sizeof(*route));
    zend_object_std_init(&route->std, ce TSRMLS_CC);
    route->handler = NULL;
    route->methods = 0;
    route->regexp = NULL;
    route->route = NULL;
    route->casts = NULL;
    retval.handle = zend_objects_store_put(route,
            (zend_objects_store_dtor_t)zend_objects_destroy_object,
            server_route_dtor,
            NULL TSRMLS_CC);
    retval.handlers = &server_route_obj_handlers;
    return retval;
}

static void server_route_dtor(void *object TSRMLS_DC)
{
    struct php_can_server_route *route = (struct php_can_server_route*)object;

    if (route->handler) {
        zval_ptr_dtor(&route->handler);
    }

    if (route->regexp) {
        efree(route->regexp);
        route->regexp = NULL;
    }

    if (route->route) {
        efree(route->route);
        route->route = NULL;
    }
    
    if (route->casts) {
        zval_ptr_dtor(&route->casts);
    }

    zend_objects_store_del_ref(&route->refhandle TSRMLS_CC);
    zend_object_std_dtor(&route->std TSRMLS_CC);
    efree(route);

}

/**
 * Constructor
 */
static PHP_METHOD(CanServerRoute, __construct)
{
    char *route;
    zval *handler;
    int route_len;
    long methods = PHP_CAN_SERVER_ROUTE_METHOD_GET;

    if (FAILURE == zend_parse_parameters_ex(ZEND_PARSE_PARAMS_QUIET, ZEND_NUM_ARGS() TSRMLS_CC,
            "sz|l", &route, &route_len, &handler, &methods)) {
        const char *space, *class_name = get_active_class_name(&space TSRMLS_CC);
        php_can_throw_exception(
            ce_can_InvalidParametersException TSRMLS_CC,
            "%s%s%s(string $route, mixed $handler[, int $methods = Route::METHOD_GET])",
            class_name, space, get_active_function_name(TSRMLS_C)
        );
        return;
    }

    struct php_can_server_route *request = (struct php_can_server_route*)
        zend_object_store_get_object(getThis() TSRMLS_CC);

    char *func_name;
    zend_bool is_callable = zend_is_callable(handler, 0, &func_name TSRMLS_CC);
    if (!is_callable) {
        php_can_throw_exception(
            ce_can_InvalidCallbackException TSRMLS_CC,
            "Handler '%s' is not a valid callback",
            func_name
        );
        efree(func_name);
        return;
    }
    efree(func_name);
    
    zval_add_ref(&handler);
    request->handler = handler;
    
    MAKE_STD_ZVAL(request->casts);
    array_init(request->casts);
    
    if (FAILURE != php_can_strpos(route, "<", 0) && FAILURE != php_can_strpos(route, ">", 0)) {
        int i;
        for (i = 0; i < route_len; i++) {
            if (route[i] != '<') {
                spprintf(&request->regexp, 0, "%s%c", request->regexp == NULL ? "" : request->regexp, route[i]);
            } else {
                int y = php_can_strpos(route, ">", i);
                char *name = php_can_substr(route, i + 1, y - (i + 1));
                int pos = php_can_strpos(name, ":", 0);
                if (FAILURE != pos) {
                    char *var = php_can_substr(name, 0, pos);
                    char *filter = php_can_substr(name, pos + 1, strlen(name) - (pos + 1));
                    if (strcmp(filter, "int") == 0) {
                        spprintf(&request->regexp, 0, "%s(?<%s>%s)", request->regexp, var, "-?[0-9]+");
                        add_assoc_long(request->casts, var, IS_LONG);
                    } else if (0 == strcmp(filter, "float")) {
                        spprintf(&request->regexp, 0, "%s(?<%s>%s)", request->regexp, var, "-?[0-9.]+");
                        add_assoc_long(request->casts, var, IS_DOUBLE);
                    } else if (0 == strcmp(filter, "path")) {
                        spprintf(&request->regexp, 0, "%s(?<%s>%s)", request->regexp, var, ".+?");
                        add_assoc_long(request->casts, var, IS_PATH);
                    } else if (0 == (pos = php_can_strpos(filter, "re:", 0))) {
                        char *reg = php_can_substr(filter, pos + 3, strlen(filter) - (pos + 3));
                        spprintf(&request->regexp, 0, "%s(?<%s>%s)", request->regexp, var, reg);
                        efree(reg);
                    }
                    efree(filter);
                    efree(var);
                    
                } else {
                    spprintf(&request->regexp, 0, "%s(?<%s>[^/]+)", request->regexp, name);
                }
                efree(name);
                i = y;
            }
        }
        spprintf(&request->regexp, 0, "\1^%s$\1", request->regexp);
    }
    
    request->route = estrndup(route, route_len);
    
    if (methods & PHP_CAN_SERVER_ROUTE_METHOD_ALL) {
        request->methods = methods;
    } else {
        php_can_throw_exception(
            ce_can_InvalidParametersException TSRMLS_CC,
            "Unexpected methods",
            func_name
        );
    }

}

/**
 * Get URI
 */
static PHP_METHOD(CanServerRoute, getUri)
{
    zend_bool as_regexp = 0;
    if (FAILURE == zend_parse_parameters_ex(ZEND_PARSE_PARAMS_QUIET, ZEND_NUM_ARGS() TSRMLS_CC,
            "|b", &as_regexp)) {
        const char *space, *class_name = get_active_class_name(&space TSRMLS_CC);
        php_can_throw_exception(
            ce_can_InvalidParametersException TSRMLS_CC,
            "%s%s%s([bool $as_regexp])",
            class_name, space, get_active_function_name(TSRMLS_C)
        );
        return;
    }
    
    struct php_can_server_route *route = (struct php_can_server_route*)
        zend_object_store_get_object(getThis() TSRMLS_CC);
    
    if (as_regexp) {
        if (route->regexp != NULL) {
            char *regexp = php_can_substr(route->regexp, 1, strlen(route->regexp) - 2);
            RETVAL_STRING(regexp, 0);
        } else {
            RETVAL_FALSE;
        }
    } else {
        RETVAL_STRING(route->route, 1);
    }
}


/**
 * Get HTTP method this route applies to
 */
static PHP_METHOD(CanServerRoute, getMethod)
{
    zend_bool as_regexp = 0;
    if (FAILURE == zend_parse_parameters_ex(ZEND_PARSE_PARAMS_QUIET, ZEND_NUM_ARGS() TSRMLS_CC,
            "|b", &as_regexp)) {
        const char *space, *class_name = get_active_class_name(&space TSRMLS_CC);
        php_can_throw_exception(
            ce_can_InvalidParametersException TSRMLS_CC,
            "%s%s%s([bool $as_regexp])",
            class_name, space, get_active_function_name(TSRMLS_C)
        );
        return;
    }
    
    struct php_can_server_route *route = (struct php_can_server_route*)
        zend_object_store_get_object(getThis() TSRMLS_CC);
    
    if (as_regexp) {
        char *retval = NULL;
        if (route->methods & PHP_CAN_SERVER_ROUTE_METHOD_GET) {
            spprintf(&retval, 0, "%s%s%s", (retval == NULL ? "" : retval), 
                    (retval == NULL ? "" : "|"), "GET");
        }
        if (route->methods & PHP_CAN_SERVER_ROUTE_METHOD_POST) {
            spprintf(&retval, 0, "%s%s%s", (retval == NULL ? "" : retval), 
                    (retval == NULL ? "" : "|"), "POST");
        }
        if (route->methods & PHP_CAN_SERVER_ROUTE_METHOD_HEAD) {
            spprintf(&retval, 0, "%s%s%s", (retval == NULL ? "" : retval), 
                    (retval == NULL ? "" : "|"), "HEAD");
        }
        if (route->methods & PHP_CAN_SERVER_ROUTE_METHOD_PUT) {
            spprintf(&retval, 0, "%s%s%s", (retval == NULL ? "" : retval), 
                    (retval == NULL ? "" : "|"), "PUT");
        }
        if (route->methods & PHP_CAN_SERVER_ROUTE_METHOD_DELETE) {
            spprintf(&retval, 0, "%s%s%s", (retval == NULL ? "" : retval), 
                    (retval == NULL ? "" : "|"), "DELETE");
        }
        if (route->methods & PHP_CAN_SERVER_ROUTE_METHOD_OPTIONS) {
            spprintf(&retval, 0, "%s%s%s", (retval == NULL ? "" : retval), 
                    (retval == NULL ? "" : "|"), "OPTIONS");
        }
        if (route->methods & PHP_CAN_SERVER_ROUTE_METHOD_TRACE) {
            spprintf(&retval, 0, "%s%s%s", (retval == NULL ? "" : retval), 
                    (retval == NULL ? "" : "|"), "TRACE");
        }
        if (route->methods & PHP_CAN_SERVER_ROUTE_METHOD_CONNECT) {
            spprintf(&retval, 0, "%s%s%s", (retval == NULL ? "" : retval), 
                    (retval == NULL ? "" : "|"), "CONNECT");
        }
        if (route->methods & PHP_CAN_SERVER_ROUTE_METHOD_PATCH) {
            spprintf(&retval, 0, "%s%s%s", (retval == NULL ? "" : retval), 
                    (retval == NULL ? "" : "|"), "PATCH");
        }
        int len = spprintf(&retval, 0, "(%s)", retval);
        RETVAL_STRINGL(retval, len, 0);
    } else {
        RETVAL_LONG(route->methods);
    }
  
}

/**
 * Get request handler associated with this route
 */
static PHP_METHOD(CanServerRoute, getHandler)
{
    struct php_can_server_route *route = (struct php_can_server_route*)
        zend_object_store_get_object(getThis() TSRMLS_CC);
    
    RETURN_ZVAL(route->handler, 1, 0);
}

static zend_function_entry server_route_methods[] = {
    PHP_ME(CanServerRoute, __construct, NULL, ZEND_ACC_FINAL | ZEND_ACC_PUBLIC)
    PHP_ME(CanServerRoute, getUri,      NULL, ZEND_ACC_FINAL | ZEND_ACC_PUBLIC)
    PHP_ME(CanServerRoute, getMethod,   NULL, ZEND_ACC_FINAL | ZEND_ACC_PUBLIC)
    PHP_ME(CanServerRoute, getHandler,  NULL, ZEND_ACC_FINAL | ZEND_ACC_PUBLIC)
    {NULL, NULL, NULL}
};

static void server_route_init(TSRMLS_D)
{
    memcpy(&server_route_obj_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    server_route_obj_handlers.clone_obj = NULL;

    // class \Can\Server\Route
    PHP_CAN_REGISTER_CLASS(
        &ce_can_server_route,
        ZEND_NS_NAME(PHP_CAN_SERVER_NS, "Route"),
        server_route_ctor,
        server_route_methods
    );

    PHP_CAN_REGISTER_CLASS_CONST_LONG(ce_can_server_route, "METHOD_GET",     PHP_CAN_SERVER_ROUTE_METHOD_GET);
    PHP_CAN_REGISTER_CLASS_CONST_LONG(ce_can_server_route, "METHOD_POST",    PHP_CAN_SERVER_ROUTE_METHOD_POST);
    PHP_CAN_REGISTER_CLASS_CONST_LONG(ce_can_server_route, "METHOD_HEAD",    PHP_CAN_SERVER_ROUTE_METHOD_HEAD);
    PHP_CAN_REGISTER_CLASS_CONST_LONG(ce_can_server_route, "METHOD_PUT",     PHP_CAN_SERVER_ROUTE_METHOD_PUT);
    PHP_CAN_REGISTER_CLASS_CONST_LONG(ce_can_server_route, "METHOD_DELETE",  PHP_CAN_SERVER_ROUTE_METHOD_DELETE);
    PHP_CAN_REGISTER_CLASS_CONST_LONG(ce_can_server_route, "METHOD_OPTIONS", PHP_CAN_SERVER_ROUTE_METHOD_OPTIONS);
    PHP_CAN_REGISTER_CLASS_CONST_LONG(ce_can_server_route, "METHOD_TRACE",   PHP_CAN_SERVER_ROUTE_METHOD_TRACE);
    PHP_CAN_REGISTER_CLASS_CONST_LONG(ce_can_server_route, "METHOD_CONNECT", PHP_CAN_SERVER_ROUTE_METHOD_CONNECT);
    PHP_CAN_REGISTER_CLASS_CONST_LONG(ce_can_server_route, "METHOD_PATCH",   PHP_CAN_SERVER_ROUTE_METHOD_PATCH);
}

PHP_MINIT_FUNCTION(can_server_route)
{
    server_route_init(TSRMLS_C);
    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(can_server_route)
{
    return SUCCESS;
}

PHP_RINIT_FUNCTION(can_server_route)
{
    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(can_server_route)
{
    return SUCCESS;
}
