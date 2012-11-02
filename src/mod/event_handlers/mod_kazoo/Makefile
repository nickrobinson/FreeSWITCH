BASE=../../../..

LOCAL_SOURCES=ei_helpers.c kazoo_request.c kazoo_bindings.c
LOCAL_OBJS=ei_helpers.o kazoo_request.o kazoo_bindings.o
LOCAL_CFLAGS= -I/usr/lib64/erlang/lib/erl_interface-3.7.5/include  -D_REENTRANT -I$(BASE)/libs/apr/include
LOCAL_LDFLAGS= -L/usr/lib64/erlang/lib/erl_interface-3.7.5/lib  -lei

include $(BASE)/build/modmake.rules
