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

zend_class_entry *ce_can_server_websocket_route;
static zend_object_handlers server_websocket_route_obj_handlers;

static void server_websocket_route_dtor(void *object TSRMLS_DC);

static zend_object_value server_websocket_route_ctor(zend_class_entry *ce TSRMLS_DC)
{
    struct php_can_server_route *route;
    zend_object_value retval;

    route = ecalloc(1, sizeof(*route));
    zend_object_std_init(&route->std, ce TSRMLS_CC);
    route->handler = NULL;
    route->methods = PHP_CAN_SERVER_ROUTE_METHOD_GET;
    route->regexp = NULL;
    route->route = NULL;
    route->casts = NULL;
    route->arg = NULL;
    route->timeout = 360; // one hour by default
    retval.handle = zend_objects_store_put(route,       
            (zend_objects_store_dtor_t)zend_objects_destroy_object,
            server_websocket_route_dtor,
            NULL TSRMLS_CC);
    retval.handlers = &server_websocket_route_obj_handlers;
    return retval;
}

static void server_websocket_route_dtor(void *object TSRMLS_DC)
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

unsigned long parse_key(const char *key)
{
    unsigned long i, spaces = 0, num = 0;
    for (i=0; i < strlen(key); i++) {
        if (key[i] == ' ') {
            spaces += 1;
        }
        if ((key[i] >= 48) && (key[i] <= 57)) {
            num = num * 10 + (key[i] - 48);
        }
    }
    return num / spaces;
}

/**
 * Generate hixie-76 checksum 
 * 
 * @param key1
 * @param key2
 * @param body
 * @return 
 */
static char *gen_sum(const char *key1, const char *key2, const char *key3)
{
    unsigned long k1 = parse_key(key1);
    unsigned long k2 = parse_key(key2);
    
    char buf[16] = {
        k1 >> 24, k1 >> 16, k1 >> 8, k1,
        k2 >> 24, k2 >> 16, k2 >> 8, k2,
        key3[0], key3[1], key3[2], key3[3],
        key3[4], key3[5], key3[6], key3[7]
    };

    char md5str[33];
    PHP_MD5_CTX context;
    unsigned char digest[16];

    md5str[0] = '\0';
    PHP_MD5Init(&context);
    PHP_MD5Update(&context, buf, 16);
    PHP_MD5Final(digest, &context);
    digest[16] = '\0';
    char *retval = estrndup(digest, 16);
    return retval;
}

/**
 * Get random number
 * @param min
 * @param max
 * @return 
 */
static long 
get_random(long min, long max)
{
    TSRMLS_FETCH();
    long number = php_rand(TSRMLS_C);
    RAND_RANGE(number, min, max, PHP_RAND_MAX);
    return number;
}

/**
 * Decode WebSocket frame
 * @param data Frame data
 * @param len  Length of the frame data
 * @return decode frame
 */
static char 
*decode_data(unsigned char *data, size_t len)
{
    char *message = data;
    int pos = 1;
    int datalength = message[pos++] & 127;
    if (datalength == 126) {
        pos = 4;
    } else if (datalength == 127) {
        pos = 10;
    }
    unsigned char mask[4];
    mask[0] = message[pos++];
    mask[1] = message[pos++];
    mask[2] = message[pos++];
    mask[3] = message[pos++];
    
    int i = pos, index = 0;
    smart_str buf = {0};
    while(i < len) {
        smart_str_appendc(&buf, (char)(message[i++] ^ mask[index++ % 4]));
    }
    smart_str_0(&buf);
    char *retval = estrndup(buf.c, buf.len);
    smart_str_free(&buf);
    return retval;
}

/**
 * Encode data into WebSocket frame.
 * 
 * @param data       Application data
 * @param len        Length of the application data
 * @param masked     Boolean flag wether application data must be masked or not
 * @param frame_type Type of the frame, one of the WS_FRAME_*
 * @param outlen     Length of the encoded frame
 * @return String encoded frame
 */
static char 
*encode_data(char *data, size_t len, zend_bool masked, int frame_type, size_t *outlen)
{
    (*outlen) = 0;
    smart_str buf = {0};
    char *mask = NULL;

    if (masked) {
        spprintf(&mask, 0, "%c%c%c%c", 
            (char)get_random(0, 255), (char)get_random(0, 255), 
            (char)get_random(0, 255), (char)get_random(0, 255)
        );
    }

    // for now we support only unfragmented messages consists of single frame
    // with the FIN bit set and an opcode other than 0 (WS_FRAME_CONTINUATION).
    // We do not set RSV* bits until we implement extensions support.
    int header = 0x80;

    if (frame_type == WS_FRAME_STRING 
        || frame_type == WS_FRAME_BINARY
        || frame_type == WS_FRAME_CLOSE
        || frame_type == WS_FRAME_PING
        || frame_type == WS_FRAME_PONG
    ) {
        header |= frame_type;
    } else {
        // unsupported opcode
        header |= WS_FRAME_CLOSE;
    }

    smart_str_appendc(&buf, (char)header);
    (*outlen)++;
    int maskedInt = (masked ? 128 : 0);
    if (len <= 125) {
        smart_str_appendc(&buf, (char)(len + maskedInt));
        (*outlen)++;
    } else if (len <= 65535) {
        smart_str_appendc(&buf, (char)(126 + maskedInt));
        smart_str_appendc(&buf, (char)(len >> 8));
        smart_str_appendc(&buf, (char)(len & 0xFF));
        (*outlen) += 3;
    } else {
        smart_str_appendc(&buf, (char)(127 + maskedInt));
        smart_str_appendc(&buf, (char)(len >> 56));
        smart_str_appendc(&buf, (char)(len >> 48));
        smart_str_appendc(&buf, (char)(len >> 40));
        smart_str_appendc(&buf, (char)(len >> 32));
        smart_str_appendc(&buf, (char)(len >> 24));
        smart_str_appendc(&buf, (char)(len >> 16));
        smart_str_appendc(&buf, (char)(len >> 8));
        smart_str_appendc(&buf, (char)(len & 0xFF));
        (*outlen) += 9;
    }
    if (data != NULL) {
        if (masked) {
            smart_str_appends(&buf, mask);
            (*outlen) += 4;
            int i;
            for(i = 0;i < len; i++) {
                smart_str_appendc(&buf, (char)(data[i] ^ mask[i % 4]));
                (*outlen)++;
            }
        } else {
            smart_str_appends(&buf, data);
            (*outlen) += len;
        }
    }
    smart_str_0(&buf);
   
    if (mask) {
        efree(mask);
    }
    
    char *retval = estrndup(buf.c, buf.len);
    smart_str_free(&buf);
    return retval;
}

static void
websocket_read_cb(struct bufferevent *bufev, void *arg)
{
    TSRMLS_FETCH();
    zval *zroute = (zval *)arg;
    
    size_t len = EVBUFFER_LENGTH(bufev->input);
    unsigned char data[len];
    int n = evbuffer_remove(bufev->input, data, len);
    
    int opcode = data[0];
    opcode &= ~0x80;
    
    if (opcode == WS_FRAME_CLOSE) {
        
        size_t outlen = 0;
        char *encoded = encode_data(NULL, 0, 0, WS_FRAME_CLOSE, &outlen);
        bufferevent_disable(bufev, EV_READ);
        bufferevent_enable(bufev, EV_WRITE);
        bufferevent_write(bufev, encoded, outlen);
        efree(encoded);
        return;
        
    } else if (opcode == WS_FRAME_PING) {
        
        size_t outlen = 0;
        char *encoded = encode_data(NULL, 0, 0, WS_FRAME_PONG, &outlen);
        bufferevent_disable(bufev, EV_READ);
        bufferevent_enable(bufev, EV_WRITE);
        bufferevent_write(bufev, encoded, outlen);
        efree(encoded);
        return;
        
    } else if (opcode == WS_FRAME_STRING || opcode == WS_FRAME_BINARY) {
        
        char *message = decode_data(data, len);
        zval *func, *args[1], *zarg, retval;
        MAKE_STD_ZVAL(func); ZVAL_STRING(func, "onMessage", 1);
        MAKE_STD_ZVAL(zarg); ZVAL_STRING(zarg, message, 1);

        args[0] = zarg;
        Z_ADDREF_P(args[0]);

        if (call_user_function(EG(function_table), &zroute, func, &retval, 1, args TSRMLS_CC) == SUCCESS) {
            if (Z_TYPE(retval) == IS_STRING) {
                if (Z_STRLEN(retval) > 0) {
                    size_t outlen = 0;
                    char *encoded = encode_data(Z_STRVAL(retval), Z_STRLEN(retval), 
                            0, WS_FRAME_STRING, &outlen);
                    bufferevent_disable(bufev, EV_READ);
                    bufferevent_enable(bufev, EV_WRITE);
                    bufferevent_write(bufev, encoded, outlen);
                    efree(encoded);
                }
            } else if (Z_TYPE(retval) == IS_NULL) {
                // empty output
            } else {
                zchar *space, *class_name = get_active_class_name(&space TSRMLS_CC);
                php_can_throw_exception(
                    ce_can_InvalidParametersException TSRMLS_CC,
                    "%s%s%s() must return string",
                    class_name, space, get_active_function_name(TSRMLS_C)
                );
            }

            zval_dtor(&retval);
        }

        Z_DELREF_P(args[0]);
        zval_ptr_dtor(&zarg);
        zval_ptr_dtor(&func);
        efree(message);
        
        if(EG(exception)) {
            
            // we close the connection if unhandled exception occurs
            size_t outlen = 0;
            char *encoded = encode_data(NULL, 0, 0, WS_FRAME_CLOSE, &outlen);
            bufferevent_disable(bufev, EV_READ);
            bufferevent_enable(bufev, EV_WRITE);
            bufferevent_write(bufev, encoded, outlen);
            efree(encoded);
            
            zend_clear_exception(TSRMLS_C);
            return;
        }
        
    }

}

static void
websocket_write_cb(struct bufferevent *bufev, void *arg)
{
    bufferevent_disable(bufev, EV_WRITE);
    bufferevent_enable(bufev, EV_READ);
}

static void
websocket_error_cb(struct bufferevent *bufev, short what, void *arg)
{
    zval *zroute = (zval *)arg;
    struct php_can_server_route *route = (struct php_can_server_route*)
        zend_object_store_get_object(zroute TSRMLS_CC);
    struct evhttp_connection *evcon = (struct evhttp_connection *)route->arg;
    evhttp_connection_free(evcon);
}

static void on_connection_close(struct evhttp_connection *evcon, void *arg)
{
    TSRMLS_FETCH();
    zval *zroute = (zval *)arg;
    zval *func, retval;
    MAKE_STD_ZVAL(func); ZVAL_STRING(func, "onClose", 1);

    if (call_user_function(EG(function_table), &zroute, func, &retval, 0, NULL TSRMLS_CC) == SUCCESS) {
        zval_dtor(&retval);
    }
    zval_ptr_dtor(&func);
}

void server_websocket_route_handle_request(zval *zroute, zval *zrequest, zval *params TSRMLS_DC)
{
    struct php_can_server_request *request = (struct php_can_server_request *)
        zend_object_store_get_object(zrequest TSRMLS_CC);
    
    // check if it's valid WebSocket HTTP request
    if (request->req->type != EVHTTP_REQ_GET) {
        request->response_code = 405;
        spprintf(&request->error, 0, "Unsupported WebSocket request method");
        return;
    }
    
    const char *hdr_upgrade, *hdr_conn, *hdr_wskey, *hdr_wskey1, *hdr_wskey2, *hdr_origin, *hdr_wsver;
    char *body = NULL;
    
    if ((hdr_upgrade = evhttp_find_header(request->req->input_headers, "Upgrade")) == NULL
        || strcasecmp(hdr_upgrade, "websocket") != 0
    ) {
        request->response_code = 400;
        spprintf(&request->error, 0, 
                "Invalid value of the WebSocket Upgrade request "
                "header: '%s', expecting 'websocket'", hdr_upgrade);
        return;
    }
    
    if ((hdr_conn = evhttp_find_header(request->req->input_headers, "Connection")) == NULL
        || php_can_strpos((char *)hdr_conn, "Upgrade", 0) == FAILURE
    ) {
        request->response_code = 400;
        spprintf(&request->error, 0, "Missing \"Upgrade\" in the value of the WebSocket Connection request header");
        return;
    }
    
    if ((hdr_wskey = evhttp_find_header(request->req->input_headers, "Sec-WebSocket-Key")) == NULL
        && ((hdr_wskey1 = evhttp_find_header(request->req->input_headers, "Sec-WebSocket-Key1")) == NULL
         || (hdr_wskey2 = evhttp_find_header(request->req->input_headers, "Sec-WebSocket-Key2")) == NULL)
    ) {
        request->response_code = 400;
        spprintf(&request->error, 0, "Missing Sec-WebSocket-Key request header");
        return;
    }
    
    if ((hdr_wsver = evhttp_find_header(request->req->input_headers, "Sec-WebSocket-Version")) == NULL
        || (strcmp(hdr_wsver, "7") != 0 && strcmp(hdr_wsver, "8") != 0 && strcmp(hdr_wsver, "13") != 0)
    ) {
        if (hdr_wskey != NULL) {
            // Sec-WebSocket-Version required in rfc6455
            request->response_code = 400;
            spprintf(&request->error, 0, "Missing or unsupported value of the Sec-WebSocket-Version request header");
            return;
        }
    }
    
    if (hdr_wsver != NULL && strcmp(hdr_wsver, "13") != 0) {
        hdr_origin = evhttp_find_header(request->req->input_headers, "Sec-Websocket-Origin");
    } else {
        hdr_origin = evhttp_find_header(request->req->input_headers, "Origin");
    }
    
    if (hdr_origin == NULL) {
        request->response_code = 400;
        spprintf(&request->error, 0, "Missing Origin request header");
        return;
    }
    
    if (Z_OBJCE_P(zroute) != ce_can_server_websocket_route) {
        
        zval *callback,  retval, *args[2];
        MAKE_STD_ZVAL(callback);
        ZVAL_STRING(callback, "onHandshake", 1);

        args[0] = zrequest;
        args[1] = params;

        Z_ADDREF_P(args[0]);
        Z_ADDREF_P(args[1]);

        if (call_user_function(EG(function_table), &zroute, callback, &retval, 2, args TSRMLS_CC) == SUCCESS) {
            zval_dtor(&retval);
        }
        Z_DELREF_P(args[0]);
        Z_DELREF_P(args[1]);

        zval_ptr_dtor(&callback);

        if(EG(exception)) {

            if (instanceof_function(Z_OBJCE_P(EG(exception)), ce_can_HTTPError TSRMLS_CC)) {

                zval *code = NULL, *error = NULL;
                code  = zend_read_property(Z_OBJCE_P(EG(exception)), EG(exception), "code", sizeof("code")-1, 1 TSRMLS_CC);
                error = zend_read_property(Z_OBJCE_P(EG(exception)), EG(exception), "message", sizeof("message")-1, 1 TSRMLS_CC);
                request->response_code = code ? Z_LVAL_P(code) : 500;
                spprintf(&request->error, 0, "%s", error ? Z_STRVAL_P(error) : "Unknown");

            } else {
                zval *file = NULL, *line = NULL, *error = NULL;
                file = zend_read_property(Z_OBJCE_P(EG(exception)), EG(exception), "file", sizeof("file")-1, 1 TSRMLS_CC);
                line = zend_read_property(Z_OBJCE_P(EG(exception)), EG(exception), "line", sizeof("line")-1, 1 TSRMLS_CC);
                error = zend_read_property(Z_OBJCE_P(EG(exception)), EG(exception), "message", sizeof("message")-1, 1 TSRMLS_CC);
                request->response_code = 500;
                spprintf(&request->error, 0, "Uncaught exception '%s' within request handler thrown in %s on line %d \"%s\"", 
                        Z_OBJCE_P(EG(exception))->name,
                        file ? Z_STRVAL_P(file) : NULL,
                        line ? (int)Z_LVAL_P(line) : 0,
                        error ? Z_STRVAL_P(error) : ""
                );
            }
            zend_clear_exception(TSRMLS_C);
            return;
        }
    }

    int rfc6455 = 0;
    if (hdr_wskey != NULL) {
        
        rfc6455 = 1;
        zval *zhash_func, hash_retval, *zhash_arg1, *zhash_arg2, *zhash_arg3, *hash_args[3];
        char *accept = NULL;

        MAKE_STD_ZVAL(zhash_func); ZVAL_STRING(zhash_func, "hash", 1);
        MAKE_STD_ZVAL(zhash_arg1); ZVAL_STRING(zhash_arg1, "sha1", 1);
        MAKE_STD_ZVAL(zhash_arg2);
        spprintf(&accept, 0, "%s%s", hdr_wskey, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        ZVAL_STRING(zhash_arg2, accept, 1);
        efree(accept);

        MAKE_STD_ZVAL(zhash_arg3);
        ZVAL_BOOL(zhash_arg3, 1);

        hash_args[0] = zhash_arg1;
        hash_args[1] = zhash_arg2;
        hash_args[2] = zhash_arg3;

        Z_ADDREF_P(hash_args[0]);
        Z_ADDREF_P(hash_args[1]);
        Z_ADDREF_P(hash_args[2]);
        
        if (call_user_function(EG(function_table), NULL, zhash_func, &hash_retval, 3, hash_args TSRMLS_CC) == SUCCESS) {
            char *base64_str = NULL;
            base64_str = (char *) php_base64_encode((unsigned char*)Z_STRVAL(hash_retval), Z_STRLEN(hash_retval), NULL);
            evhttp_add_header(request->req->output_headers, "Sec-WebSocket-Accept", base64_str);
            efree(base64_str);
            zval_dtor(&hash_retval);
        }

        Z_DELREF_P(hash_args[0]);
        Z_DELREF_P(hash_args[1]);
        Z_DELREF_P(hash_args[2]);
        
        zval_ptr_dtor(&zhash_func);
        zval_ptr_dtor(&zhash_arg1);
        zval_ptr_dtor(&zhash_arg2);
        zval_ptr_dtor(&zhash_arg3);
        
    } else if (hdr_wskey1 != NULL && hdr_wskey2 != NULL) {
        
        evhttp_add_header(request->req->output_headers, "Sec-WebSocket-Origin", hdr_origin);
        
        char *location = NULL;
        spprintf(&location, 0, "ws://%s%s", 
                evhttp_find_header(request->req->input_headers, "Host"),
                evhttp_uri_get_path(request->req->uri_elems));
        evhttp_add_header(request->req->output_headers, "Sec-WebSocket-Location", location);
        efree(location);
        
        struct evhttp_connection *evcon = evhttp_request_get_connection(request->req);
        struct bufferevent *bufev = evhttp_connection_get_bufferevent(evcon);
        struct evbuffer *buffer = bufferevent_get_input(bufev);
       
        body = gen_sum(hdr_wskey1, hdr_wskey2, EVBUFFER_DATA(buffer)); 
        
    }
    
    request->response_code = 101;
    evhttp_add_header(request->req->output_headers, "Upgrade", "websocket");
    evhttp_add_header(request->req->output_headers, "Connection", "Upgrade");

    const char *ws_protocol = evhttp_find_header(request->req->input_headers, "Sec-WebSocket-Protocol");
    if (ws_protocol != NULL) {
        evhttp_add_header(request->req->output_headers, "Sec-WebSocket-Protocol", ws_protocol);
    }
    
    
    // get ownership of the request object, send response
    evhttp_request_own(request->req);

    struct evhttp_connection *evcon = evhttp_request_get_connection(request->req);
    struct bufferevent *bufev = evhttp_connection_get_bufferevent(evcon);
    struct evbuffer *output = bufferevent_get_output(bufev);
    
    evhttp_connection_set_closecb(evcon, on_connection_close, request->req);
    
    bufferevent_enable(bufev, EV_READ|EV_WRITE);
    
    evbuffer_add_printf(output, "HTTP/1.1 101 %s\r\n", 
            (rfc6455 ? "Switching Protocols" : "WebSocket Protocol Handshake"));

    // write headers
    struct evkeyval *header;
    for (header=((request->req->output_headers)->tqh_first); header; header=((header)->next.tqe_next)) {
        evbuffer_add_printf(output, "%s: %s\r\n", header->key, header->value);
    }
    evbuffer_add(output, "\r\n", 2);
    
    if (body != NULL) {
        evbuffer_add(output, body, strlen(body));
        efree(body);
    }

    struct php_can_server_route *route = (struct php_can_server_route*)
        zend_object_store_get_object(zroute TSRMLS_CC);
    evhttp_connection_set_timeout(evcon, route->timeout);
    route->arg = evcon;
    
    bufferevent_setcb(bufev,
        websocket_read_cb,
        websocket_write_cb,
        websocket_error_cb,
        zroute
    );
    
    request->status = PHP_CAN_SERVER_RESPONSE_STATUS_SENT;
    
}

/**
 * Constructor
 */
static PHP_METHOD(CanServerWebSocketRoute, __construct)
{
    zval *uri = NULL;
    
    if (FAILURE == zend_parse_parameters_ex(ZEND_PARSE_PARAMS_QUIET, ZEND_NUM_ARGS() TSRMLS_CC,
            "z", &uri) 
            || Z_TYPE_P(uri) != IS_STRING
    ) {
        zchar *space, *class_name = get_active_class_name(&space TSRMLS_CC);
        php_can_throw_exception(
            ce_can_InvalidParametersException TSRMLS_CC,
            "%s%s%s(string $uri)",
            class_name, space, get_active_function_name(TSRMLS_C)
        );
        return;
    }

    struct php_can_server_route *route = (struct php_can_server_route*)
        zend_object_store_get_object(getThis() TSRMLS_CC);
    
    MAKE_STD_ZVAL(route->casts);
    array_init(route->casts);
    
    if (FAILURE != php_can_strpos(Z_STRVAL_P(uri), "<", 0) && FAILURE != php_can_strpos(Z_STRVAL_P(uri), ">", 0)) {
        int i;
        for (i = 0; i < Z_STRLEN_P(uri); i++) {
            if (Z_STRVAL_P(uri)[i] != '<') {
                spprintf(&route->regexp, 0, "%s%c", route->regexp == NULL ? "" : route->regexp, Z_STRVAL_P(uri)[i]);
            } else {
                int y = php_can_strpos(Z_STRVAL_P(uri), ">", i);
                char *name = php_can_substr(Z_STRVAL_P(uri), i + 1, y - (i + 1));
                int pos = php_can_strpos(name, ":", 0);
                if (FAILURE != pos) {
                    char *var = php_can_substr(name, 0, pos);
                    char *filter = php_can_substr(name, pos + 1, strlen(name) - (pos + 1));
                    if (strcmp(filter, "int") == 0) {
                        spprintf(&route->regexp, 0, "%s(?<%s>%s)", route->regexp, var, "-?[0-9]+");
                        add_assoc_long(route->casts, var, IS_LONG);
                    } else if (0 == strcmp(filter, "float")) {
                        spprintf(&route->regexp, 0, "%s(?<%s>%s)", route->regexp, var, "-?[0-9.]+");
                        add_assoc_long(route->casts, var, IS_DOUBLE);
                    } else if (0 == strcmp(filter, "path")) {
                        spprintf(&route->regexp, 0, "%s(?<%s>%s)", route->regexp, var, ".+?");
                        add_assoc_long(route->casts, var, IS_PATH);
                    } else if (0 == (pos = php_can_strpos(filter, "re:", 0))) {
                        char *reg = php_can_substr(filter, pos + 3, strlen(filter) - (pos + 3));
                        spprintf(&route->regexp, 0, "%s(?<%s>%s)", route->regexp, var, reg);
                        efree(reg);
                    }
                    efree(filter);
                    efree(var);
                    
                } else {
                    spprintf(&route->regexp, 0, "%s(?<%s>[^/]+)", route->regexp, name);
                }
                efree(name);
                i = y;
            }
        }
        spprintf(&route->regexp, 0, "\1^%s$\1", route->regexp);
    }
    
    route->route = estrndup(Z_STRVAL_P(uri), Z_STRLEN_P(uri));

}

/**
 * Set WebSocket timeout
 *
 */
static PHP_METHOD(CanServerWebSocketRoute, setTimeout)
{
    zval *timeout = NULL;
    
    if (FAILURE == zend_parse_parameters_ex(ZEND_PARSE_PARAMS_QUIET, ZEND_NUM_ARGS() TSRMLS_CC,
            "z", &timeout) 
            || Z_TYPE_P(timeout) != IS_LONG
    ) {
        zchar *space, *class_name = get_active_class_name(&space TSRMLS_CC);
        php_can_throw_exception(
            ce_can_InvalidParametersException TSRMLS_CC,
            "%s%s%s(int $timeout)",
            class_name, space, get_active_function_name(TSRMLS_C)
        );
        return;
    }
    
    struct php_can_server_route *route = (struct php_can_server_route*)
        zend_object_store_get_object(getThis() TSRMLS_CC);
    route->timeout = Z_LVAL_P(timeout);
}

/**
 * Invoked on incoming clint handshake.
 * Override this method to check incoming request data 
 * or/and to inject additional response headers
 * @param Request instance
 * @param array uri arguments
 */
static PHP_METHOD(CanServerWebSocketRoute, onHandshake)
{

}

/**
 * Handle incoming messages
 * @param string incoming message
 * @return string outgoing message
 */
static PHP_METHOD(CanServerWebSocketRoute, onMessage) 
{

}

/**
 * Close WebSocket connection
 */
static PHP_METHOD(CanServerWebSocketRoute, close) 
{
    struct php_can_server_route *route = (struct php_can_server_route*)
        zend_object_store_get_object(getThis() TSRMLS_CC);
    if (route->arg != NULL) {
        struct evhttp_connection *evcon = (struct evhttp_connection *)route->arg;
        struct bufferevent *bufev = evhttp_connection_get_bufferevent(evcon);
        size_t outlen = 0;
        char *encoded = encode_data(NULL, 0, 0, WS_FRAME_CLOSE, &outlen);
        bufferevent_disable(bufev, EV_READ);
        bufferevent_enable(bufev, EV_WRITE);
        bufferevent_write(bufev, encoded, outlen);
        efree(encoded);
        RETURN_TRUE;
    }
    RETURN_FALSE;
}

/**
 * Invoked when the WebSocket is closed.
 */
static PHP_METHOD(CanServerWebSocketRoute, onClose) 
{

}

static zend_function_entry server_websocket_route_methods[] = {
    PHP_ME(CanServerWebSocketRoute, __construct, NULL, ZEND_ACC_FINAL | ZEND_ACC_PUBLIC)
    PHP_ME(CanServerWebSocketRoute, setTimeout,  NULL, ZEND_ACC_PUBLIC)
    PHP_ME(CanServerWebSocketRoute, onHandshake, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(CanServerWebSocketRoute, onMessage,   NULL, ZEND_ACC_PUBLIC)
    PHP_ME(CanServerWebSocketRoute, close,       NULL, ZEND_ACC_PUBLIC)
    PHP_ME(CanServerWebSocketRoute, onClose,     NULL, ZEND_ACC_PUBLIC)
    {NULL, NULL, NULL}
};

static void server_websocket_route_init(TSRMLS_D)
{
    memcpy(&server_websocket_route_obj_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    server_websocket_route_obj_handlers.clone_obj = NULL;

    // class \Can\Server\WebSocketRoute extends \Can\Server\Route
    PHP_CAN_REGISTER_SUBCLASS(
        &ce_can_server_websocket_route,
        ce_can_server_route,
        ZEND_NS_NAME(PHP_CAN_SERVER_NS, "WebSocketRoute"),
        server_websocket_route_ctor,
        server_websocket_route_methods
    );
}

PHP_MINIT_FUNCTION(can_server_websocket_route)
{
    server_websocket_route_init(TSRMLS_C);
    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(can_server_websocket_route)
{
    return SUCCESS;
}

PHP_RINIT_FUNCTION(can_server_websocket_route)
{
    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(can_server_websocket_route)
{
    return SUCCESS;
}
