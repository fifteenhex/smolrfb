MAKEFLAGS += --no-builtin-rules

# Make sure we know where to get nolibc
ifndef NOLIBCDIR
$(error Please pass NOLIBCDIR with the path to your copy of nolibc (tools/include/nolibc/ in the linux source))
endif

# Upstream nolibc has no sockets, nolibc-extensions carries them
ifndef NOLIBCEXTDIR
$(error Please pass NOLIBCEXTDIR with the path to your copy of nolibc-extensions)
endif

# The fake card smolrfb_fakedrm serves lives in the fakedrm repo
ifndef FAKEDRMDIR
$(error Please pass FAKEDRMDIR with the path to your copy of fakedrm)
endif

COPTS=-ggdb \
	-nostdlib \
	-std=c99 \
	-Os \
	-include $(NOLIBCDIR)/nolibc.h \
	-include $(NOLIBCEXTDIR)/include/nolibc-extensions.h \
	-Wl,--hash-style=gnu

PROGS=smolrfb_test smolrfb_selftest smolrfb_fakedrm fakedrm_demo

all: $(PROGS)

smolrfb_test: smolrfb_test.c smolrfb.h
	$(CC) $(COPTS) -o $@ $<

smolrfb_selftest: smolrfb_selftest.c
	$(CC) $(COPTS) -o $@ $<

smolrfb_fakedrm: smolrfb_fakedrm.c smolrfb.h $(FAKEDRMDIR)/fakedrm.h
	$(CC) $(COPTS) -I$(FAKEDRMDIR) -o $@ $<

# fakedrm's demo app, something for the check to run
fakedrm_demo: $(FAKEDRMDIR)/fakedrm_demo.c
	$(CC) $(COPTS) -o $@ $<

# Out of the way of any real VNC display on 590x
CHECKPORT?=15900
CHECKPORT_FAKEDRM?=15901

.PHONY: check
check: $(PROGS)
	@./smolrfb_test $(CHECKPORT) & pid=$$!; \
	trap 'kill $$pid' EXIT; \
	sleep 1; \
	./smolrfb_selftest $(CHECKPORT)
	@./smolrfb_fakedrm $(CHECKPORT_FAKEDRM) ./fakedrm_demo & pid=$$!; \
	trap 'kill $$pid' EXIT; \
	sleep 1; \
	./smolrfb_selftest $(CHECKPORT_FAKEDRM)

.PHONY: clean
clean:
	rm -rf $(PROGS)
