all: libcurl_wrapper.c  libcurl_wrapper.h  list.h  main.c 
	gcc *.c -o example -lpthread -lcurl -Wall -g

test: all
	./example
	

.PHONY: clean

clean:
	rm -rf *.o example
