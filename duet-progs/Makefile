# Export all variables to sub-makes by default
export

CC = @gcc
LN = ln
AM_CFLAGS = -Wall -D_FILE_OFFSET_BITS=64 -fno-strict-aliasing -fPIC
CFLAGS = -g -O1 -fno-strict-aliasing
objects = utils.o
cmds_objects = cmds-status.o cmds-task.o cmds-debug.o

INSTALL = install
prefix ?= /usr/local
bindir = $(prefix)/bin
LIBS = #-luuid -lblkid -lm -lz -llzo2 -L.
libdir ?= $(prefix)/lib
incdir = $(prefix)/include/duet

MAKE = @make
MAKEOPTS = --no-print-directory

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

headers = ioctl.h

# make C=1 to enable sparse
AM_CFLAGS += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2

.c.o:
	@echo "    [CC]     $@"
	$(CC) $(AM_CFLAGS) $(CFLAGS) -c $<

all: version.h $(progs) manpages $(BUILDDIRS)
$(SUBDIRS): $(BUILDDIRS)
$(BUILDDIRS):
	@echo "Making all in $(patsubst build-%,%,$@)"
	$(MAKE) $(MAKEOPTS) -C $(patsubst build-%,%,$@)

version.h:
	@echo "    [SH]     $@"
	bash version.sh

# keep intermediate files from the below implicit rules around
.PRECIOUS: $(addsuffix .o,$(progs))

# Make any duet-foo out of duet-foo.o, with appropriate libs. The $($(subst...))
# bits below takes the duet_*_libs definitions above and turns them into a list
# of libraries to link against if they exist
duet-%: $(objects) duet-%.o
	@echo "    [LD]     $@"
	$(CC) $(CFLAGS) -o $@ $(objects) $@.o $(LDFLAGS) $(LIBS) \
		$($(subst -,_,$@-libs))

duet: $(objects) duet.o help.o $(cmds_objects)
	@echo "    [LD]     $@"
	$(CC) $(CFLAGS) -o duet duet.o help.o $(cmds_objects) $(objects) \
		$(LDFLAGS) $(LIBS) -lpthread

manpages:
	$(MAKE) $(MAKEOPTS) -C man

ioctl-test: $(objects) $(libs) ioctl-test.o
	@echo "    [LD]     $@"
	$(CC) $(CFLAGS) -o ioctl-test $(objects) ioctl-test.o $(LDFLAGS) $(LIBS)

clean: $(CLEANDIRS)
	@echo "Cleaning"
	rm -f $(progs) *.o version.h ioctl-test

$(CLEANDIRS):
	@echo "Cleaning $(patsubst clean-%,%,$@)"
	$(MAKE) $(MAKEOPTS) -C $(patsubst clean-%,%,$@) clean

install: $(progs) $(INSTALLDIRS)
	$(INSTALL) -m755 -d $(DESTDIR)$(bindir)
	$(INSTALL) $(progs) $(DESTDIR)$(bindir)
	$(INSTALL) -m755 -d $(DESTDIR)$(incdir)
	$(INSTALL) -m644 $(headers) $(DESTDIR)$(incdir)

$(INSTALLDIRS):
	@echo "Making install in $(patsubst install-%,%,$@)"
	$(MAKE) $(MAKEOPTS) -C $(patsubst install-%,%,$@) install
