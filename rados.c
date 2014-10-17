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
#include <semaphore.h>
#include <assert.h>

void usage() {
	printf("Usage:\n"
			"UPLOAD FILE\n"
			"./striprados -p poolname -k key filename\n"
			"DOWNLOAD FILE\n"
			"./striprados -p poolname -g key filename\n"
			"LIST ALL FILES\n"
			"./striprados -p poolname -l\n");
}

/*
#define NOOPS -1
#define DONWLOAD 0
#define UPLOAD 1
#define LIST     2
#define DELETE
#define INFO
*/
enum act {
 NOOPS = -1,
 DONWLOAD,
 UPLOAD,
 LIST ,
 DELETE,
 INFO
};

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

struct buffer_manager {
	char ** free_buf;
	int index;
	int max_buf_num;
	int current_buf_num;
	sem_t mutex;
	sem_t available_bufs;
};


int init_buffer_manager(struct buffer_manager *bm, int concurrent) {
	/* concurrent must >= 1 */
	int ret;
	bm->free_buf = (char **)calloc(concurrent , sizeof(char*));
	if (bm->free_buf == NULL) {
		printf("failed to allocate free buffer manager\n");
		return -1;
	}
	bm->free_buf[0] = calloc(BUFFSIZE, sizeof(char));

	if (bm->free_buf[0] == NULL) {
		printf("failed to allocate the first buffer\n");
		ret = -1;
		goto out;
	}
	bm->index = 0;
	bm->max_buf_num = concurrent;
	/* point to the last available buffer slot */
	bm->current_buf_num = 0;
	
	/* initial mutex */
	if (sem_init(&bm->mutex, 0, 1) != 0) {
		ret = -errno;
		goto out1;
		
	}
	/* initial available_bufs */
	if (sem_init(&bm->available_bufs, 0, concurrent) != 0 ) {
		ret = -errno;
		goto out2;
	}

	return 0;

out2:
	sem_destroy(&bm->mutex);
out1:
	free(bm->free_buf[0]);
out:
	free(bm->free_buf);
	return ret;
	
}

/* we must be sure that all buffer has been reclaimed by put_buffer_back */
void destory_buffer_manager(struct buffer_manager *bm) {
	int i;
	for (i = 0 ; i < bm->current_buf_num ; i++) {
		free(bm->free_buf[i]);
	}
	free(bm->free_buf);
	sem_destroy(&bm->mutex);
	sem_destroy(&bm->available_bufs);
}


char* get_free_buffer(struct buffer_manager *bm) {
	/*bm->index will always equal to sem_value(&bm->available_bufs) - 1 */
	sem_wait(&bm->available_bufs);
	/* There is available_bufs for me to use */
	/* now to manapulate the free buf list */
	sem_wait(&bm->mutex);

	/* lazy allocate buffer */
	if (bm->current_buf_num < bm->max_buf_num && bm->index < 0) {
		bm->index++ ;
		bm->free_buf[bm->index] = calloc(BUFFSIZE, sizeof(char));
		if (bm->free_buf[bm->index] == NULL) {
			sem_post(&bm->mutex);
			sem_post(&bm->available_bufs);
			return NULL;
		}
		bm->current_buf_num ++;
	}

	/* user used a buffer */
	bm->index-- ;
	sem_post(&bm->mutex);
	return bm->free_buf[bm->index + 1];
}

void put_buffer_back(struct buffer_manager *bm, char *buf) {
	sem_wait(&bm->mutex);
	bm->index++;
	bm->free_buf[bm->index] = buf;
	sem_post(&bm->mutex);

	sem_post(&bm->available_bufs);
}

struct buffer_manager bm;

void set_completion_complete(rados_completion_t cb, void *arg)
{
	char *buf = (char*)arg;
	put_buffer_back(&bm, buf);
}


/* aio */
int do_put2(rados_striper_t striper, const char *key, const char *filename, uint16_t concurrent, int overwrite) {
	
	int ret = 0;
	int i;
	uint64_t count = 0;
	uint64_t offset = 0;
	char *buf = NULL;
	rados_completion_t my_completion;
	#define COMPLETION_LIST_SIZE 256
	rados_completion_t *completion_list = calloc(COMPLETION_LIST_SIZE, sizeof(rados_completion_t));
	int32_t next_num_writes = 0;
	uint32_t capacity = COMPLETION_LIST_SIZE;

	ret = init_buffer_manager(&bm, concurrent);

	if (ret < 0) {
		printf("failed to create buffer_manager\n");
		return -1;
	}
	
	int fd = open(filename, O_RDONLY);
	if (fd < 0) {
		printf("error reading file %s", filename);
		ret = -1;
		goto out;
	}

	if (overwrite == 1)
		rados_striper_trunc(striper, key, 0);
	count = BUFFSIZE;
	while (count != 0 ) {

		/* it may block */
		buf = get_free_buffer(&bm);

		/* can not allocate new buffer lazily, continue */
		if (buf == NULL) {
			printf("failed to get buf\n");
			continue;
		}

		count = read(fd, buf, BUFFSIZE);

		if (count < 0) {
			printf("failed to read from file\n");
			ret = -1;
			break;
		}

		if (count == 0) {
			ret = 0;
			break;
		}

		/* use completion_list to store every completion_list  */
		ret = rados_aio_create_completion((void *)buf, set_completion_complete, NULL, &my_completion);
		if (ret < 0) {
			printf("failed to create completion\n");
			goto out1;
		}
		if (next_num_writes >= capacity) {
			completion_list =  realloc(completion_list, capacity << 1);
			capacity = capacity << 1;
		}
		completion_list[next_num_writes++] = my_completion;

		rados_striper_aio_write(striper, key, my_completion, buf, count, offset);

		offset += count;
		printf("%lu\n", offset);
		fflush(stdout);

	}
	
out1:
	for(i = 0 ; i < next_num_writes ; i ++) {
		rados_aio_wait_for_safe(completion_list[i]);
		rados_aio_release(completion_list[i]);
	}
	
	close(fd);
out:
	destory_buffer_manager(&bm);
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

	char numbuf[128];
	uint64_t offset = 0;
	int count = 0;
	uint64_t file_size;
	int ret = 0;

	memset(numbuf, 0, 128);
	int fd = open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0644);
	if (fd < 0) {
		printf("error writing file %s\n", filename);
		return -1;
	}

	char *buf = malloc(BUFFSIZE);
	if (buf == NULL) {
		ret = -1;
		goto out;
	}
	memset(buf, 0, BUFFSIZE);


	char * sobj = malloc(strlen(key) + 17 + 1);
	if (sobj == NULL) {
		ret = -1;
		goto out1;
	}

	sprintf(sobj,"%s.%016d", key, 0);

	if (rados_getxattr(ioctx, sobj, "striper.size", numbuf, 128) > 0) {
		sscanf(numbuf, "%lu", &file_size);
	} else {
		ret = -1;
		goto out2;
	}
	
	while (1) {
		count = rados_striper_read(striper, key, buf, BUFFSIZE, offset);
		if (count < 0) {
			printf("error reading rados file %s", key);
			close(fd);
			return -1;
		}
		if (count == 0) {
			ret = 0;
			break;
		}
		if (write(fd, buf, count) < 0){ 
			ret = -1;
			goto out2;
		}
		offset += count;
		printf("%lu%%\r", offset*100/file_size);
		fflush(stdout);
	}

out2:
	free(sobj);
out1:	
	free(buf);
out:
	close(fd);
	return ret;
}

int do_delete(rados_striper_t striper, const char *key) {
	int ret;
	printf("deleting %s\n",key);
	ret = rados_striper_remove(striper, key);
	if (ret < 0) {
		printf("%s delete failed\n", key);
		return -1;
	}
	printf("%s deleted\n",key);
	return 0;
}

/* not implemetated */
int do_info() {
	return 0;
}

int main(int argc, const char **argv)
{

	int opt;
	char *pool_name = NULL;
	char *key = NULL;
	const char *filename = NULL;
	int ret = 0;
	enum act action = NOOPS;

	while ((opt = getopt(argc, (char* const *) argv, "p:k:g:lr:i:")) != -1) {
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
			case 'r':
				action = DELETE;
				key = optarg;
				break;
			case 'i':
				action = INFO;
				key = optarg;
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
	} else if ((action == LIST || action == DELETE || action == INFO )&& pool_name) {
		
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
	printf("set up a rados cluster object\n");

	ret = rados_conf_read_file(rados, "/etc/ceph/ceph.conf");

	ret = rados_connect(rados);
	if (ret < 0) {
		printf("couldn't connect to cluster! error %d\n", ret);
		ret = EXIT_FAILURE;
		goto out;
	}
	printf("connected to the rados cluster\n");


	ret = rados_ioctx_create(rados, pool_name, &io_ctx);
	if (ret < 0) {
		printf("couldn't set up ioctx! error %d\n", ret);
		ret = EXIT_FAILURE;
		goto out;
	} else
		printf("created an ioctx for our pool\n");

	ret = rados_striper_create(io_ctx, &striper);
	if (ret < 0) {
		printf("couldn't set up striper error %d\n", ret);
		ret = EXIT_FAILURE;
		goto out;
	} else {
		printf("created a striper for our pool\n");
	}


	rados_striper_set_object_layout_stripe_unit(striper, 128<<10);
	rados_striper_set_object_layout_object_size(striper, 8<<20);
	rados_striper_set_object_layout_stripe_count(striper, 256);


	switch (action) {
		case LIST:
			ret = do_ls(io_ctx);
			break;
		case UPLOAD:
			ret = do_put2(striper, key, filename, 4, 0);
			break;
		case DONWLOAD:
			ret = do_get(io_ctx, striper, key, filename);
			break; 
		case DELETE:
			ret = do_delete(striper, key);
			break;
		case INFO:
			ret = do_info(striper, key);
			break;
		default:
			printf("wrong action\n");
			ret = -1;
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
