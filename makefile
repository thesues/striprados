striprados:rados.c
	cc -g -o$@ -lradosstriper rados.c
clean:
	rm striprados -rf
