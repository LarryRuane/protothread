CC = gcc
LD = gcc -pthread
AR = ar
CFLAGS = -g -Wall -m64

prefix = /usr/local
bindir = $(prefix)/bin
libdir = $(prefix)/lib
includedir = $(prefix)/include
DESTDIR =

ifeq ($(DEBUG),yes)
	CFLAGS += -DPT_DEBUG=1
else
ifeq ($(DEBUG),no)
	CFLAGS += -DPT_DEBUG=0
endif
endif

SRCDIR = .

TARGETS = pttest libprotothread.a

LIB_OBJS = \
       protothread.o \

OBJS = $(LIB_OBJS) \
       protothread_test.o

.PHONY: all clean install

all: $(TARGETS)

define OBJ_template
$(1).o: $(SRCDIR)/$(1).c $(wildcard $(SRCDIR)/$(1).h) $(SRCDIR)/protothread.h
	$(CC) $(CFLAGS) -c -o $$@ -I$(SRCDIR)/ $$<
endef

$(foreach x,$(basename $(OBJS)),$(eval $(call OBJ_template,$(x))))

pttest:	$(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

libprotothread.a: $(LIB_OBJS)
	$(AR) rcs $@ $^

clean:
	rm -f *.o $(TARGETS)

install:
	cp -vf pttest $(DESTDIR)$(bindir)/pttest$(SUFFIX)
	cp -vf libprotothread.a $(DESTDIR)$(libdir)/libprotothread$(SUFFIX).a
	cp -vf $(SRCDIR)/*.h $(DESTDIR)$(includedir)/
