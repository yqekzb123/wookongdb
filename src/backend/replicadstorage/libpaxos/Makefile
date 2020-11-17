#-------------------------------------------------------------------------
#
# Makefile for libpaxos
#
# Copyright (c) 1994, Regents of the University of California
#
# src/backend/replicadstorage/libpaxos
#
#-------------------------------------------------------------------------

subdir = src/backend/replicadstorage/libpaxos
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
# override CPPFLAGS := -I$(libpq_srcdir) -I. -I$(srcdir) $(CPPFLAGS)

OBJS = acceptor.o base.o breakpoint.o bytes_buffer.o checkpoint_receiver.o checkpoint_sender.o cleaner.o commitctx.o committer.o communicate.o concurrent.o config.o cp_mgr.o crc32.o db.o dfnetwork.o event_base.o event_loop.o group.o inside_options.o instance.o ioloop.o learner.o learner_sender.o logger.o log_store.o master_mgr.o master_sm.o master_variables_store.o message_event.o msg_counter.o network.o node.o notifier_pool.o notify.o options.o paxos_log.o pnode.o propose_batch.o proposer.o replayer.o serial_lock.o sm_base.o sm.o socket.o system_variables_store.o system_v_sm.o tcp_acceptor.o tcp_client.o tcp.o timer.o udp.o util.o wait_lock.o master_sm.pb.o paxos_msg.pb.o

include $(top_srcdir)/src/backend/common.mk
