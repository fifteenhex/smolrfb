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

PROGS=smolrfb_test

all: $(PROGS)

smolrfb_test: smolrfb_test.c smolrfb.h
	$(CC) $(COPTS) -o $@ $<

.PHONY: clean
clean:
	rm -rf $(PROGS)
