#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include "libcurl_wrapper.h"

void cb(char *data, size_t data_len, int status) {
    printf("\nstatus=%d\n", status);
    printf("data=%s\n", data);
}

int main() {
    cwp_ctx *ctx = cwp_ctx_init();
    int i;
    char *url = "http://www.baidu.com";
    time_t s = time(NULL);
    for (i = 0; i < 200; i++) {
	    cwp_new_post(ctx, url, strlen(url), "fuck you", strlen("fuck you"), cb);
    }
    time_t t = time(NULL);
    printf("fin---%zu\n", t - s);

    sleep(1000);

    return 0;
}
