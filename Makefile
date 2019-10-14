CC=gcc
CFLAGS=-Wall -Werror
DEBUG := 0
LIBS=-lcrypto -lssl

.PHONY: clean

SOURCE_FILES=buffer.c cache.c connection.c html.c http.c main.c parse.c tex.c utils.c
OBJ_FILES=$(SOURCE_FILES:.c=.o)

DEP_FILES := \
	buffer.h \
	cache.h \
	connection.h \
	html.h \
	http.h \
	parse.h \
	tex.h \
	types.h \
	utils.h \
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
