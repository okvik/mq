</$objtype/mkfile

HFILES=`{test -d src && walk -f src | grep '\.h$'}
CFILES=`{test -d src && walk -f src | grep '\.c$'}
CMAIN=`{grep -l '^(thread)?main\(' $CFILES /dev/null}
CCOM=`{grep -L '^(thread)?main\(' $CFILES /dev/null | sed '/^\/dev\/null/d'}
OCOM=${CCOM:src/%.c=obj/$objtype/%.o}

BINTARG=${CMAIN:src/%.c=bin/$objtype/%}
RCFILES=`{test -d rc && walk -f rc}
MANFILES=`{test -d man && walk -n 2,2 -f man}

BIN=/$objtype/bin
RC=/rc/bin
MAN=/sys/man

DIRS=bin obj bin/$objtype obj/$objtype

BININST=${BINTARG:bin/$objtype/%=$BIN/%}
RCINST=${RCFILES:rc/%=$RC/%}
MANINST=${MANFILES:man/%=$MAN/%}
INST=$BININST $RCINST $MANINST

none:V: all

$DIRS:
	mkdir -p $target

obj/$objtype/%.o: obj/$objtype $HFILES

obj/$objtype/%.o: src/%.c
	$CC $CFLAGS -o $target src/$stem.c

bin/$objtype/%: bin/$objtype obj/$objtype/%.o $OCOM
	$LD $LDFLAGS -o $target obj/$objtype/$stem.o $OCOM

$BIN/%: bin/$objtype/%
	cp $prereq $target

$RC/%: rc/%
	cp -x $prereq $target

/sys/man/%: man/%
	cp $prereq $target

man:V: $MANINST

%.cpus:V:
	for(objtype in $CPUS) mk $MKFLAGS $stem

all:V: $BINTARG

install:V: $INST

installall:V: install.cpus

uninstall:V:
	rm -f $INST

clean:V:
	rm -rf bin obj
