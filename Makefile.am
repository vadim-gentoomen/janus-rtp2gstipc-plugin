ACLOCAL_AMFLAGS = -I m4
JANUS_PATH=$(exec_prefix)
CFLAGS = $(shell pkg-config --cflags glib-2.0 gstreamer-1.0) -I$(JANUS_PATH)/include/janus
LIBS = $(shell pkg-config --libs glib-2.0 gstreamer-1.0) -lgstbase-1.0 -lgstapp-1.0

lib_LTLIBRARIES = libjanus_rtp2gstipc.la
libjanus_rtp2gstipc_la_SOURCES = janus_rtp2gstipc.c rtp2gst.c
libjanus_rtp2gstipc_la_LDFLAGS = -version-info 0:0:0 $(shell pkg-config --libs glib-2.0) -L$(JANUS_PATH)/lib
libdir = $(exec_prefix)/lib/janus/plugins
