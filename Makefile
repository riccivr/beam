# beam - ephemeral local-first file and video sharing
# See LICENSE file for copyright and license details.

include config.mk

SRC = qr.c beam.c
OBJ = $(SRC:.c=.o)

all: options beam

options:
	@echo beam build options:
	@echo "CFLAGS   = $(CFLAGS)"
	@echo "LDFLAGS  = $(LDFLAGS)"
	@echo "CC       = $(CC)"

.c.o:
	@echo CC $<
	@$(CC) -c $(CFLAGS) $<

$(OBJ): config.mk arg.h beam.h qr.h

beam: $(OBJ)
	@echo CC -o $@
	@$(CC) -o $@ $(OBJ) $(LDFLAGS)

clean:
	@echo cleaning
	@rm -f beam $(OBJ) beam-$(VERSION).tar.gz

dist: clean
	@echo creating dist tarball
	@mkdir -p beam-$(VERSION)/tests
	@cp -R LICENSE Makefile README.md config.mk beam.1 arg.h beam.h qr.h beam.c qr.c tests beam-$(VERSION)
	@tar -cf beam-$(VERSION).tar beam-$(VERSION)
	@gzip beam-$(VERSION).tar
	@rm -rf beam-$(VERSION)

install: all
	@echo installing executable file to $(DESTDIR)$(PREFIX)/bin
	@mkdir -p $(DESTDIR)$(PREFIX)/bin
	@cp -f beam $(DESTDIR)$(PREFIX)/bin
	@chmod 755 $(DESTDIR)$(PREFIX)/bin/beam
	@echo installing manual page to $(DESTDIR)$(MANPREFIX)/man1
	@mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	@sed "s/VERSION/$(VERSION)/g" < beam.1 > $(DESTDIR)$(MANPREFIX)/man1/beam.1
	@chmod 644 $(DESTDIR)$(MANPREFIX)/man1/beam.1

uninstall:
	@echo removing executable file from $(DESTDIR)$(PREFIX)/bin
	@rm -f $(DESTDIR)$(PREFIX)/bin/beam
	@echo removing manual page from $(DESTDIR)$(MANPREFIX)/man1
	@rm -f $(DESTDIR)$(MANPREFIX)/man1/beam.1

test: beam
	@sh tests/test_beam.sh

.PHONY: all options clean dist install uninstall test
