striprados:rados.c
	cc -Wall -g -o$@ -lradosstriper rados.c
clean:
	rm striprados -rf
