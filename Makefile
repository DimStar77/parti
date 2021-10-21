CC      = gcc
CFLAGS  = -g -O2 -fomit-frame-pointer -Wall
LDFLAGS = -ljson-c -luuid -lblkid
BINDIR  = /usr/bin

GIT2LOG := $(shell if [ -x ./git2log ] ; then echo ./git2log --update ; else echo true ; fi)
GITDEPS := $(shell [ -d .git ] && echo .git/HEAD .git/refs/heads .git/refs/tags)
VERSION := $(shell $(GIT2LOG) --version VERSION ; cat VERSION)
BRANCH  := $(shell git branch | perl -ne 'print $$_ if s/^\*\s*//')
PREFIX  := parti-$(VERSION)

CFLAGS  += -DVERSION=\"$(VERSION)\"

PARTI_SRC = parti.c disk.c filesystem.c util.c
PARTI_OBJ = $(PARTI_SRC:.c=.o)

.PHONY: all install archive clean

all: changelog parti

changelog: $(GITDEPS)
	$(GIT2LOG) --changelog changelog

$(PARTI_OBJ): %.o: %.c disk.h filesystem.h util.h
	$(CC) -c $(CFLAGS) $<

parti: $(PARTI_OBJ)
	$(CC) $(PARTI_OBJ) $(LDFLAGS) -o $@

install: parti
	install -m 755 -D parti $(DESTDIR)$(BINDIR)/parti

archive: changelog
	mkdir -p package
	git archive --prefix=$(PREFIX)/ $(BRANCH) > package/$(PREFIX).tar
	tar -r -f package/$(PREFIX).tar --mode=0664 --owner=root --group=root --mtime="`git show -s --format=%ci`" --transform='s:^:$(PREFIX)/:' VERSION changelog
	xz -f package/$(PREFIX).tar

clean:
	rm -f *~ *.o parti changelog VERSION
	rm -rf package
