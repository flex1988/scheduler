CFLAGS?= -std=c99 -pedantic $(OPTIMIZATION) -Wall -W 
CCOPT= $(CFLAGS) $(CCLINK)

DEBUG?= -g -rdynamic -ggdb 

OBJ:ae.o anet.o fool.o

PRGNAME = fool-server

ae.o:ae.c ae.h zmalloc.h config.h ae_kqueue.c
ae_kqueue.o:ae_kqueue.c
ae_select.o:ae_select.c
anet.o:anet.c fmacros.h anet.h
fool.o:fool.c fmacros.h config.h fool.h ae.h anet.h zmalloc.h

fool-server:$(OBJ)
	$(CC) -o $(PRGNAME) $(CCOPT) $(DEBUG) $(OBJ)

clean: 
	rm *.o
