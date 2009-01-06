CFLAGS = -g -Wall

OBJS = \
       protothread_test.o \
       protothread_lock.o \
       protothread_sem.o \
       protothread.o \

pttest:	$(OBJS) protothread.h
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(OBJS): protothread.h

clean:
	rm -f *.o
