MODULE = qdless
SPEC = smartmet-qdless

REQUIRES = gdal fmt

include $(shell echo $${PREFIX-/usr})/share/smartmet/devel/makefile.inc

DEFINES = -DUNIX

INCLUDES += \
	-isystem $(includedir)/netcdf-3 \
	-I$(includedir)/smartmet \
	-Iinclude

LIBS += $(PREFIX_LDFLAGS) \
	-lncursesw -ltinfo -lpanelw \
	-ljsoncpp \
	-lnetcdf_c++4 -lnetcdf \
	-lsmartmet-grid-files \
	-lsmartmet-calculator \
	-lsmartmet-smarttools \
	-lsmartmet-newbase \
	-lsmartmet-macgyver \
	-lsmartmet-gis \
	-lsmartmet-imagine \
	-lboost_regex \
	-lboost_program_options \
	-lboost_iostreams \
	-lboost_thread \
	-lboost_filesystem \
	$(REQUIRED_LIBS) \
	-lhdf5 \
	-lbz2 -ljpeg -lpng -lz -lrt \
	-lpthread

# Compilation directories

vpath %.cpp source main
vpath %.h include
vpath %.o $(objdir)
vpath %.d $(objdir)

# Files to compile

HDRS     = $(patsubst include/%,%,$(wildcard include/*.h))
SRCS     = $(patsubst source/%,%,$(wildcard source/*.cpp))
OBJS     = $(SRCS:%.cpp=%.o)
OBJFILES = $(OBJS:%.o=obj/%.o)
MAINSRC  = qdless.cpp
MAINOBJ  = obj/qdless.o

INCLUDES := -Iinclude $(INCLUDES)

ALLSRCS = $(wildcard main/*.cpp source/*.cpp)

.PHONY: test rpm

# Rules

all: objdir qdless
debug: objdir qdless
release: objdir qdless
profile: objdir qdless

qdless: $(MAINOBJ) $(OBJFILES)
	$(CXX) $(LDFLAGS) -o $@ $(MAINOBJ) $(OBJFILES) $(LIBS)

clean:
	rm -f qdless source/*~ include/*~ main/*~
	rm -rf obj
	@if [ -f test/Makefile ]; then $(MAKE) -C test $@; fi

format:
	clang-format -i -style=file include/*.h source/*.cpp main/*.cpp

mandir ?= $(PREFIX)/share/man

install:
	mkdir -p $(bindir)
	$(INSTALL_PROG) qdless $(bindir)/qdless
	mkdir -p $(datadir)/smartmet/qdless/palettes
	$(INSTALL_DATA) cnf/qdless.conf $(datadir)/smartmet/qdless/qdless.conf
	@for f in palettes/*.json; do \
	  echo $(INSTALL_DATA) $$f $(datadir)/smartmet/qdless/palettes/; \
	  $(INSTALL_DATA) $$f $(datadir)/smartmet/qdless/palettes/; \
	done
	$(INSTALL_DATA) data/cities1000.tsv $(datadir)/smartmet/qdless/cities1000.tsv

test:
	@if [ -f test/Makefile ]; then cd test && $(MAKE) test; \
	else echo "no test/Makefile — skipping"; fi

objdir:
	@mkdir -p $(objdir)

rpm: clean $(SPEC).spec
	rm -f $(SPEC).tar.gz
	tar -czvf $(SPEC).tar.gz --exclude test --exclude-vcs --transform "s,^,$(SPEC)/," *
	rpmbuild -tb $(SPEC).tar.gz
	rm -f $(SPEC).tar.gz

.SUFFIXES: $(SUFFIXES) .cpp

obj/%.o : %.cpp
	$(CXX) $(CFLAGS) $(INCLUDES) -c -MD -MF $(patsubst obj/%.o, obj/%.d, $@) -MT $@ -o $@ $<

-include obj/*.d
