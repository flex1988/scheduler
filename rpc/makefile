CC = gcc
CFLAGS = -g -DRPC_SVC_FG
RPCGEN_FLAG = -C
all: client server
client: client.o notify_clnt.o
		$(CC) -o client client.o notify_clnt.o
server: server.o notify_svc.o 	
	  $(CC) -o server server.o notify_svc.o
server.o: server.c notify.h
		$(CC) $(CFLAGS) -c server.c
client.o: client.c notify.h
		$(CC) $(CFLAGS) -c client.c 
notify_svc.o: notify_svc.c notify.h
		$(CC) $(CFLAGS) -c notify_svc.c 
notify_clnt.o: notify_clnt.c notify.h
		$(CC) $(CFLAGS) -c notify_clnt.c 
notify_clnt.c notify_svc.c notify.h: notify.x
		rpcgen $(RPCGEN_FLAG) notify.x
clean:
		rm -f client notify_client.o server notify_server.o notify_clnt.* notify_svc.* notify.h
