bin_PROGRAMS = famfsd

AM_CPPFLAGS = -I$(top_srcdir)/meta/src \
              -I$(top_srcdir)/meta/src/uthash \
              -I$(top_srcdir)/meta/src/Mlog2 \
              -I$(top_srcdir)/common/src \
	      -I$(top_srcdir)/userspace-rcu/include \
	      -I$(top_srcdir)/userspace-rcu/src \
              -I$(top_srcdir)/node/isa-l/include \
              -I$(top_srcdir)/node/isa-l/erasure_code/erasure_code

AM_CFLAGS = -Wall $(LIBFABRIC_CFLAGS) $(MPI_CFLAGS) -mcx16

famfsd_SOURCES   = arraylist.c \
                   arraylist.h \
                   unifycr_cmd_handler.c \
                   unifycr_cmd_handler.h \
                   unifycr_const.c \
                   unifycr_const.h \
                   unifycr_debug.c \
                   unifycr_debug.h \
                   unifycr_init.c \
                   unifycr_init.h \
                   unifycr_metadata.c \
                   unifycr_metadata.h \
                   unifycr_request_manager.c \
                   unifycr_request_manager.h \
                   unifycr_service_manager.c \
                   unifycr_service_manager.h \
                   unifycr_sock.c \
                   unifycr_sock.h \
                   unifycr_global.h \
                   unifycr_setup.h \
                   lf_client.c \
                   lf_client.h \
                   log.h \
		   f_allocator.c \
		   f_allocator.h \
		   f_layout_ctl.c \
		   f_layout_ctl.h \
	           f_helper.h \
        	   f_helper.c \
		   f_encode_recovery.h \
		   f_encode_recovery.c \
		   f_recovery.h \
		   f_recovery.c \
                   f_ec.h \
                   f_ec.c

# Preprocessor options for inlined code from common/src/famfs_bbitops.h
famfsd_CPPFLAGS = -mbmi2 -fms-extensions \
		    -funroll-loops --param max-completely-peel-times=64 $(AM_CPPFLAGS)

famfsd_LDFLAGS = -static $(LIBFABRIC_LIBS) $(MPI_CLDFLAGS) \
                   -L$(top_builddir)/node/isa-l/.libs

famfsd_LDADD = $(top_builddir)/userspace-rcu/src/liburcu.la \
                 $(top_builddir)/meta/src/libmdhim.la \
                 $(top_builddir)/common/src/libfamfs_common.la \
		 $(top_builddir)/common/src/libfamfs_meta.la \
                 $(LEVELDB_LIBS) \
		 -lurcu-qsbr -lurcu-cds -latomic -lisal

CLEANFILES = $(bin_PROGRAMS)
