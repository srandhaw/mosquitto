include ../../config.mk

CFLAGS_FINAL=${CFLAGS} -fsanitize=address -fno-omit-frame-pointer -I.. -I../../lib -I../.. -I../deps

.PHONY: all clean reallyclean

all : mosquitto_db_dump

mosquitto_db_dump : db_dump.o
	${CROSS_COMPILE}${CC} $^ -o $@ ${LDFLAGS} -fsanitize=address -fno-omit-frame-pointer ${LIBS}

db_dump.o : db_dump.c ../persist.h
	${CROSS_COMPILE}${CC} $(CFLAGS_FINAL) -c $< -o $@

clean : 
	-rm -f *.o mosquitto_db_dump
