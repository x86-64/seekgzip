CC=gcc
SWIG=swig
PYTHON=python

LDFLAGS=-lz

USR_BIN_TARGETS=seekgzip
USR_LIB_TARGETS=libseekgzip.so
USR_INC_TARGETS=seekgzip.h
PHONY_TARGETS=.python

TARGETS=$(USR_BIN_TARGETS) $(USR_LIB_TARGETS) $(PHONY_TARGETS)

all: $(TARGETS)
clean:
	rm -rf $(TARGETS)
	rm -rf export_python.cpp
	
install:
	mkdir -p $(DESTDIR)/usr/bin/ $(DESTDIR)/usr/lib/ $(DESTDIR)/usr/include/seekgzip/
	cp $(USR_BIN_TARGETS) $(DESTDIR)/usr/bin/
	cp $(USR_LIB_TARGETS) $(DESTDIR)/usr/lib/
	cp $(USR_INC_TARGETS) $(DESTDIR)/usr/include/seekgzip/
	test -f .python && $(PYTHON) setup.py install || exit 0

seekgzip: seekgzip.c main.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ seekgzip.c main.c

libseekgzip.so: seekgzip.c
	$(CC) $(CFLAGS) $(LDFLAGS) -fPIC -shared -o $@ $<

.python: swig.i export_cpp.h export_cpp.cpp setup.py
	$(SWIG) -c++ -python -o export_python.cpp swig.i
	$(PYTHON) setup.py build
	touch $@

