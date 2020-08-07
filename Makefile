CC=gcc
CFLAGS=-Wall -Werror
DEBUG := 0
LIBS=-lcrypto -lssl

.PHONY: clean

SOURCE_FILES=buffer.c cache.c connection.c hash_bucket.c html.c http.c main.c parse.c string_utils.c tex.c utils.c
OBJ_FILES=$(SOURCE_FILES:.c=.o)

DEP_FILES := \
	buffer.h \
	cache.h \
	connection.h \
	hash_bucket.h \
	html.h \
	http.h \
	parse.h \
	string_utils.h \
	tex.h \
	types.h \
	utils.h \
	wikigrab.h

wikigrab: $(OBJ_FILES) $(DEP_FILES)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

$(OBJ_FILES): $(SOURCE_FILES) $(DEP_FILES)
ifeq ($(DEBUG),1)
	$(CC) $(CFLAGS) -O2 -g -DDEBUG -c $^
else
	$(CC) $(CFLAGS) -O2 -c $^
endif


clean:
	rm *.o
