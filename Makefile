

# find the OS
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

# Compile flags for linux / osx
ifeq ($(uname_S),Linux)
	SHOBJ_CFLAGS ?= -W -fPIC -Wall -fno-common -g -ggdb -std=c99 -D_XOPEN_SOURCE=600 -O0 -pthread -fvisibility=hidden
	SHOBJ_LDFLAGS ?= -shared -fvisibility=hidden
else
	SHOBJ_CFLAGS ?= -W -fPIC -Wall -dynamic -fno-common -g -ggdb -std=c99 -O0 -pthread -fvisibility=hidden
	SHOBJ_LDFLAGS ?= -bundle -undefined dynamic_lookup -fvisibility=hidden
endif

# OS X 11.x doesn't have /usr/lib/libSystem.dylib and needs an explicit setting.
ifeq ($(uname_S),Darwin)
ifeq ("$(wildcard /usr/lib/libSystem.dylib)","")
APPLE_LIBS = -L /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib -lsystem
endif
endif

.SUFFIXES: .c .so .xo .o

# hiredis
HIREDIS_DIR = ${SOURCEDIR}/hiredis
HIREDIS_CFLAGS ?= -I$(SOURCEDIR) -I$(HIREDIS_DIR) -I$(HIREDIS_DIR)/adapters

# threadpool
THREADPOOL_DIR = ${SOURCEDIR}/threadpool
THREADPOOL_CFLAGS ?= -I$(THREADPOOL_DIR)

# dep
DEP_DIR = ${SOURCEDIR}/dep
DEP_CFLAGS ?= -I$(DEP_DIR)

SOURCEDIR=$(shell pwd -P)
CC_SOURCES = $(wildcard $(SOURCEDIR)/*.c) $(wildcard $(DEP_DIR)/*.c) $(wildcard $(THREADPOOL_DIR)/thpool.c)
CC_OBJECTS = $(sort $(patsubst %.c, %.o, $(CC_SOURCES)))


all: init redisxslot.so

init:
	@git submodule init
	@git submodule update
#	@make -C $(HIREDIS_DIR)

${SOURCEDIR}/module.o: ${SOURCEDIR}/module.c
	$(CC) -c -o $@ $(SHOBJ_CFLAGS) $(THREADPOOL_CFLAGS) $(DEP_CFLAGS) $<

${SOURCEDIR}/redisxslot.o: ${SOURCEDIR}/redisxslot.c
	$(CC) -c -o $@ $(SHOBJ_CFLAGS) $(THREADPOOL_CFLAGS) $(DEP_CFLAGS) $<

%.o: %.c
	$(CC) -c -o $@ $(SHOBJ_CFLAGS) $<


redisxslot.so: $(CC_OBJECTS)
	$(LD) -o $@ $(CC_OBJECTS) \
	$(SHOBJ_LDFLAGS) $(APPLE_LIBS) \
	-lc

clean:
	cd $(SOURCEDIR) && rm -rvf *.xo *.so *.o *.a
	cd $(SOURCEDIR)/dep && rm -rvf *.xo *.so *.o *.a
	cd $(THREADPOOL_DIR) && rm -rvf *.xo *.so *.o *.a
#	cd $(HIREDIS_DIR) && make clean
