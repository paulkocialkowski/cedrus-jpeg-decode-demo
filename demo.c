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
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/dma-buf.h>
#include <linux/media.h>
#include <linux/videodev2.h>

#include <libudev.h>

#include "demo.h"
#include "dma_buf.h"
#include "dma_heap.h"
#include "v4l2.h"
#include "media.h"
#include "perf.h"

long demo_buffer_sync_flags(struct demo_buffer *buffer)
{
	unsigned int type_base;

	if (!buffer)
		return -EINVAL;

	type_base = v4l2_type_base(buffer->buffer.type);
	if (type_base == V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return DMA_BUF_SYNC_WRITE;
	else if (type_base == V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return DMA_BUF_SYNC_READ;
	else
		return 0;
}

int demo_buffer_sync(struct demo_buffer *buffer, long flags)
{
	unsigned int i;
	int ret;

	if (!buffer)
		return -EINVAL;

	if (buffer->dma_buf_fd[0] < 0)
		return 0;

	for (i = 0; i < buffer->planes_count; i++) {
		ret = dma_buf_sync(buffer->dma_buf_fd[i], flags);
		if (ret)
			return ret;
	}

	return 0;
}

int demo_buffer_sync_begin(struct demo_buffer *buffer)
{
	long flags = demo_buffer_sync_flags(buffer);

	return demo_buffer_sync(buffer, flags | DMA_BUF_SYNC_START);
}

int demo_buffer_sync_finish(struct demo_buffer *buffer)
{
	long flags = demo_buffer_sync_flags(buffer);

	return demo_buffer_sync(buffer, flags | DMA_BUF_SYNC_END);
}

int demo_buffer_setup_base(struct demo_buffer *buffer, int video_fd,
			   unsigned int memory, unsigned int type,
			   unsigned int index, unsigned int planes_count)
{
	struct v4l2_plane *planes;
	unsigned int i;
	int ret;

	if (v4l2_type_mplane_check(type)) {
		if (planes_count > 4)
			return -EINVAL;
	} else {
		if (planes_count > 1)
			return -EINVAL;
	}

	buffer->planes_count = planes_count;

	for (i = 0; i < planes_count; i++)
		buffer->dma_buf_fd[i] = -1;

	v4l2_buffer_setup_base(&buffer->buffer, type, memory);
	v4l2_buffer_setup_index(&buffer->buffer, index);
	v4l2_buffer_setup_planes(&buffer->buffer, buffer->planes, planes_count);

	ret = v4l2_buffer_query(video_fd, &buffer->buffer);
	if (ret) {
		fprintf(stderr, "Failed to query buffer\n");
		return ret;
	}

	return 0;
}

int demo_buffer_setup_import(struct demo *demo, struct demo_buffer *buffer,
			     struct demo_buffer *import_buffer,
			     int import_video_fd)
{
	unsigned int import_length;
	unsigned int length;
	unsigned int i;
	void *data;
	int fd;
	int ret;

	if (buffer->planes_count != import_buffer->planes_count)
		return -EINVAL;

	for (i = 0; i < buffer->planes_count; i++) {
		v4l2_buffer_plane_length(&import_buffer->buffer, i,
					 &import_length);
		v4l2_buffer_plane_length(&buffer->buffer, i, &length);

		if (import_length < length)
			return -EINVAL;

		/* Reuse dma-heap fd if possible or export dma-buf fds. */
		if (demo->allocator == DEMO_ALLOCATOR_DMA_HEAP) {
			fd = import_buffer->dma_buf_fd[i];
			if (fd < 0)
				return -EINVAL;
		} else {
			ret = v4l2_buffer_export(import_video_fd,
						 &import_buffer->buffer, i,
						 O_RDWR, &fd);
			if (ret)
				return ret;

			/* Duplicate file descriptor for reference count. */
			buffer->dma_buf_fd[i] = dup(fd);
		}

		v4l2_buffer_setup_fd(&buffer->buffer, i, fd);

		data = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED,
			    fd, 0);
		if (data == MAP_FAILED) {
			/* TODO: Cleanup previous mappings on error. */
			return -ENOMEM;
		}

		buffer->data[i] = data;
	}

	return 0;
}

int demo_buffer_setup_dma_heap(struct demo *demo, struct demo_buffer *buffer,
			    int video_fd)
{
	unsigned int length;
	unsigned int i;
	void *data;
	int fd;
	int ret;

	for (i = 0; i < buffer->planes_count; i++) {
		v4l2_buffer_plane_length(&buffer->buffer, i, &length);

		fd = dma_heap_alloc(demo->dma_heap_fd, length, O_RDWR);
		if (fd < 0) {
			/* TODO: Cleanup previous allocations/mappings on error. */
			return -ENOMEM;
		}

		buffer->dma_buf_fd[i] = fd;

		v4l2_buffer_setup_fd(&buffer->buffer, i, fd);

		data = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED,
			    fd, 0);
		if (data == MAP_FAILED) {
			/* TODO: Cleanup previous mappings on error. */
			return -ENOMEM;
		}

		buffer->data[i] = data;
	}

	return 0;
}

int demo_buffer_setup_v4l2(struct demo *demo, struct demo_buffer *buffer,
			   int video_fd)
{
	unsigned int offset;
	unsigned int length;
	unsigned int i;
	void *data;

	for (i = 0; i < buffer->planes_count; i++) {
		v4l2_buffer_plane_offset(&buffer->buffer, i, &offset);
		v4l2_buffer_plane_length(&buffer->buffer, i, &length);

		data = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED,
			    video_fd, offset);
		if (data == MAP_FAILED) {
			/* TODO: Cleanup previous mappings on error. */
			return -ENOMEM;
		}

		buffer->data[i] = data;
	}

	return 0;
}

int demo_buffer_setup(struct demo *demo, struct demo_buffer *buffer,
		      int video_fd, unsigned int memory, unsigned int type,
		      unsigned int index, unsigned int planes_count,
		      bool import_camera)
{
	struct demo_buffer *import_buffer;
	int import_video_fd;
	int ret;

	if (!demo || !buffer)
		return -EINVAL;

	ret = demo_buffer_setup_base(buffer, video_fd, memory, type, index,
				     planes_count);
	if (ret)
		return ret;

	if (import_camera) {
		import_buffer = &demo->camera.capture_buffers[index];
		import_video_fd = demo->camera.video_fd;

		ret = demo_buffer_setup_import(demo, buffer, import_buffer,
					       import_video_fd);
	} else if (demo->allocator == DEMO_ALLOCATOR_DMA_HEAP) {
		ret = demo_buffer_setup_dma_heap(demo, buffer, video_fd);
	} else if (demo->allocator == DEMO_ALLOCATOR_V4L2) {
		ret = demo_buffer_setup_v4l2(demo, buffer, video_fd);
	} else {
		ret = -EINVAL;
	}

	return ret;
}

void demo_buffer_cleanup(struct demo_buffer *buffer)
{
	unsigned int length;
	unsigned int i;

	if (!buffer)
		return;

	for (i = 0; i < buffer->planes_count; i++) {
		v4l2_buffer_plane_length(&buffer->buffer, i, &length);
		munmap(buffer->data[i], length);
		buffer->data[i] = NULL;

		if (buffer->dma_buf_fd[i] >= 0) {
			close(buffer->dma_buf_fd[i]);
			buffer->dma_buf_fd[i] = -1;
		}
	}
}

int demo_open_media_decoder(struct udev *udev, const char *media_path,
			    int *video_fd)
{
	int function = MEDIA_ENT_F_PROC_VIDEO_DECODER;
	struct media_device_info device_info = { 0 };
	struct media_v2_topology topology = { 0 };
	struct media_v2_interface *interfaces = NULL;
	struct media_v2_entity *entities = NULL;
	struct media_v2_pad *pads = NULL;
	struct media_v2_link *links = NULL;
	struct media_v2_entity *base_entity;
	struct media_v2_interface *base_interface;
	struct media_v2_pad *sink_pad;
	struct media_v2_link *sink_link;
	struct media_v2_pad *source_pad;
	struct media_v2_link *source_link;
	struct udev_device *device;
	const char *video_path;
	int media_fd = -1;
	dev_t devnum;
	int fd;
	int ret;

	media_fd = open(media_path, O_RDWR);
	if (media_fd < 0)
		return -errno;

	ret = media_device_info(media_fd, &device_info);
	if (ret)
		goto complete;

	ret = media_topology_get(media_fd, &topology);
	if (ret)
		goto complete;

	if (!topology.num_interfaces || !topology.num_entities ||
	    !topology.num_pads || !topology.num_links) {
		ret = -ENODEV;
		goto complete;
	}

	interfaces = calloc(1, topology.num_interfaces * sizeof(*interfaces));
	if (!interfaces) {
		ret = -ENOMEM;
		goto complete;
	}

	topology.ptr_interfaces = (__u64)interfaces;

	entities = calloc(1, topology.num_entities * sizeof(*entities));
	if (!entities) {
		ret = -ENOMEM;
		goto complete;
	}

	topology.ptr_entities = (__u64)entities;

	pads = calloc(1, topology.num_pads * sizeof(*pads));
	if (!pads) {
		ret = -ENOMEM;
		goto complete;
	}

	topology.ptr_pads = (__u64)pads;

	links = calloc(1, topology.num_links * sizeof(*links));
	if (!links) {
		ret = -ENOMEM;
		goto complete;
	}

	topology.ptr_links = (__u64)links;

	ret = media_topology_get(media_fd, &topology);
	if (ret)
		goto complete;

	base_entity = media_topology_entity_find_by_function(&topology,
							     function);
	if (!base_entity) {
		ret = -ENODEV;
		goto complete;
	}

	sink_pad = media_topology_pad_find_by_entity(&topology,
						     base_entity->id,
						     MEDIA_PAD_FL_SINK);
	if (!sink_pad) {
		ret = -ENODEV;
		goto complete;
	}

	sink_link = media_topology_link_find_by_pad(&topology, sink_pad->id,
						    sink_pad->flags);
	if (!sink_link) {
		ret = -ENODEV;
		goto complete;
	}

	source_pad = media_topology_pad_find_by_id(&topology,
						   sink_link->source_id);
	if (!source_pad) {
		ret = -ENODEV;
		goto complete;
	}

	source_link = media_topology_link_find_by_entity(&topology,
							 source_pad->entity_id,
							 MEDIA_PAD_FL_SINK);
	if (!source_link) {
		ret = -ENODEV;
		goto complete;
	}

	base_interface =
		media_topology_interface_find_by_id(&topology,
						    source_link->source_id);
	if (!base_interface) {
		ret = -ENODEV;
		goto complete;
	}

	devnum = makedev(base_interface->devnode.major,
			 base_interface->devnode.minor);

	device = udev_device_new_from_devnum(udev, 'c', devnum);
	if (!device) {
		ret = -ENODEV;
		goto complete;
	}

	video_path = udev_device_get_devnode(device);
	fd = open(video_path, O_RDWR | O_NONBLOCK);

	udev_device_unref(device);

	if (fd < 0) {
		ret = -errno;
		goto complete;
	}

	*video_fd = fd;

	ret = 0;

complete:
	if (media_fd >= 0)
		close(media_fd);

	if (links)
		free(links);

	if (pads)
		free(pads);

	if (entities)
		free(entities);

	if (interfaces)
		free(interfaces);

	return ret;
}

int demo_open_media_camera(struct udev *udev, const char *media_path,
			   int *video_fd)
{
	int function = MEDIA_ENT_F_CAM_SENSOR;
	struct media_device_info device_info = { 0 };
	struct media_v2_topology topology = { 0 };
	struct media_v2_interface *interfaces = NULL;
	struct media_v2_entity *entities = NULL;
	struct media_v2_pad *pads = NULL;
	struct media_v2_link *links = NULL;
	struct media_v2_entity *base_entity;
	struct udev_device *device;
	unsigned int capabilities;
	const char *video_path;
	int media_fd = -1;
	unsigned int i;
	dev_t devnum;
	bool check;
	int ret;
	int fd;

	media_fd = open(media_path, O_RDWR);
	if (media_fd < 0)
		return -errno;

	ret = media_device_info(media_fd, &device_info);
	if (ret)
		goto complete;

	ret = media_topology_get(media_fd, &topology);
	if (ret)
		goto complete;

	if (!topology.num_interfaces || !topology.num_entities ||
	    !topology.num_pads || !topology.num_links) {
		ret = -ENODEV;
		goto complete;
	}

	interfaces = calloc(1, topology.num_interfaces * sizeof(*interfaces));
	if (!interfaces) {
		ret = -ENOMEM;
		goto complete;
	}

	topology.ptr_interfaces = (__u64)interfaces;

	entities = calloc(1, topology.num_entities * sizeof(*entities));
	if (!entities) {
		ret = -ENOMEM;
		goto complete;
	}

	topology.ptr_entities = (__u64)entities;

	pads = calloc(1, topology.num_pads * sizeof(*pads));
	if (!pads) {
		ret = -ENOMEM;
		goto complete;
	}

	topology.ptr_pads = (__u64)pads;

	links = calloc(1, topology.num_links * sizeof(*links));
	if (!links) {
		ret = -ENOMEM;
		goto complete;
	}

	topology.ptr_links = (__u64)links;

	ret = media_topology_get(media_fd, &topology);
	if (ret)
		goto complete;

	base_entity = media_topology_entity_find_by_function(&topology,
							     function);
	if (!base_entity) {
		ret = -ENODEV;
		goto complete;
	}

	/* Lookup a capture video node in the same media device. */
	for (i = 0; i < topology.num_interfaces; i++) {
		struct media_v2_interface *interface = &interfaces[i];

		if (interface->intf_type != MEDIA_INTF_T_V4L_VIDEO)
			continue;

		devnum = makedev(interface->devnode.major,
				 interface->devnode.minor);

		device = udev_device_new_from_devnum(udev, 'c', devnum);
		if (!device)
			continue;

		video_path = udev_device_get_devnode(device);
		fd = open(video_path, O_RDWR | O_NONBLOCK);

		udev_device_unref(device);

		if (fd < 0)
			continue;

		capabilities = 0;
		ret = v4l2_capabilities_probe(fd, &capabilities, NULL, NULL);
		if (ret)
			continue;

		check = capabilities & V4L2_CAP_VIDEO_CAPTURE;
		if (!check)
			continue;

		*video_fd = fd;

		ret = 0;
		goto complete;
	}

	ret = -ENODEV;

complete:
	if (media_fd >= 0)
		close(media_fd);

	if (links)
		free(links);

	if (pads)
		free(pads);

	if (entities)
		free(entities);

	if (interfaces)
		free(interfaces);

	return ret;
}

int demo_open(struct demo *demo)
{
	struct demo_decoder *decoder = &demo->decoder;
	struct demo_camera *camera = &demo->camera;
	struct udev *udev = NULL;
	struct udev_enumerate *enumerate = NULL;
	struct udev_list_entry *devices;
	struct udev_list_entry *entry;
	int ret;

	if (!demo)
		return -EINVAL;

	decoder->video_fd = -1;
	camera->video_fd = -1;

	udev = udev_new();
	if (!udev)
		goto error;

	enumerate = udev_enumerate_new(udev);
	if (!enumerate)
		goto error;

	udev_enumerate_add_match_subsystem(enumerate, "media");
	udev_enumerate_scan_devices(enumerate);

	devices = udev_enumerate_get_list_entry(enumerate);

	udev_list_entry_foreach(entry, devices) {
		struct udev_device *device;
		const char *media_path;
		const char *path;

		path = udev_list_entry_get_name(entry);
		if (!path)
			continue;

		device = udev_device_new_from_syspath(udev, path);
		if (!device)
			continue;

		media_path = udev_device_get_devnode(device);

		if (decoder->video_fd < 0)
			demo_open_media_decoder(udev, media_path,
						&decoder->video_fd);

		if (camera->video_fd < 0)
			demo_open_media_camera(udev, media_path,
					       &camera->video_fd);

		udev_device_unref(device);

		if (decoder->video_fd >= 0 && camera->video_fd >= 0)
			break;
	}

	ret = 0;
	goto complete;

error:
	if (decoder->video_fd >= 0) {
		close(decoder->video_fd);
		decoder->video_fd = -1;
	}

	if (camera->video_fd >= 0) {
		close(camera->video_fd);
		camera->video_fd = -1;
	}

	ret = -1;

complete:
	if (enumerate)
		udev_enumerate_unref(enumerate);

	if (udev)
		udev_unref(udev);

	return ret;
}

void demo_close(struct demo *demo)
{
	struct demo_decoder *decoder = &demo->decoder;
	struct demo_camera *camera = &demo->camera;

	if (!demo)
		return;

	if (decoder->video_fd >= 0) {
		close(decoder->video_fd);
		decoder->video_fd = -1;
	}

	if (camera->video_fd >= 0) {
		close(camera->video_fd);
		camera->video_fd = -1;
	}
}

int demo_setup(struct demo *demo, int source, int allocator, unsigned int width,
	       unsigned int height)
{
	int fd;
	int ret;

	if (!demo)
		return -EINVAL;

	demo->source = source;
	demo->allocator = allocator;
	demo->width = width;
	demo->height = height;

	if (allocator == DEMO_ALLOCATOR_DMA_HEAP) {
		fd = dma_heap_open("reserved");
		if (fd < 0)
			return fd;

		demo->dma_heap_fd = fd;
	}

	if (source == DEMO_SOURCE_CAMERA) {
		ret = demo_camera_setup(demo);
		if (ret) {
			fprintf(stderr, "Failed to setup camera\n");
			return ret;
		}
	}

	ret = demo_decoder_setup(demo);
	if (ret) {
		fprintf(stderr, "Failed to setup decoder\n");
		return ret;
	}

	return 0;
}

void demo_cleanup(struct demo *demo)
{
	if (!demo)
		return;

	demo_decoder_cleanup(demo);

	if (demo->source == DEMO_SOURCE_CAMERA)
		demo_camera_cleanup(demo);

	if (demo->allocator == DEMO_ALLOCATOR_DMA_HEAP)
		close(demo->dma_heap_fd);
}

int demo_file_read(struct demo *demo)
{
	struct demo_file *file = &demo->file;
	struct demo_decoder *decoder = &demo->decoder;
	struct demo_buffer *buffer;
	struct perf perf = { 0 };
	unsigned int plane_index = 0;
	unsigned int length;
	void *data;
	int ret;

	if (!demo)
		return -EINVAL;

	ret = demo_decoder_buffer_current(demo, decoder->output_type, &buffer);
	if (ret)
		return ret;

	data = buffer->data[plane_index];

	v4l2_buffer_plane_length(&buffer->buffer, plane_index, &length);
	if (length < file->size)
		return -ENOMEM;

	ret = demo_buffer_sync_begin(buffer);
	if (ret)
		return ret;

	perf_before(&perf);
	ret = read(file->fd, data, file->size);
	perf_after(&perf);

	if (ret < file->size) {
		fprintf(stderr, "Failed to read from source file\n");
		return 1;
	}

	printf("Read %u bytes from source file\n", file->size);

	perf_print(&perf, "source read");

	ret = demo_buffer_sync_finish(buffer);
	if (ret)
		return ret;

	v4l2_buffer_setup_plane_length_used(&buffer->buffer, plane_index,
					    file->size);

	return 0;
}

int demo_file_open(struct demo *demo, char *path)
{
	struct demo_file *file = &demo->file;
	struct stat stat;
	int ret;
	int fd;

	if (!demo)
		return -EINVAL;

	file->fd = -1;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Failed to open input file\n");
		return -errno;
	}

	memset(&stat, 0, sizeof(stat));

	ret = fstat(fd, &stat);
	if (ret) {
		fprintf(stderr, "Failed to stat input file\n");
		return -errno;
	}

	file->fd = fd;
	file->size = stat.st_size;

	return 0;
}

void demo_file_close(struct demo *demo)
{
	struct demo_file *file = &demo->file;

	if (!demo)
		return;

	if (file->fd >= 0) {
		close(file->fd);
		file->fd = -1;
	}
}

int demo_dump(struct demo *demo, char *dump_path)
{
	struct demo_decoder *decoder = &demo->decoder;
	struct perf perf = { 0 };
	struct demo_buffer *buffer;
	unsigned int plane_index = 0;
	unsigned int size;
	void *data;
	int fd = -1;
	int ret;

	ret = demo_decoder_buffer_current(demo, decoder->capture_type, &buffer);
	if (ret)
		return ret;

	v4l2_buffer_plane_length_used(&buffer->buffer, plane_index, &size);

	data = buffer->data[plane_index];

	fd = open(dump_path, O_RDWR | O_TRUNC | O_CREAT, 0644);
	if (dump_path < 0) {
		fprintf(stderr, "Failed to open dump file\n");
		return -errno;
	}

	ret = demo_buffer_sync_begin(buffer);
	if (ret)
		goto complete;

	perf_before(&perf);
	ret = write(fd, data, size);
	perf_after(&perf);

	if (ret < size) {
		fprintf(stderr, "Failed to write data to output file\n");
		ret = -EIO;
		goto complete;
	}

	printf("Wrote %u bytes to dump file %s\n", size, dump_path);

	perf_print(&perf, "dump write");

	ret = demo_buffer_sync_finish(buffer);
	if (ret)
		goto complete;

	ret = 0;

complete:
	if (fd >= 0)
		close(fd);

	return ret;
}

int main(int argc, char *argv[])
{
	struct demo demo = { 0 };
	unsigned int width;
	unsigned int height;
	int source;
	int allocator;
	unsigned int dump_size;
	char *dump_path;
	int dump_fd;
	int ret;

	dump_path = "output.yuv";
	source = DEMO_SOURCE_CAMERA;
	allocator = DEMO_ALLOCATOR_DMA_HEAP;
	width = 1280;
	height = 720;

	if (source == DEMO_SOURCE_FILE) {
		char *source_path = argv[1];

		if (argc < 2)
			return 1;

		ret = demo_file_open(&demo, source_path);
		if (ret)
			return 1;
	}

	ret = demo_open(&demo);
	if (ret)
		return 1;

	ret = demo_setup(&demo, source, allocator, width, height);
	if (ret)
		return 1;

	if (source == DEMO_SOURCE_FILE) {
		ret = demo_file_read(&demo);
		if (ret)
			return 1;

		demo_file_close(&demo);
	} else {
		ret = demo_camera_roll(&demo);
		if (ret)
			return 1;
	}

	ret = demo_decoder_run(&demo);
	if (ret)
		return 1;

	ret = demo_dump(&demo, dump_path);
	if (ret)
		return 1;

	demo_cleanup(&demo);
	demo_close(&demo);

	return 0;
}
