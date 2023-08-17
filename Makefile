# find the OS
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

# Compile flags for linux / osx
ifeq ($(uname_S),Linux)
	SHOBJ_CFLAGS ?= -W -fPIC -Wall -fno-common -g -ggdb -std=c99 -D_XOPEN_SOURCE=600 -O0 -pthread -fvisibility=hidden
	SHOBJ_LDFLAGS ?= -shared -fvisibility=hidden
else
	SHOBJ_CFLAGS ?= -W -fPIC -Wall -dynamic -fno-common -g -ggdb -std=c99 -O0 -pthread -fvisibility=hidden
	SHOBJ_LDFLAGS ?= -bundle -undefined dynamic_lookup -keep_private_externs
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
HIREDIS_RUNTIME_DIR ?= $(SOURCEDIR)
HIREDIS_CFLAGS ?= -I$(HIREDIS_DIR) -I$(HIREDIS_DIR)/adapters
HIREDIS_LDFLAGS ?= -L$(HIREDIS_DIR)
HIREDIS_STLIB ?= $(HIREDIS_DIR)/libhiredis.a
ifeq ($(uname_S),Darwin)
HIREDIS_DYLIB ?= $(HIREDIS_LDFLAGS) -lhiredis -rpath $(SOURCEDIR)
else
HIREDIS_DYLIB ?= $(HIREDIS_LDFLAGS) -lhiredis -rpath=$(SOURCEDIR)
endif

ifeq ($(uname_S),Darwin)
HIREDIS_LIB_FLAGS ?= $(HIREDIS_STLIB)
else
HIREDIS_LIB_FLAGS ?= $(HIREDIS_STLIB) $(HIREDIS_CFLAGS)
endif
ifeq ($(HIREDIS_USE_DYLIB),1)
HIREDIS_LIB_FLAGS = $(HIREDIS_DYLIB)
endif

# threadpool
THREADPOOL_DIR = ${SOURCEDIR}/threadpool
THREADPOOL_CFLAGS ?= -I$(THREADPOOL_DIR)

# dep
DEP_DIR = ${SOURCEDIR}/dep
DEP_CFLAGS ?= -I$(DEP_DIR)

SOURCEDIR=$(shell pwd -P)
CC_SOURCES = $(wildcard $(SOURCEDIR)/*.c) \
	$(wildcard $(THREADPOOL_DIR)/thpool.c) \
	$(wildcard $(DEP_DIR)/*.c) 
CC_OBJECTS = $(sort $(patsubst %.c, %.o, $(CC_SOURCES)))


all: init redisxslot.so ldd_so

help:
	@echo "make HIREDIS_USE_DYLIB=1 , linker with use hiredis.so"
	@echo "make HIREDIS_USE_DYLIB=1 HIREDIS_RUNTIME_DIR=/usr/local//usr/local/lib ,if pkg install hiredis, linker with HIREDIS_RUNTIME_DIR use hiredis.so"

init:
	@git submodule init
	@git submodule update
	@make -C $(HIREDIS_DIR) CFLAGS="-fvisibility=hidden" LDFLAGS="-fvisibility=hidden"
ifeq ($(HIREDIS_USE_DYLIB),1)
	@rm -rvf $(SOURCEDIR)/libhiredis.so.1.1.0
	@ln -s $(HIREDIS_DIR)/libhiredis.so $(SOURCEDIR)/libhiredis.so.1.1.0
endif

${SOURCEDIR}/module.o: ${SOURCEDIR}/module.c
	$(CC) -c -o $@ $(SHOBJ_CFLAGS) $(DEP_CFLAGS) $(THREADPOOL_CFLAGS) $(HIREDIS_CFLAGS) $< 

${SOURCEDIR}/redisxslot.o: ${SOURCEDIR}/redisxslot.c
	$(CC) -c -o $@ $(SHOBJ_CFLAGS) $(DEP_CFLAGS) $(THREADPOOL_CFLAGS) $(HIREDIS_CFLAGS) $<

%.o: %.c
	$(CC) -c -o $@ $(SHOBJ_CFLAGS) $<

redisxslot.so: $(CC_OBJECTS)
	$(LD) -o $@ ${HIREDIS_LIB_FLAGS} $(CC_OBJECTS) \
	$(SHOBJ_LDFLAGS) \
	$(APPLE_LIBS) \
	-lc

ldd_so:
ifeq ($(uname_S),Darwin)
	@otool -L $(SOURCEDIR)/redisxslot.so
else
	@ldd $(SOURCEDIR)/redisxslot.so
endif

clean:
	cd $(SOURCEDIR) && rm -rvf *.xo *.so *.o *.a
	cd $(SOURCEDIR)/dep && rm -rvf *.xo *.so *.o *.a
	cd $(THREADPOOL_DIR) && rm -rvf *.xo *.so *.o *.a
	cd $(HIREDIS_DIR) && make clean 
	rm -rvf $(SOURCEDIR)/libhiredis.so.1.1.0
