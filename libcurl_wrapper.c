#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <curl/curl.h>
#include <malloc.h>
#include "list.h"
#include "libcurl_wrapper.h"

struct _cwp_handle {
    struct list_head list;
    CURL *handle;
    struct curl_slist *headers;
    char *out_data;
    size_t out_data_len;
    char *in_data;
    size_t in_data_cap;
    size_t in_data_len;
    cwp_callback cb;
    int free_flag;
};

struct _cwp_ctx {
    pthread_mutex_t mutex;
    int max_handle_num;
    cwp_handle **handles;
    struct list_head free;
    struct list_head used;
    CURLM *multi_handle;
    int stop;
};

static cwp_handle *cwp_handle_new() {
    size_t size = sizeof(cwp_handle);
    cwp_handle *h = (cwp_handle *)malloc(size);
    assert(h != NULL);
    memset(h, '\0', size);
    return h;
}

static size_t cwp_write_callback(void *contents, size_t size, size_t nmemb,
                                 void *userp) {
    cwp_handle *h = (cwp_handle *)userp;
    size_t real_size = size * nmemb;
    size_t total_size = real_size + h->in_data_len;

    if (total_size + 1 > h->in_data_cap) {
        size_t resize = total_size + 1;
        h->in_data = (char *)realloc(h->in_data, resize);
        assert(h->in_data != NULL);
        h->in_data_cap = resize;
    }
    memcpy(&(h->in_data[h->in_data_len]), contents, real_size);
    h->in_data_len += real_size;
    h->in_data[h->in_data_len] = '\0';
    return real_size;
}

static void cwp_handle_post_init(cwp_handle *h, char *url, size_t url_len,
                                 char *out_data, size_t out_data_len,
                                 cwp_callback cb) {
    h->handle = curl_easy_init();
    assert(h->handle != NULL);

    curl_easy_setopt(h->handle, CURLOPT_URL, url);
    h->headers =
        curl_slist_append(h->headers, "Content-Type: application/json");
    curl_easy_setopt(h->handle, CURLOPT_HTTPHEADER, h->headers);

    h->out_data = (char *)malloc(out_data_len);
    assert(h->out_data != NULL);
    memcpy(h->out_data, out_data, out_data_len);
    h->out_data_len = out_data_len;

    h->in_data = (char *)malloc(CWP_IN_BUFFER_SIZE);
    assert(h->in_data != NULL);
    h->in_data_cap = CWP_IN_BUFFER_SIZE;
    h->in_data_len = 0;

    h->cb = cb;

    curl_easy_setopt(h->handle, CURLOPT_POSTFIELDS, h->out_data);
    curl_easy_setopt(h->handle, CURLOPT_WRITEFUNCTION, cwp_write_callback);
    curl_easy_setopt(h->handle, CURLOPT_WRITEDATA, h);
    curl_easy_setopt(h->handle, CURLOPT_PRIVATE, h);

    // DEBUG
    // curl_easy_setopt(h->handle, CURLOPT_VERBOSE, 1L);
}

static void cwp_handle_clear(cwp_ctx *ctx, cwp_handle *h) {
    list_del(&h->list);

    if (h->headers) {
        curl_slist_free_all(h->headers);
    }
    if (h->handle) {
        curl_easy_cleanup(h->handle);
    }
    if (h->out_data) {
        free(h->out_data);
        h->out_data_len = 0;
    }
    if (h->in_data) {
        free(h->in_data);
        h->in_data_len = 0;
        h->in_data_cap = 0;
    }

    if (h->free_flag) {
        free(h);
        return;
    }

    h->cb = NULL;

    list_add(&h->list, &ctx->free);
    curl_multi_remove_handle(ctx->multi_handle, h);
}

static void cwp_ctx_lock(cwp_ctx *ctx) { pthread_mutex_lock(&ctx->mutex); }

static void cwp_ctx_unlock(cwp_ctx *ctx) {
    pthread_mutex_unlock(&ctx->mutex);
}

static void *cwp_perform_thread(void *arg) {
    cwp_ctx *ctx = (cwp_ctx *)arg;

    CURLMsg *msg;
    int msgs_left;

    int still_running = 1;

    while (1) {
        struct timeval timeout;
        int rc;
        CURLMcode mc;

        int is_used_list_empty = 0;

        int ctx_stop = 0;
        cwp_ctx_lock(ctx);
        ctx_stop = ctx->stop;
        cwp_ctx_unlock(ctx);
        if (ctx_stop) {
            return NULL;
        }

        cwp_ctx_lock(ctx);
        is_used_list_empty = list_empty(&ctx->used);
        cwp_ctx_unlock(ctx);

        if (is_used_list_empty) {  // sleep and skip
            struct timespec tsp;
            tsp.tv_sec = 0;
            tsp.tv_nsec = 100000000L;  // 0.1s
            nanosleep(&tsp, &tsp);
            continue;
        }

        fd_set fdread;
        fd_set fdwrite;
        fd_set fdexcep;
        int maxfd = -1;

        long curl_timeo = -1;
        FD_ZERO(&fdread);
        FD_ZERO(&fdwrite);
        FD_ZERO(&fdexcep);

        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        cwp_ctx_lock(ctx);
        curl_multi_timeout(ctx->multi_handle, &curl_timeo);
        cwp_ctx_unlock(ctx);
        if (curl_timeo >= 0) {
            timeout.tv_sec = curl_timeo / 1000;
            if (timeout.tv_sec > 1)
                timeout.tv_sec = 1;
            else
                timeout.tv_usec = (curl_timeo % 1000) * 1000;
        }

        cwp_ctx_lock(ctx);
        mc = curl_multi_fdset(ctx->multi_handle, &fdread, &fdwrite,
                              &fdexcep, &maxfd);
        cwp_ctx_unlock(ctx);

        if (mc != CURLM_OK) {
            fprintf(stderr, "curl_multi_fdset() failed, code %d.\n", mc);
            assert(0);
        }

        if (maxfd == -1) {
            /* Portable sleep for platforms other than Windows. */
            struct timeval wait = {0, 100 * 1000}; /* 100ms */
            rc = select(0, NULL, NULL, NULL, &wait);
        } else {
            rc = select(maxfd + 1, &fdread, &fdwrite, &fdexcep, &timeout);
        }

        switch (rc) {
            case -1:
                /* select error */
                fprintf(stderr, "select error code=%d\n", rc);
            case 0:
            /* timeout */
            default:
                cwp_ctx_lock(ctx);
                curl_multi_perform(ctx->multi_handle, &still_running);
                cwp_ctx_unlock(ctx);
        }

        cwp_ctx_lock(ctx);
        msg = curl_multi_info_read(ctx->multi_handle, &msgs_left);
        cwp_ctx_unlock(ctx);
        while (msg) {
            if (msg->msg == CURLMSG_DONE) {
                cwp_handle *h = NULL;
                curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &h);
                if (h->cb)
                    h->cb(h->in_data, h->in_data_len, msg->data.result);

                cwp_ctx_lock(ctx);
                cwp_handle_clear(ctx, h);
                cwp_ctx_unlock(ctx);
            }
            cwp_ctx_lock(ctx);
            msg = curl_multi_info_read(ctx->multi_handle, &msgs_left);
            cwp_ctx_unlock(ctx);
        }
    }

    return NULL;
}

cwp_ctx *cwp_ctx_init() {
    cwp_ctx *ctx = (cwp_ctx *)malloc(sizeof(cwp_ctx));
    assert(ctx);
    pthread_mutex_init(&ctx->mutex, NULL);
    ctx->max_handle_num = CWP_DEFAULT_HANDLES_NUM;
    ctx->handles =
        (cwp_handle **)malloc(sizeof(cwp_handle **) * ctx->max_handle_num);
    assert(ctx->handles);
    int i;
    INIT_LIST_HEAD(&ctx->free);
    INIT_LIST_HEAD(&ctx->used);

    for (i = 0; i < ctx->max_handle_num; i++) {
        ctx->handles[i] = cwp_handle_new();
        list_add(&ctx->handles[i]->list, &ctx->free);
    }
    int rc = curl_global_init(CURL_GLOBAL_ALL);
    assert(rc == 0);
    ctx->multi_handle = curl_multi_init();
    ctx->stop = 0;

    pthread_t t;
    int s = pthread_create(&t, NULL, cwp_perform_thread, ctx);
    assert(s == 0);
    pthread_detach(t);
    return ctx;
}

void cwp_ctx_stop(cwp_ctx *ctx) {
    cwp_ctx_lock(ctx);
    ctx->stop = 1;
    struct list_head *pos;
    list_for_each(pos, &ctx->used) {
        cwp_handle_clear(ctx, list_entry(pos, cwp_handle, list));
    }
    free(ctx->handles);
    curl_multi_cleanup(ctx->multi_handle);
    cwp_ctx_unlock(ctx);
}

cwp_handle *cwp_new_post(cwp_ctx *ctx, char *url, size_t url_len,
                         char *out_data, size_t out_data_len,
                         cwp_callback cb) {
    cwp_ctx_lock(ctx);

    cwp_handle *h = NULL;
    if (list_empty(&ctx->free)) {
        h = cwp_handle_new();
        h->free_flag = 1;
    } else {
        h = list_entry(ctx->free.next, cwp_handle, list);
        list_del(&h->list);
    }
    cwp_handle_post_init(h, url, url_len, out_data, out_data_len, cb);
    curl_multi_add_handle(ctx->multi_handle, h->handle);
    list_add(&h->list, &ctx->used);

    cwp_ctx_unlock(ctx);
    
    return h;
}

