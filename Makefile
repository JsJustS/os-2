TARGET = proxy-executable

SRCS = proxy.h proxy.c main.c libs/cache.h libs/cache.c libs/sync.h libs/sync.c

LIB_DIR = libs

CC=gcc
RM=rm
CFLAGS= -g -Wall
LIBS=-lpthread #./llhttp/build/libllhttp.a -lpthread
INCLUDE_DIR="." #./llhttp/build/

all: ${TARGET}

${TARGET}: ${SRCS} #libllhttp
	${CC} ${CFLAGS} -I${INCLUDE_DIR} -iquote${LIB_DIR} ${SRCS} ${LIBS} -o ${TARGET}

libllhttp: ./llhttp/build/libllhttp.a

./llhttp/build/libllhttp.a:
	(cd llhttp && npm ci && make)

clean:
	${RM} -f *.o ${TARGET}
