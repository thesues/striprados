// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: set ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.	See file COPYING.
 * Copyright 2013 Inktank
 */

// deanraccoon@gmail.com
//
// install the librados-dev package to get this
#define _LARGEFILE64_SOURCE /* used for lseek64 */
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
#include <signal.h>
#include <inttypes.h>
#include <time.h>
#include <pthread.h>
#include "threadpool.h"
#include "list.h"

#define debug(f, arg...) fprintf(stderr, f, ## arg)
#define output(f, arg...) fprintf(stdout, f, ## arg)

void usage() {
	debug("Usage:\n"
			"UPLOAD FILE\n"
			"striprados -p <poolname> -u <key> <filename>\n"
			"DOWNLOAD FILE\n"
			"striprados -p <poolname> -g <key> <filename>\n"
			"DELETE SINGLE FILE\n"
			"striprados -p <poolname> -r <key> [-f]\n"
			"DELETE MULTIPLE FILES\n"
			"striprados -p <poolname> -d <file-contains-keys> [-f]\n"
			"LIST ALL FILES\n"
			"striprados -p <poolname> -l\n"
			"ERASE OLD VER FILES SINCE DAYS GOES\n"
			"striprados -p <poolname> -e <days> [-f] [-m]\n");
	output("fail\n");
	
}
typedef struct {
	struct list_head job_list;
	pthread_mutex_t job_mutex;
	int count;
}remove_list,*remove_list_t;

typedef struct{
	struct list_head list;
	char *oid;
}remove_node,*remove_node_t;

typedef struct{
	rados_ioctx_t io_ctx;
	rados_striper_t striper;
}rm_args,*rm_args_t;

enum act {
 NOOPS = -1,
 DONWLOAD,
 UPLOAD,
 LIST ,
 DELETE,
 INFO,
 CLEAR
};


#define BUFFSIZE (32 << 20) /* 32M */
#define STRIPEUNIT (512 << 10) /* 512K */
#define OBJECTSIZE (64 << 20) /* 64M */
#define STRIPECOUNT 4 


int quit = 0;
remove_list rlist;
int force = 0;
int multi = 0;


int is_head_object(const char * entry) {
	const char *p;
	if((p = strrchr(entry, '.')) != NULL) {
		if (strncmp(p+1, "0000000000000000", 16) == 0)
			return p-entry;
	}
	return 0;
}


int is_ver_object(const char * obj_name){
	if (strncmp(obj_name, "ver_", 4) == 0)
		return 1;
	else
		return 0;
}

int try_break_lock(rados_ioctx_t io_ctx, rados_striper_t striper, char *oid){
	int exclusive;
	char tag[1024];
	char clients[1024];
	char cookies[1024];
	char addresses[1024];
	int ret = 0;
	size_t tag_len = 1024;
	size_t clients_len = 1024;
	size_t cookies_len = 1024;
	size_t addresses_len = 1024;
	char *firstObjOid = NULL;
	char *tail = ".0000000000000000";
	firstObjOid = malloc(strlen(oid)+strlen(tail)+1);
	strcpy(firstObjOid, oid);
	strcat(firstObjOid, tail);
	ret = rados_list_lockers(io_ctx, firstObjOid, "striper.lock", &exclusive, tag, &tag_len, clients, &clients_len, cookies, &cookies_len, addresses, &addresses_len);
	if (ret < 0){
		debug("%s rados_list_lockers failed errno: %d \n", oid, ret);
		goto out;
	}
	ret = rados_break_lock(io_ctx, firstObjOid, "striper.lock", clients, cookies);
	if (ret < 0){
		debug("%s rados_break_lock failed errno: %d \n", oid, ret);
		goto out;
	}
out:
	free(firstObjOid);
	return ret;
}

int striprados_remove(rados_ioctx_t io_ctx, rados_striper_t striper, char *oid){
	int ret;
	int retry = 0;
retry:
	ret = rados_striper_remove(striper, oid);
	if (ret == -EBUSY && force == 1 && retry == 0){
		ret = try_break_lock(io_ctx,striper,oid);
		retry++;
		if (ret == 0)
			goto retry;
	}
	if (ret < 0) {
		debug("%s delete failed errno: %d \n", oid, ret);
	}else{
		debug("%s deleted\n",oid);
	}
	return ret;
}

struct entry_cache {

};

int is_cached(const char * entry) {
	return 1;
}

int do_ls(rados_ioctx_t ioctx) {
	int ret;
	const char *entry;
	rados_list_ctx_t list_ctx;
	char buf[128];
	int length;
	ret = rados_objects_list_open(ioctx, &list_ctx);
	if (ret < 0) {
		debug("error reading list");
		return -1;
	}
	debug("===striper objects list===\n");
	while(!quit && rados_objects_list_next(list_ctx, &entry, NULL) != -ENOENT) {
		if (is_cached(entry) == 0)
			continue;
		if ((length = is_head_object(entry)) == 0)
			continue;
		memset(buf, 0, 128);
		if (rados_getxattr(ioctx, entry, "striper.size", buf, 128) > 0) {
			output("%-10.*s|%-10s\n", length, entry, buf);
		} else {
			debug("can not get striper.size of %s", entry);
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
		debug("failed to allocate free buffer manager\n");
		return -1;
	}
	bm->free_buf[0] = calloc(BUFFSIZE, sizeof(char));

	if (bm->free_buf[0] == NULL) {
		debug("failed to allocate the first buffer\n");
		ret = -1;
		goto out;
	}
	bm->index = 0;
	bm->max_buf_num = concurrent;
	/* current alrealy allocated buf */
	bm->current_buf_num = 1;
	
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



void quit_handler(int i)
{
	quit = 1;
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
		debug("failed to create buffer_manager\n");
		return -1;
	}
	
	int fd = open(filename, O_RDONLY);
	if (fd < 0) {
		debug("error reading file %s", filename);
		ret = -1;
		goto out;
	}
	/* check the file size */
	struct stat sb;
	fstat(fd, &sb);
	if (sb.st_size <= 0) {
		ret = -1;
		debug("the size of file %s is 0\n", filename);
		goto checkfilefail;
	}

	if (overwrite == 1)
		rados_striper_trunc(striper, key, 0);

	count = BUFFSIZE;
	while (count != 0 && !quit) {

		/* it may block */
		buf = get_free_buffer(&bm);

		/* can not allocate new buffer lazily, continue */
		if (buf == NULL) {
			debug("failed to get buf\n");
			continue;
		}

		count = read(fd, buf, BUFFSIZE);

		if (count < 0) {
			put_buffer_back(&bm, buf);
			debug("failed to read from file\n");
			ret = -1;
			break;
		}

		if (count == 0) {
			put_buffer_back(&bm, buf);
			ret = 0;
			break;
		}

		/* use completion_list to store every completion_list  */
		ret = rados_aio_create_completion((void *)buf, set_completion_complete, NULL, &my_completion);
		if (ret < 0) {
			debug("failed to create completion\n");
			goto out1;
		}
		if (next_num_writes == capacity - 1) {
			completion_list =  realloc(completion_list, (capacity << 1) * sizeof(rados_completion_t));
			capacity = capacity << 1;
		}
		completion_list[next_num_writes] = my_completion;
		next_num_writes ++;

		rados_striper_aio_write(striper, key, my_completion, buf, count, offset);

		offset += count;
		debug("%lu%%\r", offset * 100 / sb.st_size);
		fflush(stderr);
	}
	
out1:


	for(i = 0 ; i < next_num_writes ; i ++) {
		rados_aio_wait_for_safe(completion_list[i]);
		rados_aio_release(completion_list[i]);
	}
	rados_striper_aio_flush(striper);
	if(completion_list)
		free(completion_list);

checkfilefail:	
	close(fd);
out:
	destory_buffer_manager(&bm);

	return ret;
}

/* sync io */
int do_put(rados_ioctx_t ioctx, rados_striper_t striper, const char *key, const char *filename) {

	struct stat sb;
	int count;
	int ret = -1;
	uint64_t offset;
	uint64_t file_size;
	char numbuf[128];
	memset(numbuf, 0, 128);

	char *buf = (char*)malloc(BUFFSIZE);
	if (buf == NULL) 
	  return -1;

	int fd = open(filename, O_RDONLY);
	if (fd < 0) {
		debug("error reading file %s", filename);
		ret = -1;
		goto out1;
	}


	count = BUFFSIZE;
	fstat(fd,&sb);
	file_size = sb.st_size;
	if (file_size < 3) {
		ret = -1;
		goto out2;
	}


	offset = 0;

	while (count != 0 && !quit) {
		count = read(fd, buf, BUFFSIZE);
		if (count < 0) {
			ret = -1;
			break;
		}
		if (count == 0) {
			ret = 0;
			break;
		}
		ret = rados_striper_write(striper, key, buf, count, offset);
		if (ret != 0)
			break;
		offset += count;
		debug("%lu%%\r", offset*100/file_size);
		fflush(stdout);
	}

out2:
	close(fd);
out1:
	free(buf);

	/* if interrupted, return -1 */
	if (quit == 1)
		return -1;
	return ret;
}


int do_get(rados_ioctx_t ioctx, rados_striper_t striper, const char *key, const char *filename) {

	char numbuf[128];
	uint64_t offset = 0;
	int count = 0;
	uint64_t file_size;
	int ret = 0;
	memset(numbuf, 0, 128);

	char *buf = malloc(BUFFSIZE);
	if (buf == NULL) {
		ret = -1;
	}
	memset(buf, 0, BUFFSIZE);


	char * sobj = malloc(strlen(key) + 17 + 1);
	if (sobj == NULL) {
		ret = -1;
		goto out;
	}

	sprintf(sobj,"%s.%016d", key, 0);

	if (rados_getxattr(ioctx, sobj, "striper.size", numbuf, 128) > 0) {
		sscanf(numbuf, "%lu", &file_size);
	} else {
		ret = -1;
		debug("no remote file or the file is not striped: %s\n", key);
		goto out1;;
	}

	int fd = open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0644);
	if (fd < 0) {
		debug("error writing file %s\n", filename);
		ret = -1;
		goto out1;
	}
	
	while (!quit) {
		count = rados_striper_read(striper, key, buf, BUFFSIZE, offset);
		if (count < 0) {
			debug("error reading rados file %s", key);
			ret = -1;
			break;
		}
		if (count == 0) {
			ret = 0;
			break;
		}
		if (write(fd, buf, count) < 0){ 
			ret = -1;
			break;
		}
		offset += count;
		debug("%lu%%\r", offset*100/file_size);
		fflush(stdout);
	}

	close(fd);
out1:
	free(sobj);
out:
	free(buf);

	/* if interrupted, return -1 */
	if (quit == 1)
		return -1;
	return ret;
}

int do_delete(rados_ioctx_t ioctx, rados_striper_t striper, char *key, const char * file) {
	int ret;
	/* delete single key */
	if (file == NULL) {
		debug("deleting %s\n",key);
		ret = striprados_remove(ioctx, striper, key);
		if (ret < 0) {
			debug("%s delete failed\n", key);
			return -1;
		}
		debug("%s deleted\n",key);
		return 0;
	}

	/* delete key from file */
	char *line = NULL;
	char *real_key = NULL;
	char *p = NULL;

	size_t len = 0;
	ssize_t read;
	FILE *fp = fopen(file, "r");
	int counts = 0;
	if (fp == NULL) {
		debug("can not open %s\n", file);
		return -1;
	}

	while(!quit && (read = getline(&line, &len, fp)) != -1) {

		/* get rid of newline character */
		/* unix \n	*/
		/* dos \r\n */
		if(line[read - 1] == '\n') {
			line[read - 1] = '\0';
			if (read >= 2 && line[read - 2 ] == '\r')
				line[read -2 ] = '\0';
		} else {
			debug("read file line %s failed\n",  line);
			return -1;
		}

		p = line;
		/* skip space in the front */
		while (*p == ' ' || *p == '\t')
			p ++;
		real_key = p;
		/* skip space behind the real key */
		while (*p != ' ' &&  *p != '\t' && *p != '\0')
			p ++;
		*p = '\0';

		/* skip empty line */
		if (strlen(real_key) < 1)
			continue;

		debug("deleting key:%s\n", real_key);
		ret = striprados_remove(ioctx, striper, real_key);
		if (ret < 0) {
			debug("deleting:%s failed\n", real_key);
			continue;
		}
		counts ++;
	}

	if (line)
		free(line);
	fclose(fp);

	if (counts == 0) {
		debug("No Object was deleted\n");
		return -1;
	}

	return 0;
}

int do_info(rados_striper_t striper, const char *key) {
	uint64_t size;
	time_t mod_time;
	int ret;
	char buffer[50];
	ret = rados_striper_stat(striper, key, &size, &mod_time);
	if (ret < 0) {
		debug("no such object\n");
		return -1;
	}
	strftime(buffer, 50, "%Y/%m/%d-%H:%M:%S", localtime(&mod_time));
	output("%-s|%"PRIu64"|%s\n", key, size, buffer);
	return 0;
}

void add_obj_to_list(remove_list_t plist, char *oid){
	remove_node_t pnode = (remove_node_t)malloc(sizeof(remove_node));
	if (pnode == NULL) {
		debug("malloc failed in %d",__LINE__);
		return;
	}
	pnode->oid = strdup(oid);
	pthread_mutex_lock(&plist->job_mutex);
	list_add_tail(&pnode->list,&plist->job_list);
	plist->count++;
	pthread_mutex_unlock(&plist->job_mutex);
}

char *get_obj_from_list(remove_list_t plist)
{
	remove_node_t pos,n;
	char *ret = NULL;
	int find = 0;
	pthread_mutex_lock(&plist->job_mutex);
	list_for_each_entry_safe(pos, n, &plist->job_list, list){
		find = 1;
		ret = pos->oid;
		break;
	}
	if (find == 1){
		
		list_del(&pos->list);
		plist->count--;
		free(pos);
	}
	pthread_mutex_unlock(&plist->job_mutex);
	return ret;
}

void process_remove_ver_objs(void *arg){
	rm_args_t agrs = (rm_args_t)arg;
	rados_ioctx_t io_ctx = agrs->io_ctx;
	rados_striper_t striper = agrs->striper;
	char *oid = NULL;
	int ret = 0;
	while(1){
		oid = get_obj_from_list(&rlist);
		if (oid == NULL){
			break;
		}
		ret = striprados_remove(io_ctx, striper, oid);
		free(oid);
		if (ret != 0){
			break;
		}
			
	}
	free(arg);
	return;
}


int do_clear_old_files(rados_striper_t striper, rados_ioctx_t ioctx, const char *key, int force) {
	int ret;
	const char *entry;
	rados_list_ctx_t list_ctx;
	char buf[128];
	int length;
	uint64_t size;
	time_t mod_time;
	time_t now_time;
	int date_of_expiry = atoi(key)*24*60*60;
	rm_args_t args = NULL;
	threadpool tp;
	tp = create_threadpool(50);
	ret = rados_objects_list_open(ioctx, &list_ctx);
	if (ret < 0) {
			debug("error reading list");
			return -1;
	}
	debug("===start delete objects ===\n");
	while(!quit && rados_objects_list_next(list_ctx, &entry, NULL) != -ENOENT) {
		if (is_cached(entry) == 0)
			continue;
		if ((length = is_head_object(entry)) == 0)
			continue;
		memset(buf, 0, sizeof(buf));
		strncpy(buf, entry, length);
		if (!is_ver_object(buf))
			continue;
		ret = rados_striper_stat(striper, buf, &size, &mod_time);
		if (ret < 0) {
			debug("no such object\n");
			return -1;
		}
		time(&now_time);
		if ((now_time - mod_time) > date_of_expiry){
			if (multi){
				add_obj_to_list(&rlist, buf);
				args = (rm_args_t)malloc(sizeof(rm_args));
				args->io_ctx = ioctx;
				args->striper = striper;
				dispatch_threadpool(tp, process_remove_ver_objs, (void *)args);
			}else{
				striprados_remove(ioctx, striper, buf);
			}
		}
	}
	
	rados_objects_list_close(list_ctx);
	while(!list_empty(&rlist.job_list)){
		sleep(5);
	}
	destroy_threadpool(tp);
	debug("===all objects deleted complete ===\n");
	return 0;
}

int main(int argc, const char **argv)
{

	int opt;
	char *pool_name = NULL;
	char *key = NULL;
	const char *filename = NULL;
	const char *to_delete_file_list = NULL;
	int ret = 0;
	enum act action = NOOPS;
	pthread_mutex_init(&rlist.job_mutex, NULL);
	INIT_LIST_HEAD(&rlist.job_list);
	rlist.count = 0;
	time_t startT, endT;
	double totalT;
	startT = time(NULL);
	while ((opt = getopt(argc, (char* const *) argv, "d:p:u:g:mflr:i:e:")) != -1) {
		switch (opt) {
			case 'd':
				action = DELETE;
				to_delete_file_list = optarg;
				break;
			case 'p':
				pool_name = optarg;
				break;
			case 'f':
				force = 1;
				break;
			case 'u':
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
			case 'e':
				action = CLEAR;
				key = optarg;
				break;
			case 'm':
				multi = 1;
				break;
			default:
				usage();
				return EXIT_FAILURE;
		}
	}

	/* check parameters */
	if (action == UPLOAD || action	== DONWLOAD) {
		if (argc == optind + 1 && pool_name) {
			filename = argv[optind];
		} else {
			usage();
			return EXIT_FAILURE;
		}
	} else if ((action == LIST || action == DELETE || action == INFO ) && pool_name) {
		/* pass */
		
	} else if (action == DELETE || to_delete_file_list != NULL) {
		/* pass */
	} else if (action == DELETE || key != NULL) {
		/* pass */
	} else if (action == CLEAR || key != NULL) {
		/* pass */
	} else {
			usage();
			return EXIT_FAILURE;
	}

	rados_ioctx_t io_ctx = NULL;
	rados_striper_t striper = NULL;

	rados_t rados = NULL;
	ret = rados_create(&rados, "admin"); // just use the client.admin keyring
	if (ret < 0) { // let's handle any error that might have come back
		debug("couldn't initialize rados! error %d\n", ret);
		ret = EXIT_FAILURE;
		goto out;
	}
	debug("set up a rados cluster object\n");

	rados_conf_set(rados, "rados_mon_op_timeout", "60");
	rados_conf_set(rados, "rados_osd_op_timeout", "180");
	ret = rados_conf_read_file(rados, "/etc/ceph/ceph.conf");

	ret = rados_connect(rados);
	if (ret < 0) {
		debug("couldn't connect to cluster! error %d\n", ret);
		ret = EXIT_FAILURE;
		goto out;
	}
	debug("connected to the rados cluster\n");


	ret = rados_ioctx_create(rados, pool_name, &io_ctx);
	if (ret < 0) {
		debug("couldn't set up ioctx! error %d\n", ret);
		ret = EXIT_FAILURE;
		goto out;
	} else
		debug("created an ioctx for our pool\n");

	ret = rados_striper_create(io_ctx, &striper);
	if (ret < 0) {
		debug("couldn't set up striper error %d\n", ret);
		ret = EXIT_FAILURE;
		goto out;
	} else {
		debug("created a striper for our pool\n");
	}


	rados_striper_set_object_layout_stripe_unit(striper, STRIPEUNIT);
	rados_striper_set_object_layout_object_size(striper, OBJECTSIZE);
	rados_striper_set_object_layout_stripe_count(striper, STRIPECOUNT);


	struct sigaction sa;
	memset(&sa, 0, sizeof(sa) );
	sa.sa_handler = quit_handler;
	sigfillset(&sa.sa_mask);
	sigaction(SIGINT,&sa,NULL);
	sigaction(SIGTERM,&sa,NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);

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
			ret = do_delete(io_ctx, striper, key, to_delete_file_list);
			break;
		case INFO:
			ret = do_info(striper, key);
			break;
		case CLEAR:
			ret = do_clear_old_files(striper, io_ctx, key, force);
			break;
		default:
			output("fail\n");
			ret = -1;
			goto out;
	}
	

out:
	if (striper) 
		rados_striper_destroy(striper);
	if (io_ctx) 
		rados_ioctx_destroy(io_ctx);
	if (rados) 
			rados_shutdown(rados);
	endT = time(NULL);
	totalT = endT-startT;
	debug("time cost %lf second\n",totalT);
	if(ret == 0)
		output("success\n");
	else
		output("fail\n");
	return ret;
}
