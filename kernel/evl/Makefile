obj-$(CONFIG_EVL) += evl.o sched/ lib/ net/

evl-y :=		\
	clock.o		\
	control.o	\
	factory.o	\
	file.o		\
	inband_notify.o	\
	init.o		\
	memory.o	\
	monitor.o	\
	mutex.o		\
	namespace.o	\
	observable.o	\
	poll.o		\
	proxy.o		\
	random.o	\
	sem.o		\
	stax.o		\
	syscall.o	\
	thread.o	\
	tick.o		\
	timer.o		\
	wait.o		\
	work.o		\
	xbuf.o

evl-y += lkl.o

evl-$(CONFIG_FTRACE) +=	trace.o
evl-$(CONFIG_EVL_LKL_RUNTIME) += lkl_host.o lkl_jmp.o

ifeq ($(CONFIG_EVL_LKL),y)
evl-y += lkl_core.o
LKL_SRC ?= $(abspath $(srctree)/../lkl)
LKL_CONFIG ?= $(LKL_SRC)/arch/lkl/configs/defconfig
ifeq ($(wildcard $(LKL_SRC)/arch/lkl/kernel/setup.c),)
$(error CONFIG_EVL_LKL requires LKL_SRC pointing to the modified LKL source tree)
endif
LKL_EMBED_BUILD ?= $(abspath $(objtree)/lkl-embedded)
LKL_EMBED_JOBS ?= 8

quiet_cmd_lkl_core = LKL     $@
      cmd_lkl_core = $(PYTHON3) $(srctree)/scripts/evl-lkl-object.py \
	--source '$(LKL_SRC)' --config '$(LKL_CONFIG)' --build '$(LKL_EMBED_BUILD)' --output '$@' \
	--cc '$(CC)' --objcopy '$(OBJCOPY)' --nm '$(NM)' \
	--jobs '$(LKL_EMBED_JOBS)' \
	$(if $(CONFIG_MITIGATION_RETPOLINE),--retpoline) \
	$(if $(CONFIG_MITIGATION_RETHUNK),--rethunk) \
	$(if $(CONFIG_X86_KERNEL_IBT),--ibt) \
	$(if $(CONFIG_EVL_LKL_RUNTIME),--runtime) \
	$(if $(CONFIG_EVL_LKL_CLOCK),--clock)

# Always enter LKL's own incremental build so any LKL source edit is seen.
# The importer preserves this object's mtime when its contents are unchanged.
$(obj)/lkl_core.o: $(srctree)/scripts/evl-lkl-object.py FORCE
	$(call cmd,lkl_core)

targets += lkl_core.o

ifeq ($(CONFIG_EVL_LKL_RUNTIME),y)
ccflags-y += -I$(obj)
quiet_cmd_lkl_host_header = GEN     $@
      cmd_lkl_host_header = $(PYTHON3) $(srctree)/scripts/evl-lkl-host-header.py '$(LKL_SRC)' > $@.tmp && \
	{ cmp -s $@.tmp $@ && rm -f $@.tmp || mv -f $@.tmp $@; }

$(obj)/lkl_host_abi.h: $(LKL_SRC)/arch/lkl/include/uapi/asm/host_ops.h \
		$(LKL_SRC)/include/uapi/asm-generic/unistd.h \
		$(srctree)/scripts/evl-lkl-host-header.py FORCE
	$(call cmd,lkl_host_header)

$(obj)/lkl_host.o: $(obj)/lkl_host_abi.h
targets += lkl_host_abi.h
endif
else
evl-y += lkl_blob.o
endif
