uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')
OPTIMIZATION?=-O0
CFLAGS?= -std=c99 $(OPTIMIZATION) -Wall -W -g
CCOPT= $(CFLAGS) $(CCLINK) $(ARCH) $(PROF)

DEBUG?= -g -rdynamic -ggdb 

OBJ = ae.o anet.o server.o zmalloc.o sds.o dict.o adlist.o util.o skiplist.o
PRGNAME = server

ae.o:ae.c ae.h zmalloc.h config.h ae_kqueue.c
ae_kqueue.o:ae_kqueue.c
ae_select.o:ae_select.c
anet.o:anet.c fmacros.h anet.h
server.o:server.c fmacros.h config.h server.h ae.h anet.h zmalloc.h
zmalloc.o:zmalloc.c zmalloc.h
sds.o:sds.c sds.h
dict.o:dict.c dict.h
adlist.o:adlist.c adlist.h
util.o:util.c util.h
skiplist.o:skiplist.c skiplist.h 

server:$(OBJ)
	$(CC) -o $(PRGNAME) $(CCOPT) $(DEBUG) $(OBJ) 

clean: 
	rm *.o server
