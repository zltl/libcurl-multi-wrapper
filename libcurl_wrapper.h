#ifndef _LIBCURL_WRAPPER_H
#define _LIBCURL_WRAPPER_H

#define CWP_IN_BUFFER_SIZE 1024
#define CWP_DEFAULT_HANDLES_NUM 1

typedef struct _cwp_handle cwp_handle;
typedef struct _cwp_ctx cwp_ctx;

// status see here: https://curl.haxx.se/libcurl/c/libcurl-errors.html
typedef void (*cwp_callback)(char *data, size_t data_len, int status);

cwp_ctx *cwp_ctx_init();

void cwp_ctx_stop(cwp_ctx *ctx);

cwp_handle *cwp_new_post(cwp_ctx *ctx, char *url, size_t url_len,
                         char *out_data, size_t out_data_len,
                         cwp_callback cb);

#endif
