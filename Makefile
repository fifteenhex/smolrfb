MAKEFLAGS += --no-builtin-rules

# Make sure we know where to get nolibc
ifndef NOLIBCDIR
$(error Please pass NOLIBCDIR with the path to your copy of nolibc (tools/include/nolibc/ in the linux source))
endif

# Upstream nolibc has no sockets, nolibc-extensions carries them
ifndef NOLIBCEXTDIR
$(error Please pass NOLIBCEXTDIR with the path to your copy of nolibc-extensions)
endif

COPTS=-ggdb \
	-nostdlib \
	-std=c99 \
	-Os \
	-include $(NOLIBCDIR)/nolibc.h \
	-include $(NOLIBCEXTDIR)/include/nolibc-extensions.h \
	-Wl,--hash-style=gnu

PROGS=smolrfb_test smolrfb_selftest

all: $(PROGS)

smolrfb_test: smolrfb_test.c smolrfb.h
	$(CC) $(COPTS) -o $@ $<

smolrfb_selftest: smolrfb_selftest.c
	$(CC) $(COPTS) -o $@ $<

# Out of the way of any real VNC display on 590x
CHECKPORT?=15900

.PHONY: check
check: $(PROGS)
	@./smolrfb_test $(CHECKPORT) & pid=$$!; \
	trap 'kill $$pid' EXIT; \
	sleep 1; \
	./smolrfb_selftest $(CHECKPORT)

.PHONY: clean
clean:
	rm -rf $(PROGS)
