CC=g++
CFLAGS=-Wall -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=26 `xml2-config --cflags`
LDFLAGS=-Wall -lpcre `pkg-config fuse --libs` `xml2-config --libs` -lsqlite3 -lpthread -lcurl
srcdir=src
builddir=build
PREFIX=/

all: $(builddir) loggedfs

$(builddir):
	mkdir $(builddir)

loggedfs: $(builddir)/loggedfs.o $(builddir)/Config.o $(builddir)/Filter.o
	$(CC) -o loggedfs $(builddir)/loggedfs.o $(builddir)/Config.o $(builddir)/Filter.o $(LDFLAGS)
$(builddir)/loggedfs.o: $(builddir)/Config.o $(builddir)/Filter.o $(srcdir)/loggedfs.cpp
	$(CC) -o $(builddir)/loggedfs.o -c $(srcdir)/loggedfs.cpp $(CFLAGS)

$(builddir)/Config.o: $(builddir)/Filter.o $(srcdir)/Config.cpp $(srcdir)/Config.h
	$(CC) -o $(builddir)/Config.o -c $(srcdir)/Config.cpp $(CFLAGS)

$(builddir)/Filter.o: $(srcdir)/Filter.cpp $(srcdir)/Filter.h
	$(CC) -o $(builddir)/Filter.o -c $(srcdir)/Filter.cpp $(CFLAGS)

clean:
	rm -rf $(builddir)/

install:
	echo $(PREFIX)
	gzip loggedfs.1
	cp loggedfs.1.gz $(PREFIX)/share/man/man1/
	cp loggedfs $(PREFIX)/bin/
	cp loggedfs.xml $(PREFIX)/etc/


mrproper: clean
	rm -rf loggedfs

release:
	tar -c --exclude="CVS" $(srcdir)/ loggedfs.xml LICENSE loggedfs.1.gz Makefile | bzip2 - > loggedfs.tar.bz2

