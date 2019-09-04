CC=gcc
CFLAGS=-Wall -Werror
DEBUG := 0
LIBS=-lcrypto -lssl

.PHONY: clean

SOURCE_FILES=main.c http.c parse.c connection.c buffer.c cache.c
OBJ_FILES=$(SOURCE_FILES:.c=.o)

DEP_FILES := \
	buffer.h \
	cache.h \
	connection.h \
	parse.h \
	wikigrab.h

wikigrab: $(OBJ_FILES) $(DEP_FILES)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

$(OBJ_FILES): $(SOURCE_FILES) $(DEP_FILES)
ifeq ($(DEBUG),1)
	$(CC) $(CFLAGS) -g -DDEBUG -c $^
else
	$(CC) $(CFLAGS) -c $^
endif


clean:
	rm *.o
