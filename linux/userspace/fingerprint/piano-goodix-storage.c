// SPDX-License-Identifier: BSD-3-Clause
/* Restricted FS/GPFS diagnostic services using Qualcomm's public packed messages.
 * CRegisterListenerCBO and IListenerCBO operations follow minkipc's IDLs.
 * Requests carry no TA identity. Only the explicit directory is accessible.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "MinkCom.h"
#include "fs_msg.h"
#include "gpfs_msg.h"
#include "piano-goodix-storage.h"

/* The factory TA uses legacy IDs 0..3 with the public GPFS message layout. */
enum { GPFS_READ, GPFS_WRITE, GPFS_REMOVE, GPFS_RENAME };

_Static_assert(sizeof(tz_file_open_req_t) == 264 &&
	       offsetof(tz_file_open_req_t, flags) == 260, "FS open request ABI");
_Static_assert(sizeof(tz_file_close_req_t) == 8 && sizeof(tz_file_open_rsp_t) == 8 &&
	       sizeof(tz_file_close_rsp_t) == 8 && sizeof(tz_file_get_errno_rsp_t) == 8,
	       "FS close/response ABI");

struct storage_listener {
	struct goodix_storage *storage;
	Object registration, memory;
	void *shared, *work;
	size_t size;
	uint32_t service;
};

struct goodix_storage {
	atomic_uint references;
	pthread_mutex_t mutex;
	struct storage_listener listeners[2];
	struct { int fd, handle; } files[FOPEN_MAX];
	int next_handle, file_error;
	int directory;
	int stopped;
};

static int storage_filename(const char path[TZ_CM_MAX_NAME_LEN], const char **name)
{
	const char prefix[] = "/data/vendor_de/0/fpdata/";
	size_t length = strnlen(path, TZ_CM_MAX_NAME_LEN);

	if (length == TZ_CM_MAX_NAME_LEN ||
	    strncmp(path, prefix, sizeof(prefix) - 1))
		return EACCES;
	*name = path + sizeof(prefix) - 1;
	if (!**name || !strcmp(*name, ".") || !strcmp(*name, ".."))
		return EACCES;
	for (const unsigned char *p = (const unsigned char *)*name; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.'))
			return EACCES;
	return 0;
}

static void log_path(const char *path)
{
	fputc('"', stderr);
	for (size_t i = 0; i < TZ_CM_MAX_NAME_LEN && path[i]; i++) {
		unsigned char c = path[i];
		if (c >= 32 && c < 127 && c != '"' && c != '\\')
			fputc(c, stderr);
		else
			fprintf(stderr, "\\x%02x", c);
	}
	fputc('"', stderr);
}

static int storage_file(struct goodix_storage *storage, const char *name, int flags)
{
	struct stat st;
	int fd = openat(storage->directory, name,
			flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);

	if (fd < 0)
		return -errno;
	if (fstat(fd, &st)) {
		int error = errno;
		close(fd);
		return -error;
	}
	if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid() ||
	    st.st_size < 0 || st.st_size > TZ_GPFS_FILE_SIZE) {
		close(fd);
		return -EACCES;
	}
	return fd;
}

/* FTS checks existence with plain open/close before asking GPFS to unseal data.
 * Export private handles, never process file descriptors supplied by QTEE.
 */
static int storage_fs_dispatch(struct goodix_storage *storage, void *buffer, size_t size)
{
	tz_file_open_req_t request;
	tz_file_open_rsp_t response = {0};
	char pathname[TZ_CM_MAX_NAME_LEN] = {0};
	const char *name;
	int error = 0, handle = -1, flags = 0;

	if (size < sizeof(response))
		return -EMSGSIZE;
	memcpy(&response.cmd_id, buffer, sizeof(response.cmd_id));
	response.ret = -1;
	if (response.cmd_id == TZ_FS_MSG_CMD_FILE_OPEN) {
		if (size < sizeof(request))
			return -EMSGSIZE;
		memcpy(&request, buffer, sizeof(request));
		memcpy(pathname, request.pathname, sizeof(pathname));
		flags = request.flags;
		error = storage_filename(pathname, &name);
		if (!error && flags != O_RDONLY)
			error = EROFS;
		if (!error) {
			size_t slot;
			for (slot = 0; slot < FOPEN_MAX && storage->files[slot].handle; slot++)
				;
			if (slot == FOPEN_MAX || storage->next_handle == INT32_MAX) {
				error = EMFILE;
			} else {
				int fd = storage_file(storage, name, O_RDONLY);
				if (fd < 0) {
					error = -fd;
				} else {
					storage->files[slot].fd = fd;
					response.ret = ++storage->next_handle;
					storage->files[slot].handle = response.ret;
				}
			}
		}
	} else if (response.cmd_id == TZ_FS_MSG_CMD_FILE_CLOSE) {
		tz_file_close_req_t close_request;
		if (size < sizeof(close_request))
			return -EMSGSIZE;
		memcpy(&close_request, buffer, sizeof(close_request));
		handle = close_request.fd;
		error = EBADF;
		for (size_t slot = 0; slot < FOPEN_MAX; slot++) {
			if (!handle || storage->files[slot].handle != handle)
				continue;
			int result = close(storage->files[slot].fd);
			error = result ? errno : 0;
			storage->files[slot].handle = 0;
			response.ret = result;
			break;
		}
	} else if (response.cmd_id == TZ_FS_MSG_CMD_FILE_GET_ERRNO) {
		response.ret = storage->file_error;
	} else {
		storage->file_error = EOPNOTSUPP;
		fprintf(stderr, "FS command=%u unsupported\n", response.cmd_id);
		return -EOPNOTSUPP;
	}
	if (error)
		storage->file_error = error;
	fprintf(stderr, "FS command=%u path=", response.cmd_id);
	log_path(pathname);
	fprintf(stderr, " flags=%d handle=%d result=%d errno=%d\n",
		flags, handle, response.ret, error);
	memset(buffer, 0, size);
	memcpy(buffer, &response, sizeof(response));
	return 0;
}

/* Operate on one bounded flat file; no path resolution or directory creation. */
static int storage_dispatch(struct goodix_storage *storage, void *buffer, size_t size)
{
	tz_gpfile_read_req_t read_request;
	tz_gpfile_read_rsp_t *response = buffer;
	char pathname[TZ_CM_MAX_NAME_LEN] = {0};
	const char *name = NULL;
	uint32_t command, count = 0, backup = 0;
	int32_t offset = 0;
	int error = EOPNOTSUPP, fd = -1;
	size_t done = 0;

	if (size < sizeof(*response) || size < sizeof(tz_gpfile_write_req_t))
		return -EMSGSIZE;
	memcpy(&command, buffer, sizeof(command));
	if (command == TZ_GPFS_MSG_CMD_GPFS_VERSION) {
		tz_gpfile_version_rsp_t version = {command, GP_FS_VERSION, 0};
		memset(buffer, 0, size);
		memcpy(buffer, &version, sizeof(version));
		fprintf(stderr, "GPFS command=%u version=%u\n", command, GP_FS_VERSION);
		return 0;
	}
	if (command <= GPFS_RENAME) {
		memcpy(&read_request, buffer, sizeof(read_request));
		memcpy(pathname, read_request.pathname, sizeof(pathname));
		error = storage_filename(pathname, &name);
		if (error)
			goto reply;
	}
	if (command == GPFS_READ || command == GPFS_WRITE) {
		offset = read_request.offset;
		count = read_request.count;
		if (offset < 0 || count > TZ_GPFS_FILE_SIZE ||
		    (uint32_t)offset > TZ_GPFS_FILE_SIZE - count) {
			error = EINVAL;
			goto reply;
		}
		if (command == GPFS_WRITE) {
			tz_gpfile_write_req_t *request = buffer;
			backup = request->backup;
			/* Do not silently omit a requested backup in this empty-store probe. */
			if (backup) {
				error = EOPNOTSUPP;
				goto reply;
			}
		}
		fd = storage_file(storage, name, command == GPFS_READ ? O_RDONLY :
				  O_RDWR | O_CREAT | O_SYNC);
		if (fd < 0) {
			error = -fd;
			goto reply;
		}
		error = 0;
		while (done < count) {
			ssize_t bytes;
			if (command == GPFS_READ)
				bytes = pread(fd, response->buf + done, count - done, offset + done);
			else
				bytes = pwrite(fd, ((tz_gpfile_write_req_t *)buffer)->buf + done,
					       count - done, offset + done);
			if (bytes < 0 && errno == EINTR)
				continue;
			if (bytes < 0 || (!bytes && command == GPFS_WRITE)) {
				error = bytes < 0 ? errno : EIO;
				break;
			}
			if (!bytes)
				break;
			done += bytes;
		}
		if (!error && command == GPFS_WRITE && (fsync(fd) || fsync(storage->directory)))
			error = errno;
		close(fd);
	} else if (command == GPFS_REMOVE || command == GPFS_RENAME) {
		/* Initialization may request cleanup. Diagnose it without deleting data. */
		error = EROFS;
	}
reply:
	fprintf(stderr, "GPFS command=%u path=", command);
	log_path(pathname);
	fprintf(stderr, " offset=%d count=%u backup=%u result=%d transferred=%zu\n",
		offset, count, backup, error, error ? 0 : done);
	if (command != GPFS_READ || error)
		memset(buffer, 0, size);
	response->cmd_id = command;
	response->err = error;
	response->num_bytes_read = error ? 0 : done;
	return 0;
}

static int32_t storage_invoke(ObjectCxt context, ObjectOp operation,
			      ObjectArg *args, ObjectCounts counts)
{
	struct storage_listener *listener = (struct storage_listener *)context;
	struct goodix_storage *storage = listener->storage;
	operation = ObjectOp_methodID(operation);
	if (operation == Object_OP_retain) {
		atomic_fetch_add(&storage->references, 1);
		return Object_OK;
	}
	if (operation == Object_OP_release) {
		if (atomic_fetch_sub(&storage->references, 1) == 1) {
			for (size_t i = 0; i < 2; i++) {
				Object_RELEASE_IF(storage->listeners[i].memory);
				free(storage->listeners[i].work);
			}
			for (size_t i = 0; i < FOPEN_MAX; i++)
				if (storage->files[i].handle)
					close(storage->files[i].fd);
			close(storage->directory);
			pthread_mutex_destroy(&storage->mutex);
			free(storage);
		}
		return Object_OK;
	}
	if (operation == 1 && counts == ObjectCounts_pack(0, 0, 0, 0)) {
		pthread_mutex_lock(&storage->mutex);
		pthread_mutex_unlock(&storage->mutex);
		return Object_OK;
	}
	/* IListenerCBO.request(out offsets, out is64, out smo1..4). */
	if (operation || counts != ObjectCounts_pack(0, 2, 0, 4) ||
	    args[1].b.size != sizeof(uint32_t))
		return Object_ERROR_INVALID;
	args[0].b.size = 0;
	memset(args[1].b.ptr, 0, sizeof(uint32_t));
	for (size_t i = 2; i < 6; i++)
		args[i].o = Object_NULL;
	if (pthread_mutex_trylock(&storage->mutex))
		return Object_ERROR_BUSY;
	int result = -ECANCELED;
	if (!storage->stopped) {
		memcpy(listener->work, listener->shared, listener->size);
		result = listener->service == FILE_SERVICE_ID ?
			storage_fs_dispatch(storage, listener->work, listener->size) :
			storage_dispatch(storage, listener->work, listener->size);
		memcpy(listener->shared, listener->work, listener->size);
	}
	pthread_mutex_unlock(&storage->mutex);
	return result ? Object_ERROR : Object_OK;
}

static int storage_directory(const char *directory)
{
	char *path, *component, *next = NULL;
	int fd, result, first = 1;

	if (!directory || directory[0] != '/')
		return -EACCES;
	path = strdup(directory);
	if (!path)
		return -ENOMEM;
	fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) {
		free(path);
		return -errno;
	}
	for (component = strtok_r(path, "/", &next); component;
	     component = strtok_r(NULL, "/", &next)) {
		if (!strcmp(component, ".") || !strcmp(component, "..") ||
		    (first && (!strcmp(component, "data") || !strcmp(component, "persist")))) {
			result = -EACCES;
			goto failed;
		}
		first = 0;
		int child = openat(fd, component,
				   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (child < 0) {
			result = -errno;
			goto failed;
		}
		close(fd);
		fd = child;
	}
	free(path);
	return fd;
failed:
	close(fd);
	free(path);
	return result;
}

int goodix_storage_open(const char *directory, struct goodix_storage **output)
{
	struct goodix_storage *storage = NULL;
	struct stat st;
	Object root = Object_NULL, environment = Object_NULL;
	DIR *entries = NULL;
	int fd = -1, result = -EINVAL;
	uint32_t uid = 87;

	*output = NULL;
	fd = storage_directory(directory);
	if (fd < 0)
		return fd;
	if (fstat(fd, &st)) {
		result = -errno;
		goto done;
	}
	if (st.st_uid != geteuid() || (st.st_mode & 0777) != 0700) {
		result = -EACCES;
		goto done;
	}
	if (flock(fd, LOCK_EX | LOCK_NB)) {
		result = -errno;
		goto done;
	}
	int scan = dup(fd);
	if (scan < 0) {
		result = -errno;
		goto done;
	}
	entries = fdopendir(scan);
	if (!entries) {
		result = -errno;
		close(scan);
		goto done;
	}
	errno = 0;
	for (struct dirent *entry; (entry = readdir(entries));)
		if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
			result = -ENOTEMPTY;
			goto done;
		}
	if (errno) {
		result = -errno;
		goto done;
	}
	storage = calloc(1, sizeof(*storage));
	if (!storage) {
		result = -ENOMEM;
		goto done;
	}
	atomic_init(&storage->references, 1);
	pthread_mutex_init(&storage->mutex, NULL);
	storage->directory = fd;
	fd = -1;
	for (size_t i = 0; i < 2; i++)
		storage->listeners[i].storage = storage;
	if (MinkCom_getRootEnvObject(&root) ||
	    MinkCom_getClientEnvObject(root, &environment)) {
		result = -EIO;
		goto done;
	}
	for (size_t i = 0; i < 2; i++) {
		struct storage_listener *listener = &storage->listeners[i];
		size_t bytes = i ? FILE_SERVICE_BUF_LEN : GPFILE_SERVICE_BUF_LEN;
		listener->service = i ? FILE_SERVICE_ID : GPFILE_SERVICE_ID;
		Object callback = {storage_invoke, (ObjectCxt)listener};
		ObjectArg open[] = {{.bi = {&uid, sizeof(uid)}}, {.o = Object_NULL}};
		if (Object_invoke(environment, 0, open, ObjectCounts_pack(1, 0, 0, 1))) {
			result = -EIO;
			goto done;
		}
		listener->registration = open[1].o;
		if (MinkCom_getMemoryObject(root, bytes, &listener->memory) ||
		    MinkCom_getMemoryObjectInfo(listener->memory, &listener->shared,
					&listener->size) || listener->size < bytes) {
			result = -ENOMEM;
			goto done;
		}
		listener->work = calloc(1, listener->size);
		if (!listener->work) {
			result = -ENOMEM;
			goto done;
		}
		ObjectArg register_args[] = {
			{.bi = {&listener->service, sizeof(listener->service)}},
			{.o = callback}, {.o = listener->memory},
		};
		int status = Object_invoke(listener->registration, 0, register_args,
					   ObjectCounts_pack(1, 0, 2, 0));
		if (status) {
			fprintf(stderr, "Storage service %u registration failed: %d\n",
				listener->service, status);
			result = -EBUSY;
			goto done;
		}
	}
	*output = storage;
	storage = NULL;
	result = 0;
done:
	if (entries)
		closedir(entries);
	if (fd >= 0)
		close(fd);
	Object_RELEASE_IF(environment);
	Object_RELEASE_IF(root);
	goodix_storage_close(storage);
	return result;
}

void goodix_storage_close(struct goodix_storage *storage)
{
	if (!storage)
		return;
	pthread_mutex_lock(&storage->mutex);
	storage->stopped = 1;
	pthread_mutex_unlock(&storage->mutex);
	for (size_t i = 0; i < 2; i++)
		Object_RELEASE_IF(storage->listeners[i].registration);
	Object callback = {storage_invoke, (ObjectCxt)&storage->listeners[0]};
	Object_release(callback);
}
