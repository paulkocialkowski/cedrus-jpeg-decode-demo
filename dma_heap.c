/*
 * Copyright (C) 2024 Paul Kocialkowski <contact@paulk.fr>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <linux/dma-heap.h>

#include "dma_heap.h"

int dma_heap_open(const char *name)
{
	const char *path_format = "/dev/dma_heap/%s";
	char path[PATH_MAX];
	int ret;
	int fd;

	if (!name)
		return -EINVAL;

	ret = snprintf(path, PATH_MAX, path_format, name);
	if (ret < 0)
		return -EINVAL;

	fd = open(path, O_RDWR);
	if (fd < 0)
		return -errno;

	return fd;
}

int dma_heap_alloc(int fd, unsigned int size, int flags)
{
	struct dma_heap_allocation_data allocation_data = { };
	int ret;

	allocation_data.len = size;
	allocation_data.fd_flags = flags;

	ret = ioctl(fd, DMA_HEAP_IOCTL_ALLOC, &allocation_data);
	if (ret)
		return -errno;

	return allocation_data.fd;
}
