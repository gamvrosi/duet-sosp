# Export all variables to sub-makes by default
export

CC = gcc
LN = ln
AM_CFLAGS = -Wall -D_FILE_OFFSET_BITS=64 -fno-strict-aliasing -fPIC
CFLAGS = -g -O2 -fno-strict-aliasing
objects =
cmds_objects = cmds-status.o cmds-task.o cmds-debug.o
libduet_objects = duet-api.o itree.o rbtree.o
libduet_headers = duet.h itree.h rbtree.h

INSTALL = install
prefix ?= /usr/local
bindir = $(prefix)/bin
libdir ?= $(prefix)/lib
incdir = $(prefix)/include/duet
LIBS = $(libs)

ifeq ("$(origin V)", "command line")
  BUILD_VERBOSE = $(V)
endif
ifndef BUILD_VERBOSE
  BUILD_VERBOSE = 0
endif

ifeq ($(BUILD_VERBOSE),1)
  Q =
else
  Q = @
endif

MAKEOPTS = --no-print-directory Q=$(Q)

progs = duet

# External libs required by various binaries: for duet-foo, specify
# duet_foo_libs = <list of libs>; see $($(subst...)) rules below

SUBDIRS = man
BUILDDIRS = $(patsubst %,build-%,$(SUBDIRS))
INSTALLDIRS = $(patsubst %,install-%,$(SUBDIRS))
CLEANDIRS = $(patsubst %,clean-%,$(SUBDIRS))

.PHONY: $(SUBDIRS)
.PHONY: $(BUILDDIRS)
.PHONY: $(INSTALLDIRS)
.PHONY: $(CLEANDIRS)
.PHONY: all install clean

libs = libduet.so.1
lib_links = libduet.so
headers = $(libduet_headers)

# make C=1 to enable sparse
check_defs := .cc-defines.h 
ifdef C
	#
	# We're trying to use sparse against glibc headers which go wild
	# trying to use internal compiler macros to test features.  We
	# copy gcc's and give them to sparse.  But not __SIZE_TYPE__
	# 'cause sparse defines that one.
	#
	dummy := $(shell $(CC) -dM -E -x c - < /dev/null | \
			grep -v __SIZE_TYPE__ > $(check_defs))
	check = sparse -include $(check_defs) -D__CHECKER__ \
		-D__CHECK_ENDIAN__ -Wbitwise -Wuninitialized -Wshadow -Wundef
	check_echo = echo
	# don't use FORTIFY with sparse because glibc with FORTIFY can
	# generate so many sparse errors that sparse stops parsing,
	# which masks real errors that we want to see.
else
	check = true
	check_echo = true
	AM_CFLAGS += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2
endif

.c.o:
	@$(check_echo) "    [SP]     $<"
	$(Q)$(check) $(AM_CFLAGS) $(CFLAGS) $<
	@echo "    [CC]     $@"
	$(Q)$(CC) $(AM_CFLAGS) $(CFLAGS) -c $<

all: version.h $(progs) manpages $(BUILDDIRS)
$(SUBDIRS): $(BUILDDIRS)
$(BUILDDIRS):
	@echo "Making all in $(patsubst build-%,%,$@)"
	$(Q)$(MAKE) $(MAKEOPTS) -C $(patsubst build-%,%,$@)

version.h:
	@echo "    [SH]     $@"
	$(Q)bash version.sh

$(libs): $(libduet_objects) $(lib_links)
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) $(libduet_objects) $(LDFLAGS) $(lib_LIBS) \
		-shared -Wl,-soname,libduet.so -o libduet.so.1

$(lib_links):
	@echo "    [LN]     $@"
	$(Q)$(LN) -sf libduet.so.1 libduet.so

# keep intermediate files from the below implicit rules around
.PRECIOUS: $(addsuffix .o,$(progs))

# Make any duet-foo out of duet-foo.o, with appropriate libs. The $($(subst...))
# bits below takes the duet_*_libs definitions above and turns them into a list
# of libraries to link against if they exist
duet-%: $(objects) $(libs) duet-%.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o $@ $(objects) $@.o $(LDFLAGS) $(LIBS) \
		$($(subst -,_,$@-libs))

duet: $(objects) duet.o help.o $(cmds_objects) $(libs)
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o duet duet.o help.o $(cmds_objects) $(objects) \
		$(LDFLAGS) $(LIBS) -lpthread

manpages:
	$(Q)$(MAKE) $(MAKEOPTS) -C man

ioctl-test: $(objects) $(libs) ioctl-test.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o ioctl-test $(objects) ioctl-test.o $(LDFLAGS) $(LIBS)

clean: $(CLEANDIRS)
	@echo "Cleaning"
	$(Q)rm -f $(progs) *.o version.h ioctl-test $(libs) $(lib_links)

$(CLEANDIRS):
	@echo "Cleaning $(patsubst clean-%,%,$@)"
	$(Q)$(MAKE) $(MAKEOPTS) -C $(patsubst clean-%,%,$@) clean

install: $(libs) $(progs) $(INSTALLDIRS)
	$(INSTALL) -m755 -d $(DESTDIR)$(bindir)
	$(INSTALL) $(progs) $(DESTDIR)$(bindir)
	$(INSTALL) -m755 -d $(DESTDIR)$(libdir)
	$(INSTALL) $(libs) $(DESTDIR)$(libdir)
	cp -a $(lib_links) $(DESTDIR)$(libdir)
	$(INSTALL) -m755 -d $(DESTDIR)$(incdir)
	$(INSTALL) -m644 $(headers) $(DESTDIR)$(incdir)
	ldconfig

$(INSTALLDIRS):
	@echo "Making install in $(patsubst install-%,%,$@)"
	$(Q)$(MAKE) $(MAKEOPTS) -C $(patsubst install-%,%,$@) install
