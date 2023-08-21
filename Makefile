# ReidsXSlot Makefile
# Copyright (C) 2023- weedge <weege007 at gmail dot com>
# This file is released under the MIT license, see the LICENSE file

CC=gcc
# redis version >= 6.0.0
REDIS_VERSION ?= 60000

# redisxslot version
REDISXSLOT_MAJOR=$(shell grep REDISXSLOT_MAJOR redisxslot.h | awk '{print $$3}')
REDISXSLOT_MINOR=$(shell grep REDISXSLOT_MINOR redisxslot.h | awk '{print $$3}')
REDISXSLOT_PATCH=$(shell grep REDISXSLOT_PATCH redisxslot.h | awk '{print $$3}')
REDISXSLOT_SONAME=$(shell grep REDISXSLOT_SONAME redisxslot.h | awk '{print $$3}')

# RedisModulesSDK
SDK_DIR = ${SOURCEDIR}/RedisModulesSDK
SDK_CFLAGS ?= -I$(SDK_DIR) -I$(SDK_DIR)/rmutil
SDK_LDFLAGS ?= -L$(SDK_DIR)/rmutil -lrmutil

# hiredis
HIREDIS_DIR = ${SOURCEDIR}/hiredis
HIREDIS_RUNTIME_DIR ?= $(SOURCEDIR)
HIREDIS_CFLAGS ?= -I$(HIREDIS_DIR) -I$(HIREDIS_DIR)/adapters
HIREDIS_LDFLAGS ?= -L$(HIREDIS_DIR)
HIREDIS_STLIB ?= $(HIREDIS_DIR)/libhiredis.a
ifeq ($(uname_S),Darwin)
HIREDIS_DYLIB ?= $(HIREDIS_LDFLAGS) -lhiredis -rpath $(HIREDIS_RUNTIME_DIR)
else
HIREDIS_DYLIB ?= $(HIREDIS_LDFLAGS) -lhiredis -rpath=$(HIREDIS_RUNTIME_DIR)
endif

HIREDIS_LIB_FLAGS ?= $(HIREDIS_LDFLAGS) $(HIREDIS_DIR)/libhiredis.a
ifeq ($(HIREDIS_USE_DYLIB),1)
HIREDIS_LIB_FLAGS = $(HIREDIS_DYLIB)
endif

# threadpool
THREADPOOL_DIR = ${SOURCEDIR}/threadpool
THREADPOOL_CFLAGS ?= -I$(THREADPOOL_DIR)

# dep
DEP_DIR = ${SOURCEDIR}/dep
DEP_CFLAGS ?= -I$(DEP_DIR)

#set environment variable RM_INCLUDE_DIR to the location of redismodule.h
ifndef RM_INCLUDE_DIR
	RM_INCLUDE_DIR=$(SDK_DIR)
endif

# find the OS
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')
# Compile flags for linux / osx
ifeq ($(uname_S),Linux)
	SHOBJ_CFLAGS ?= -DREDIS_VERSION=$(REDIS_VERSION) -I$(RM_INCLUDE_DIR) \
					-W -fPIC -Wall -fno-common -g -ggdb -std=gnu99 -D_XOPEN_SOURCE=600 -O0 -pthread -fvisibility=hidden
	SHOBJ_LDFLAGS ?= -shared -Bsymbolic -fvisibility=hidden
else
	SHOBJ_CFLAGS ?= -DREDIS_VERSION=$(REDIS_VERSION) -I$(RM_INCLUDE_DIR) \
					-W -fPIC -Wall -dynamic -fno-common -g -ggdb -std=gnu99 -O0 -pthread -fvisibility=hidden
	SHOBJ_LDFLAGS ?= -bundle -undefined dynamic_lookup -keep_private_externs
endif

# OS X 11.x doesn't have /usr/lib/libSystem.dylib and needs an explicit setting.
ifeq ($(uname_S),Darwin)
ifeq ("$(wildcard /usr/lib/libSystem.dylib)","")
APPLE_LIBS = -L /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib -lsystem
endif
endif

.SUFFIXES: .c .so .xo .o
SOURCEDIR=$(shell pwd -P)
CC_SOURCES = $(wildcard $(SOURCEDIR)/*.c) \
	$(wildcard $(THREADPOOL_DIR)/thpool.c) \
	$(wildcard $(DEP_DIR)/*.c) 
CC_OBJECTS = $(sort $(patsubst %.c, %.o, $(CC_SOURCES)))


all: init redisxslot.so ldd_so

help:
	@echo "please choose make with below env params"
	@echo "RM_INCLUDE_DIR={redis_absolute_path}/src, include redismodule.h"
	@echo "HIREDIS_USE_DYLIB=1, linker with use hiredis.so"
	@echo "HIREDIS_USE_DYLIB=1 HIREDIS_RUNTIME_DIR=/usr/local/lib ,if pkg install hiredis, linker with HIREDIS_RUNTIME_DIR use hiredis.so"
	@echo "REDIS_VERSION=6000, default 6000(6.0.0), use 70200(7.2.0) inlcude 7.2.0+ redismodule.h to use feature api"

init:
	@git submodule init
	@git submodule update
	@make -C $(SDK_DIR)/rmutil CFLAGS="-g -fPIC -O0 -std=gnu99 -Wall -Wno-unused-function -fvisibility=hidden -I$(RM_INCLUDE_DIR)"
	@make -C $(HIREDIS_DIR) OPTIMIZATION="-O0" CFLAGS="-fvisibility=hidden" LDFLAGS="-fvisibility=hidden"
ifeq ($(HIREDIS_USE_DYLIB),1)
	@rm -rvf $(HIREDIS_RUNTIME_DIR)/libhiredis.so.1.1.0
	@ln -s $(HIREDIS_DIR)/libhiredis.so $(HIREDIS_RUNTIME_DIR)/libhiredis.so.1.1.0
endif

${SOURCEDIR}/module.o: ${SOURCEDIR}/module.c
	$(CC) -c -o $@ $(SHOBJ_CFLAGS) $(DEP_CFLAGS) \
	$(THREADPOOL_CFLAGS) \
	$(HIREDIS_CFLAGS) \
	$(SDK_CFLAGS) \
	$< 

${SOURCEDIR}/redisxslot.o: ${SOURCEDIR}/redisxslot.c
	$(CC) -c -o $@ $(SHOBJ_CFLAGS) $(DEP_CFLAGS) \
	$(THREADPOOL_CFLAGS) \
	$(HIREDIS_CFLAGS) \
	$(SDK_CFLAGS) \
	$<

%.o: %.c
	$(CC) -c -o $@ $(SHOBJ_CFLAGS) $<

redisxslot.so: $(CC_OBJECTS)
	$(LD) -o $@ $(CC_OBJECTS) \
	$(SHOBJ_LDFLAGS) \
	$(SDK_LDFLAGS) \
	$(HIREDIS_LIB_FLAGS) \
	$(APPLE_LIBS) \
	-lc

ldd_so:
	@rm -rvf $(SOURCEDIR)/redisxslot.so.$(REDISXSLOT_SONAME)
ifeq ($(uname_S),Darwin)
	@otool -L $(SOURCEDIR)/redisxslot.so
	@sudo ln -s $(SOURCEDIR)/redisxslot.so $(SOURCEDIR)/redisxslot.dylib.$(REDISXSLOT_SONAME)
else
	@ldd $(SOURCEDIR)/redisxslot.so
	@sudo ln -s $(SOURCEDIR)/redisxslot.so $(SOURCEDIR)/redisxslot.so.$(REDISXSLOT_SONAME)
endif

clean:
	cd $(SOURCEDIR) && rm -rvf *.xo *.so *.o *.a
	cd $(SOURCEDIR)/dep && rm -rvf *.xo *.so *.o *.a
	cd $(THREADPOOL_DIR) && rm -rvf *.xo *.so *.o *.a
	cd $(HIREDIS_DIR) && make clean 
	cd $(SDK_DIR)/rmutil && make clean 
	rm -rvf $(HIREDIS_RUNTIME_DIR)/libhiredis.so.1.1.0
	rm -rvf $(SOURCEDIR)/redisxslot.so.$(REDISXSLOT_SONAME)
