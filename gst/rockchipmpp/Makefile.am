plugin_LTLIBRARIES = libgstrockchipmpp.la

libgstrockchipmpp_la_SOURCES =			\
	gstmppobject.c				\
	gstmppvideoenc.c			\
	gstmpph264enc.c				\
	gstmppjpegenc.c				\
	gstmppdecbufferpool.c			\
	gstmppbufferpool.c			\
	gstmppallocator.c			\
	gstmppvideodec.c			\
	gstmpp.c				\
        $(NULL)

libgstrockchipmpp_la_CFLAGS =			\
        $(GST_CFLAGS)                           \
        $(GST_BASE_CFLAGS)                      \
        $(GST_PLUGINS_BASE_CFLAGS)              \
        $(GST_ALLOCATORS_CFLAGS)                \
        $(GST_VIDEO_CFLAGS)                     \
	$(ROCKCHIP_MPP_CFLAGS)                  \
        $(NULL)

libgstrockchipmpp_la_LIBADD =			\
	$(GST_LIBS)				\
	$(GST_BASE_LIBS)			\
	$(GST_PLUGINS_BASE_LIBS)		\
	$(GST_VIDEO_LIBS)                       \
	$(GST_ALLOCATORS_LIBS)                  \
	$(ROCKCHIP_MPP_LIBS)			\
	$(NULL)

libgstrockchipmpp_la_LDFLAGS =			\
	$(GST_PLUGIN_LDFLAGS)			\
	$(NULL)

libgstrockchipmpp_la_LIBTOOLFLAGS =		\
	$(GST_PLUGIN_LIBTOOLFLAGS)		\
	$(NULL)

noinst_HEADERS =				\
	gstmppvideoenc.h			\
	gstmpph264enc.h				\
	gstmppjpegenc.h				\
	gstmppdecbufferpool.h			\
	gstmppallocator.h			\
	gstmppvideodec.h			\
	$(NULL)
