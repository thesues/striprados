striprados:rados.c
	cc  -Wall -g -o$@ -lradosstriper rados.c -lc
clean:
	rm striprados -rf
	rm core* -rf
