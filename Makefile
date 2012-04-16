CC=gcc
SWIG=swig
PYTHON=python

LDFLAGS=-lz

USR_BIN_TARGETS=seekgzip
USR_LIB_TARGETS=libseekgzip.so
PHONY_TARGETS=.python

TARGETS=$(USR_BIN_TARGETS) $(USR_LIB_TARGETS) $(PHONY_TARGETS)

all: $(TARGETS)
clean:
	rm -rf $(TARGETS)
	rm -rf export_python.cpp
	
install:
	mkdir -p $(DESTDIR)/usr/bin/ $(DESTDIR)/usr/lib/
	cp $(USR_BIN_TARGETS) $(DESTDIR)/usr/bin/
	cp $(USR_LIB_TARGETS) $(DESTDIR)/usr/lib/
	test -f .python && $(PYTHON) setup.py install || exit 0

seekgzip: seekgzip.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ -DBUILD_UTILITY $<

libseekgzip.so: seekgzip.c
	$(CC) $(CFLAGS) $(LDFLAGS) -fPIC -shared -o $@ $<

.python: swig.i export_cpp.h export_cpp.cpp setup.py
	$(SWIG) -c++ -python -o export_python.cpp swig.i
	$(PYTHON) setup.py build
	touch $@

