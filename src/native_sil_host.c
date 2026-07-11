/* SPDX-License-Identifier: Apache-2.0 */

#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

void *rdd2_native_sil_host_map(const char *path, unsigned long size)
{
	int fd = open(path, O_RDWR);
	struct stat status;
	void *mapping;

	if (fd < 0) {
		return NULL;
	}
	if (fstat(fd, &status) != 0 || status.st_size < (off_t)size) {
		close(fd);
		return NULL;
	}
	mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	return mapping == MAP_FAILED ? NULL : mapping;
}

void rdd2_native_sil_host_unmap(void *mapping, unsigned long size)
{
	if (mapping != NULL) {
		(void)munmap(mapping, size);
	}
}
