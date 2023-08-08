

# find the OS
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

# Compile flags for linux / osx
ifeq ($(uname_S),Linux)
	SHOBJ_CFLAGS ?= -W -Wall -fno-common -g -ggdb -std=c99 -O2
	SHOBJ_LDFLAGS ?= -shared
else
	SHOBJ_CFLAGS ?= -W -Wall -dynamic -fno-common -g -ggdb -std=c99 -O2
	SHOBJ_LDFLAGS ?= -bundle -undefined dynamic_lookup
endif

# OS X 11.x doesn't have /usr/lib/libSystem.dylib and needs an explicit setting.
ifeq ($(uname_S),Darwin)
ifeq ("$(wildcard /usr/lib/libSystem.dylib)","")
LIBS = -L /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib -lsystem
endif
endif

.SUFFIXES: .c .so .xo .o

SOURCEDIR=$(shell pwd -P)
CC_SOURCES = $(wildcard $(SOURCEDIR)/*.c) $(wildcard $(SOURCEDIR)/dep/*.c)
CC_OBJECTS = $(sort $(patsubst %.c, %.o, $(CC_SOURCES)))

# hiredis
HIREDIS_DIR = ${SOURCEDIR}/hiredis
HIREDIS_CFLAGS ?= -I$(SOURCEDIR) -I$(HIREDIS_DIR)
HIREDIS_LDFLAGS ?= -L $(HIREDIS_DIR)
# gcc -fPIC -shared -I../../redis/src/ -I.. example-redismoduleapi.c -o example-redismoduleapi.so

all: redisxslot.so

init:
	@git submodule init
	@git submodule update
	@make -C $(HIREDIS_DIR)

%.o: %.c
	$(CC) -c -o $@ $(SHOBJ_CFLAGS) $<


redisxslot.so: $(CC_OBJECTS)
	$(LD) -o $@ $(CC_OBJECTS) $(SHOBJ_LDFLAGS) $(LIBS) -lc

clean:
	cd $(SOURCEDIR) && rm -rvf *.xo *.so *.o *.a
	cd $(SOURCEDIR)/dep && rm -rvf *.xo *.so *.o *.a
	cd $(HIREDIS_DIR) && make clean
