striprados:rados.c
	cc  -Wall -g -o$@ -lradosstriper rados.c
install:
	install -D striprados $$DESTDIR/usr/bin/striprados
clean:
	rm striprados -rf
	rm core* -rf
dist:
	make clean
	git archive --format=tar --prefix striprados/ HEAD | gzip > striprados.tar.gz

