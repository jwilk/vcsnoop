# Copyright © 2022 Jakub Wilk <jwilk@jwilk.net>
# SPDX-License-Identifier: MIT

PREFIX = /usr/local
DESTDIR =

bindir = $(PREFIX)/bin

CFLAGS ?= -g -O2
CFLAGS += -Wall -Wextra

.PHONY: all
all: vcsnoop

.PHONY: install
install: vcsnoop
	install -d $(DESTDIR)$(bindir)
	install -m755 $(<) $(DESTDIR)$(bindir)/

.PHONY: clean
clean:
	rm -f vcsnoop

# vim:ts=4 sts=4 sw=4 noet
