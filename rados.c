// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 * Copyright 2013 Inktank
 */

// install the librados-dev package to get this
#include <radosstriper/libradosstriper.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

void usage() {
	printf("Usage:\n"
			"UPLOAD FILE\n"
			"./striprados -p poolname -k key filename\n"
			"DOWNLOAD FILE\n"
			"./striprados -p poolname -g key filename\n"
			"LIST ALL FILES\n"
			"./striprados -p poolname -l\n");
}

#define UPLOAD 1
#define DONWLOAD 0

#define BUFFSIZE 2<<20 /* 2M */

int do_ls() {

}

int do_put(rados_striper_t striper, char *key, char *filename) {
	int fd = open(filename, O_RDONLY);
	/* stack should be big enough to hold this buf*/
	char buf[BUFFSIZE];
	int count;
	int offset = 0;
	if (fd < 0) {
		printf("error reading file %s", filename);
		return -1;
	}
	count = BUFFSIZE;
	while (count != 0 ) {
		count = read(fd, buf, BUFFSIZE);
		if (count < 0) {
			close(fd);
			return -1;
		}
		if (count == 0) {
			close(fd);
			break;
		}
		rados_striper_write(striper, key, buf,count, offset);
		offset += count;
	}
	return 0;
}


int do_get(rados_striper_t striper, char *key, char *filename) {
	char buf[BUFFSIZE];
	int offset = 0;
	int count = 0;
	int fd = open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0644);
	if (fd < 0) {
		printf("error writing file %s\n", filename);
		return -1;
	}
	printf("getting key %s for file %s\n", key, filename);
	while (1) {
		count = rados_striper_read(striper, key, buf, BUFFSIZE, offset);
		if (count < 0) {
			printf("error reading rados file %s", key);
			close(fd);
			return -1;
		}
		if (count == 0)
			break;
		if (write(fd, buf, count) < 0)
			break;
		offset += count;
	}
	close(fd);
	return 0;
}


int main(int argc, const char **argv)
{

	int opt;
	int is_upload; 
	char *pool_name = NULL;
	char *key = NULL;
	char *filename = NULL;
	int ret = 0;
	int i = 0;
	int ls = 0;    

	while ((opt = getopt(argc, argv, "p:k:g:l")) != -1) {
		switch (opt) {
			case 'p':
				pool_name = optarg;
				break;
			case 'k':
				is_upload = UPLOAD;
				key = optarg;
				break;
			case 'g':
				is_upload = DONWLOAD;
				key = optarg;
				break;
			case 'l':
				ls = 1;
				break;
			default:
				usage();
				return EXIT_FAILURE;
		}
	}

	printf("argc %d, optind %d\n", argc, optind);
	if (!ls) {
		if (argc == optind + 1) {
			filename = argv[optind];
		} else {
			usage();
			return EXIT_FAILURE;
		}


	}	

	rados_ioctx_t io_ctx = NULL;
	rados_striper_t striper = NULL;

	rados_t rados = NULL;
	ret = rados_create(&rados, "admin"); // just use the client.admin keyring
	if (ret < 0) { // let's handle any error that might have come back
		printf("couldn't initialize rados! error %d\n", ret);
		ret = EXIT_FAILURE;
		goto out;
	}
	printf("we just set up a rados cluster object\n");

	ret = rados_conf_read_file(rados, "/etc/ceph/ceph.conf");

	ret = rados_connect(rados);
	if (ret < 0) {
		printf("couldn't connect to cluster! error %d\n", ret);
		ret = EXIT_FAILURE;
		goto out;
	}
	printf("we just connected to the rados cluster\n");


	ret = rados_ioctx_create(rados, pool_name, &io_ctx);
	if (ret < 0) {
		printf("couldn't set up ioctx! error %d\n", ret);
		ret = EXIT_FAILURE;
		goto out;
	} else {
		printf("we just created an ioctx for our pool\n");
	}

	ret = rados_striper_create(io_ctx, &striper);
	if (ret < 0) {
		printf("couldn't set up striper error %d\n", ret);
		ret = EXIT_FAILURE;
		goto out;
	} else {
		printf("we just created a striper for our pool\n");
	}


	rados_striper_set_object_layout_stripe_unit(striper, 512<<10);  /* 512K */
	rados_striper_set_object_layout_object_size(striper, 4<<20); /* 4M */
	rados_striper_set_object_layout_stripe_count(striper, 4);


	if(ls) 
		ret = do_ls(striper);
	else if (is_upload == UPLOAD)
		ret = do_put(striper, key, filename);
	else if (is_upload == DONWLOAD)
		ret = do_get(striper, key, filename);

out:
	if (striper) {
		rados_striper_destroy(striper);
	}
	if (io_ctx) {
		rados_ioctx_destroy(io_ctx);
	}
	if(rados)
		rados_shutdown(rados);
	return ret;
}
