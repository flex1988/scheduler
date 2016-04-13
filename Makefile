uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')
OPTIMIZATION?=-O2
CFLAGS?= -std=c99 -pedantic $(OPTIMIZATION) -Wall -W 
CCOPT= $(CFLAGS) $(CCLINK) $(ARCH) $(PROF)

DEBUG?= -g -rdynamic -ggdb 

OBJ = ae.o anet.o fool.o zmalloc.o sds.o dict.o adlist.o util.o 
PRGNAME = fool-server

ae.o:ae.c ae.h zmalloc.h config.h ae_kqueue.c
ae_kqueue.o:ae_kqueue.c
ae_select.o:ae_select.c
anet.o:anet.c fmacros.h anet.h
fool.o:fool.c fmacros.h config.h fool.h ae.h anet.h zmalloc.h
zmalloc.o:zmalloc.c zmalloc.h
sds.o:sds.c sds.h
dict.o:dict.c dict.h
adlist.o:adlist.c adlist.h
util.o:util.c util.h

fool-server:$(OBJ)
	$(CC) -o $(PRGNAME) $(CCOPT) $(DEBUG) $(OBJ)

clean: 
	rm *.o fool-server
