sbindir ?= $(PREFIX)/sbin
libdir ?= $(PREFIX)/lib/tgt

TGTD_OBJS += $(addprefix iscsi/, conn.o param.o session.o \
		iscsid.o target.o chap.o sha1.o md5.o transport.o iscsi_tcp.o \
		isns.o)

ifneq ($(CEPH_RBD),)
MODULES += bs_rbd.so
endif

ifneq ($(GLFS_BD),)
MODULES += bs_glfs.so
endif

ifneq ($(SD_NOTIFY),)
CFLAGS += -DUSE_SYSTEMD
endif

CFLAGS += -DUSE_SIGNALFD
CFLAGS += -DUSE_TIMERFD
CFLAGS += -DUSE_EVENTFD
TGTD_OBJS += bs_aio.o
LIBS += -laio

ifneq ($(ISCSI_RDMA),)
TGTD_OBJS += iscsi/iser.o iscsi/iser_text.o
LIBS += -libverbs -lrdmacm
endif

INCLUDES += -I.

CFLAGS += -D_GNU_SOURCE
CFLAGS += $(INCLUDES)
CFLAGS += -O3 -flto=8
CFLAGS += -Wall -Wstrict-prototypes -Werror
CFLAGS += -DTGT_VERSION=\"$(VERSION)$(EXTRAVERSION)\"
CFLAGS += -DBSDIR=\"$(DESTDIR)$(libdir)/backing-store\"

LIBS += -lpthread -ldl

ifneq ($(SD_NOTIFY),)
LIBS += -lsystemd
endif

PROGRAMS += tgtd tgtadm tgtimg
TGTD_OBJS += tgtd.o mgmt.o target.o scsi.o log.o driver.o util.o work.o \
		concat_buf.o parser.o spc.o sbc.o mmc.o osd.o scc.o smc.o \
		ssc.o libssc.o bs_rdwr.o bs_ssc.o \
		bs_null.o bs_sg.o bs.o bs_sheepdog.o

TGTD_DEP = $(TGTD_OBJS:.o=.d)

LDFLAGS = $(CFLAGS) -Wl,-E,-rpath=$(libdir)

.PHONY:all
all: $(PROGRAMS) $(MODULES)

tgtd: $(TGTD_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS) $(LIBS)

-include $(TGTD_DEP)

TGTADM_OBJS = tgtadm.o concat_buf.o
TGTADM_DEP = $(TGTADM_OBJS:.o=.d)

tgtadm: $(TGTADM_OBJS)
	$(CC) $^ -o $@ $(CFLAGS)

-include $(TGTADM_DEP)

TGTIMG_OBJS = tgtimg.o libssc.o
TGTIMG_DEP = $(TGTIMG_OBJS:.o=.d)

tgtimg: $(TGTIMG_OBJS)
	$(CC) $^ -o $@ $(CFLAGS)

-include $(TGTIMG_DEP)

%.o: %.c
	$(CC) -c $(CFLAGS) $*.c -o $*.o
	@$(CC) -MM $(CFLAGS) -MF $*.d -MT $*.o $*.c

%.so: %.c
	$(CC) -shared $(CFLAGS) $*.c -o $*.so

bs_rbd.so: bs_rbd.c
	$(CC) -shared $(CFLAGS) bs_rbd.c -o bs_rbd.so -lrados -lrbd

bs_glfs.so: bs_glfs.c
	$(CC) -I/usr/include/glusterfs/api -shared $(CFLAGS) bs_glfs.c -o bs_glfs.so -lgfapi

.PHONY: install
install: $(PROGRAMS) $(MODULES)
	install -d -m 755 $(DESTDIR)$(sbindir)
	install -m 755 $(PROGRAMS) $(DESTDIR)$(sbindir)
ifneq ($(MODULES),)
	install -d -m 755 $(DESTDIR)$(libdir)/backing-store
	install -m 755 $(MODULES) $(DESTDIR)$(libdir)/backing-store
endif

.PHONY: clean
clean:
	rm -f *.[od] *.so $(PROGRAMS) iscsi/*.[od] ibmvio/*.[od] fc/*.[od]
