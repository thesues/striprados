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
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>

void usage() {
	printf("Usage:\n"
			"UPLOAD FILE\n"
			"./striprados -p poolname -k key filename\n"
			"DOWNLOAD FILE\n"
			"./striprados -p poolname -g key filename\n"
			"LIST ALL FILES\n"
			"./striprados -p poolname -l\n");
}

#define NOOPS -1
#define DONWLOAD 0
#define UPLOAD 1
#define LIST     2

#define BUFFSIZE 64<<20 /* 64M */


int is_head_object(const char * entry) {
	const char *p;
	if((p = strrchr(entry, '.')) != NULL) {
		if (strncmp(p+1, "0000000000000000", 16) == 0)
			return p-entry;
	}
	return 0;
}

int do_ls(rados_ioctx_t ioctx) {
	int ret;
	const char *entry;
	rados_list_ctx_t list_ctx;
	char buf[128];
	int length;
	ret = rados_objects_list_open(ioctx, &list_ctx);
	if (ret < 0) {
		printf("error reading list");
		return -1;
	}	
	printf("===striper objects list===\n");
	while(rados_objects_list_next(list_ctx, &entry, NULL) != -ENOENT) {
		if ((length = is_head_object(entry)) == 0)
			continue;
		memset(buf, 0, 128);
		if (rados_getxattr(ioctx, entry, "striper.size", buf, 128) > 0) {
			printf("%-10.*s|%-10s\n", length, entry, buf);
		} else {
			printf("can not get striper.size of %s", entry);
		}
		
	}
	
	rados_objects_list_close(list_ctx);
	return 0;
}

/* aio */
int do_put2(rados_striper_t striper, const char *key, const char *filename, uint16_t concurrent) {
	
	uint16_t i = 0 ;
	int ret = 0;
	char **buf_list = malloc(concurrent * sizeof(char *));
	uint64_t count = 0;
	uint64_t offset = 0;
	if (buf_list == NULL) {
		printf("calloc failed\n");
		return -1;
	}

	for(i = 0 ; i < concurrent; i++) {
		buf_list[i] = calloc(1, BUFFSIZE);
		if (buf_list[i] == NULL) {
			printf("calloc failed\n");
			ret = -1;
			goto out;
		}
	}


	int fd = open(filename, O_RDONLY);
	if (fd < 0) {
		printf("error reading file %s", filename);
		ret = -1;
		goto out;
	}

	rados_completion_t *completion_list = calloc(concurrent ,  sizeof(rados_completion_t));
	if (completion_list == NULL) {
		goto out1;
	}

	for (i = 0 ; i < concurrent ; i ++ )
		if (rados_aio_create_completion(NULL, NULL, NULL, &completion_list[i]) < 0 )
			goto out3;

	count = BUFFSIZE;
	while (count != 0 ) {
		for (i = 0 ; i < concurrent; i ++) {
			count = read(fd, buf_list[i], BUFFSIZE);
			if (count < 0) {
				printf("read file failed\n");
				ret = -1;
				goto out2;
				
			}
			if (count == 0) {
				ret = 0;
				goto out2;
			}
			rados_striper_aio_write(striper, key, completion_list[i], buf_list[i], count, offset);
			offset += count;
		}
		for(i = 0 ; i < concurrent ; i++) 
			rados_aio_wait_for_complete(completion_list[i]);

		printf("%lld\r", offset);
		fflush(stdout);
	}

out3:
	for(i = 0; i < concurrent; i++) {
		if(completion_list[i] != NULL)
			rados_aio_release(completion_list[i]);
	}
out2:
	free(completion_list);
	
out1:
	close(fd);
out:
	for(i = 0 ; i < concurrent; i++) {
		if (buf_list[i] != NULL)
			free(buf_list[i]);
	}
	free(buf_list);
	return ret;
}

/* sync io */
int do_put(rados_striper_t striper, const char *key, const char *filename) {
	char *buf = (char*)malloc(BUFFSIZE);
	struct stat sb;
	int count;
	uint64_t offset = 0;
	uint64_t file_size;

	int fd = open(filename, O_RDONLY);
	/* stack should be big enough to hold this buf*/
	
	if (fd < 0) {
		printf("error reading file %s", filename);
		return -1;
	}
	count = BUFFSIZE;
	fstat(fd,&sb);
	file_size = sb.st_size;
	while (count != 0 ) {
		count = read(fd, buf, BUFFSIZE);
		if (count < 0) {
			close(fd);
			break;
		}
		if (count == 0) {
			close(fd);
			break;
		}
		rados_striper_write(striper, key, buf, count, offset);
		offset += count;
		printf("%lu%%\r", offset*100/file_size);
		fflush(stdout);
	}
	free(buf);
	return 0;
}


int do_get(rados_ioctx_t ioctx, rados_striper_t striper, const char *key, const char *filename) {

	char *buf = malloc(BUFFSIZE);
	char numbuf[128];
	uint64_t offset = 0;
	int count = 0;
	uint64_t file_size;
	memset(numbuf, 0, 128);
	memset(buf, 0, BUFFSIZE);

	int fd = open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0644);
	if (fd < 0) {
		printf("error writing file %s\n", filename);
		return -1;
	}

	printf("getting key %s for file %s\n", key, filename);
	char * sobj = malloc(strlen(key) + 17 + 1);
	sprintf(sobj,"%s.%016d", key, 0);

	if (rados_getxattr(ioctx, sobj, "striper.size", numbuf, 128) > 0) {
		sscanf(numbuf, "%lu", &file_size);
		if (file_size == 0)
			goto out;
	} else
		goto out;
	
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
		printf("%lld%%\r", offset*100/file_size);
		fflush(stdout);
	}
out:
	close(fd);
	free(buf);
	free(sobj);
	return 0;
}


int main(int argc, const char **argv)
{

	int opt;
	char *pool_name = NULL;
	char *key = NULL;
	const char *filename = NULL;
	int ret = 0;
	int action = NOOPS;

	while ((opt = getopt(argc, (char* const *) argv, "p:k:g:l")) != -1) {
		switch (opt) {
			case 'p':
				pool_name = optarg;
				break;
			case 'k':
				action = UPLOAD;
				key = optarg;
				break;
			case 'g':
				action = DONWLOAD;
				key = optarg;
				break;
			case 'l':
				action = LIST;
				break;
			default:
				usage();
				return EXIT_FAILURE;
		}
	}

	if (action == UPLOAD || action  == DONWLOAD) {
		if (argc == optind + 1 && pool_name) {
			filename = argv[optind];
		} else {
			usage();
			return EXIT_FAILURE;
		}
	} else if (action == LIST && pool_name) {
		
	} else {
			usage();
			return EXIT_FAILURE;
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
	printf("just set up a rados cluster object\n");

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


	rados_striper_set_object_layout_stripe_unit(striper, 128<<10);
	rados_striper_set_object_layout_object_size(striper, 8<<20);
	rados_striper_set_object_layout_stripe_count(striper, 128);


	switch (action) {
		case LIST:
			do_ls(io_ctx);
			break;
		case UPLOAD:
			do_put2(striper, key, filename, 4);
			break;
		case DONWLOAD:
			do_get(io_ctx, striper, key, filename);
			break; 
		default:
			printf("wrong action\n");
			goto out;
	}

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
