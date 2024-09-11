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

#include "dma_buf.h"
#include "dma_heap.h"
#include "v4l2.h"
#include "media.h"
#include "perf.h"

enum cedrus_demo_allocator {
	CEDRUS_DEMO_ALLOCATOR_V4L2,
	CEDRUS_DEMO_ALLOCATOR_DMA_HEAP,
};

struct cedrus_demo_buffer {
	struct v4l2_buffer buffer;
	struct v4l2_plane planes[4];
	unsigned int planes_count;

	void *data[4];
	int dma_buf_fd[4];
};

struct cedrus_demo {
	int media_fd;
	int video_fd;
	int dma_heap_fd;

	int allocator;
	unsigned int memory;

	unsigned int output_type;
	unsigned int output_width;
	unsigned int output_height;
	unsigned int output_pixel_format;
	struct v4l2_format output_format;

	struct cedrus_demo_buffer output_buffers[3];
	unsigned int output_buffers_count;
	unsigned int output_buffer_index;

	unsigned int capture_type;
	unsigned int capture_width;
	unsigned int capture_height;
	unsigned int capture_pixel_format;
	struct v4l2_format capture_format;

	struct cedrus_demo_buffer capture_buffers[3];
	unsigned int capture_buffers_count;
	unsigned int capture_buffer_index;
};

static int media_device_probe(struct cedrus_demo *demo, struct udev *udev,
			      const char *media_path, int function)
{
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
	int video_fd = -1;
	dev_t devnum;
	int ret;

	media_fd = open(media_path, O_RDWR);
	if (media_fd < 0)
		return -errno;

	ret = media_device_info(media_fd, &device_info);
	if (ret)
		goto error;

	ret = media_topology_get(media_fd, &topology);
	if (ret)
		goto error;

	if (!topology.num_interfaces || !topology.num_entities ||
	    !topology.num_pads || !topology.num_links) {
		ret = -ENODEV;
		goto error;
	}

	interfaces = calloc(1, topology.num_interfaces * sizeof(*interfaces));
	if (!interfaces) {
		ret = -ENOMEM;
		goto error;
	}

	topology.ptr_interfaces = (__u64)interfaces;

	entities = calloc(1, topology.num_entities * sizeof(*entities));
	if (!entities) {
		ret = -ENOMEM;
		goto error;
	}

	topology.ptr_entities = (__u64)entities;

	pads = calloc(1, topology.num_pads * sizeof(*pads));
	if (!pads) {
		ret = -ENOMEM;
		goto error;
	}

	topology.ptr_pads = (__u64)pads;

	links = calloc(1, topology.num_links * sizeof(*links));
	if (!links) {
		ret = -ENOMEM;
		goto error;
	}

	topology.ptr_links = (__u64)links;

	ret = media_topology_get(media_fd, &topology);
	if (ret)
		goto error;

	base_entity = media_topology_entity_find_by_function(&topology,
							     function);
	if (!base_entity) {
		ret = -ENODEV;
		goto error;
	}

	sink_pad = media_topology_pad_find_by_entity(&topology,
						     base_entity->id,
						     MEDIA_PAD_FL_SINK);
	if (!sink_pad) {
		ret = -ENODEV;
		goto error;
	}

	sink_link = media_topology_link_find_by_pad(&topology, sink_pad->id,
						    sink_pad->flags);
	if (!sink_link) {
		ret = -ENODEV;
		goto error;
	}

	source_pad = media_topology_pad_find_by_id(&topology,
						   sink_link->source_id);
	if (!source_pad) {
		ret = -ENODEV;
		goto error;
	}

	source_link = media_topology_link_find_by_entity(&topology,
							 source_pad->entity_id,
							 MEDIA_PAD_FL_SINK);
	if (!source_link) {
		ret = -ENODEV;
		goto error;
	}

	base_interface = media_topology_interface_find_by_id(&topology,
							       source_link->source_id);
	if (!base_interface) {
		ret = -ENODEV;
		goto error;
	}

	devnum = makedev(base_interface->devnode.major,
			 base_interface->devnode.minor);

	device = udev_device_new_from_devnum(udev, 'c', devnum);
	if (!device) {
		ret = -ENODEV;
		goto error;
	}

	video_path = udev_device_get_devnode(device);

	video_fd = open(video_path, O_RDWR | O_NONBLOCK);
	if (video_fd < 0) {
		ret = -errno;
		goto error;
	}

	demo->media_fd = media_fd;
	demo->video_fd = video_fd;

	ret = 0;
	goto complete;

error:
	if (media_fd >= 0)
		close(media_fd);

	if (video_fd >= 0)
		close(video_fd);

complete:
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

int cedrus_demo_open(struct cedrus_demo *demo)
{
	struct udev *udev = NULL;
	struct udev_enumerate *enumerate = NULL;
	struct udev_list_entry *devices;
	struct udev_list_entry *entry;
	int ret;

	if (!demo)
		return -EINVAL;

	demo->media_fd = -1;
	demo->video_fd = -1;

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

		ret = media_device_probe(demo, udev, media_path,
					 MEDIA_ENT_F_PROC_VIDEO_DECODER);

		udev_device_unref(device);

		if (!ret)
			break;
	}

	if (demo->media_fd < 0) {
		fprintf(stderr, "Failed to open base media device\n");
		goto error;
	}

	if (demo->video_fd < 0) {
		fprintf(stderr, "Failed to open base video device\n");
		goto error;
	}

	ret = 0;
	goto complete;

error:
	if (demo->media_fd >= 0) {
		close(demo->media_fd);
		demo->media_fd = -1;
	}

	if (demo->video_fd >= 0) {
		close(demo->video_fd);
		demo->video_fd = -1;
	}

	ret = -1;

complete:
	if (enumerate)
		udev_enumerate_unref(enumerate);

	if (udev)
		udev_unref(udev);

	return ret;
}

void cedrus_demo_close(struct cedrus_demo *demo)
{
	if (!demo)
		return;

	if (demo->media_fd >= 0) {
		close(demo->media_fd);
		demo->media_fd = -1;
	}

	if (demo->video_fd >= 0) {
		close(demo->video_fd);
		demo->video_fd = -1;
	}
}

int cedrus_demo_buffer_setup(struct cedrus_demo *demo,
			     struct cedrus_demo_buffer *buffer,
			     unsigned int type, unsigned int index,
			     unsigned int planes_count)
{
	struct v4l2_plane *planes;
	unsigned int offset;
	unsigned int length;
	unsigned int i;
	void *data;
	int fd;
	int ret;

	if (!demo || !buffer)
		return -EINVAL;

	if (v4l2_type_mplane_check(type)) {
		if (planes_count > 4)
			return -EINVAL;
	} else {
		if (planes_count > 1)
			return -EINVAL;
	}

	buffer->planes_count = planes_count;

	v4l2_buffer_setup_base(&buffer->buffer, type, demo->memory);
	v4l2_buffer_setup_index(&buffer->buffer, index);
	v4l2_buffer_setup_planes(&buffer->buffer, buffer->planes, planes_count);

	ret = v4l2_buffer_query(demo->video_fd, &buffer->buffer);
	if (ret) {
		fprintf(stderr, "Failed to query buffer\n");
		return ret;
	}

	if (demo->allocator == CEDRUS_DEMO_ALLOCATOR_DMA_HEAP) {
		for (i = 0; i < planes_count; i++) {
			v4l2_buffer_plane_length(&buffer->buffer, i, &length);

			fd = dma_heap_alloc(demo->dma_heap_fd, length, O_RDWR);
			if (fd < 0) {
				/* TODO: Cleanup previous allocations/mappings on error. */
				return -ENOMEM;
			}

			buffer->dma_buf_fd[i] = fd;

			v4l2_buffer_setup_fd(&buffer->buffer, i, fd);

			data = mmap(NULL, length, PROT_READ | PROT_WRITE,
				    MAP_SHARED, fd, 0);
			if (data == MAP_FAILED) {
				/* TODO: Cleanup previous mappings on error. */
				return -ENOMEM;
			}

			buffer->data[i] = data;
		}
	} else if (demo->allocator == CEDRUS_DEMO_ALLOCATOR_V4L2) {
		for (i = 0; i < planes_count; i++) {
			v4l2_buffer_plane_offset(&buffer->buffer, i, &offset);
			v4l2_buffer_plane_length(&buffer->buffer, i, &length);

			data = mmap(NULL, length, PROT_READ | PROT_WRITE,
				    MAP_SHARED, demo->video_fd, offset);
			if (data == MAP_FAILED) {
				/* TODO: Cleanup previous mappings on error. */
				return -ENOMEM;
			}

			buffer->data[i] = data;
		}
	}

	return 0;
}

void cedrus_demo_buffer_cleanup(struct cedrus_demo *demo,
				struct cedrus_demo_buffer *buffer)
{
	unsigned int length;
	unsigned int i;

	if (!demo || !buffer)
		return;

	for (i = 0; i < buffer->planes_count; i++) {
		v4l2_buffer_plane_length(&buffer->buffer, i, &length);
		munmap(buffer->data[i], length);
		buffer->data[i] = NULL;
	}

	if (demo->allocator == CEDRUS_DEMO_ALLOCATOR_DMA_HEAP) {
		for (i = 0; i < buffer->planes_count; i++) {
			close(buffer->dma_buf_fd[i]);
			buffer->dma_buf_fd[i] = -1;
		}
	}
}

int cedrus_demo_buffer_current(struct cedrus_demo *demo, unsigned int type,
			       struct cedrus_demo_buffer **buffer)
{
	if (!demo || !buffer)
		return -EINVAL;

	if (type == demo->output_type)
		*buffer = &demo->output_buffers[demo->output_buffer_index];
	else if (type == demo->capture_type)
		*buffer = &demo->capture_buffers[demo->capture_buffer_index];
	else
		return -EINVAL;

	return 0;
}

int cedrus_demo_buffer_cycle(struct cedrus_demo *demo, unsigned int type)
{
	if (!demo)
		return -EINVAL;

	if (type == demo->output_type) {
		demo->output_buffer_index++;
		demo->output_buffer_index %= demo->output_buffers_count;
	} else if (type == demo->capture_type) {
		demo->capture_buffer_index++;
		demo->capture_buffer_index %= demo->capture_buffers_count;
	} else {
		return -EINVAL;
	}

	return 0;
}

int cedrus_demo_buffer_access(struct cedrus_demo *demo, unsigned int type,
			      unsigned int plane_index, void **data,
			      unsigned int *size)
{
	struct cedrus_demo_buffer *buffer;
	unsigned int length;
	int ret;

	if (!demo || !data || !size)
		return -EINVAL;

	ret = cedrus_demo_buffer_current(demo, type, &buffer);
	if (ret)
		return ret;

	if (plane_index >= buffer->planes_count)
		return -EINVAL;

	*data = buffer->data[plane_index];

	if (type == demo->output_type) {
		v4l2_buffer_plane_length(&buffer->buffer, plane_index, &length);
		if (length < *size)
			return -EINVAL;

		v4l2_buffer_setup_plane_length_used(&buffer->buffer,
						    plane_index, *size);
	} else if (type == demo->capture_type) {
		v4l2_buffer_plane_length_used(&buffer->buffer, plane_index,
					      size);
	}

	return 0;
}

int cedrus_demo_buffer_sync(struct cedrus_demo *demo, unsigned int type,
			    long flags)
{
	struct cedrus_demo_buffer *buffer;
	unsigned int i;
	int ret;

	if (!demo)
		return -EINVAL;

	if (demo->allocator != CEDRUS_DEMO_ALLOCATOR_DMA_HEAP)
		return 0;

	if (type == demo->output_type)
		flags |= DMA_BUF_SYNC_WRITE;
	else if (type == demo->capture_type)
		flags |= DMA_BUF_SYNC_READ;
	else
		return -EINVAL;

	ret = cedrus_demo_buffer_current(demo, type, &buffer);
	if (ret)
		return ret;

	for (i = 0; i < buffer->planes_count; i++) {
		ret = dma_buf_sync(buffer->dma_buf_fd[i], flags);
		if (ret)
			return ret;
	}

	return 0;
}

int cedrus_demo_buffer_sync_begin(struct cedrus_demo *demo, unsigned int type)
{
	return cedrus_demo_buffer_sync(demo, type, DMA_BUF_SYNC_START);
}

int cedrus_demo_buffer_sync_finish(struct cedrus_demo *demo, unsigned int type)
{
	return cedrus_demo_buffer_sync(demo, type, DMA_BUF_SYNC_END);
}

int cedrus_demo_setup(struct cedrus_demo *demo, int allocator)
{
	unsigned int planes_count;
	unsigned int count;
	unsigned int size;
	unsigned int i;
	bool check;
	int fd;
	int ret;

	if (!demo)
		return -EINVAL;

	switch (allocator) {
	case CEDRUS_DEMO_ALLOCATOR_V4L2:
		demo->memory = V4L2_MEMORY_MMAP;
		break;
	case CEDRUS_DEMO_ALLOCATOR_DMA_HEAP:
		demo->memory = V4L2_MEMORY_DMABUF;

		fd = dma_heap_open("reserved");
		if (fd < 0)
			return fd;

		demo->dma_heap_fd = fd;
		break;
	default:
		return -EINVAL;
	}

	demo->allocator = allocator;

	demo->output_type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	demo->capture_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	demo->output_width = 1280;
	demo->output_height = 720;
	demo->output_pixel_format = V4L2_PIX_FMT_JPEG;

	demo->capture_width = demo->output_width;
	demo->capture_height = demo->output_height;
	demo->capture_pixel_format = V4L2_PIX_FMT_NV16;

	planes_count = 1;

	/* Output pixel format check */

	check = v4l2_pixel_format_check(demo->video_fd, demo->output_type,
					demo->output_pixel_format);
	if (!check) {
		fprintf(stderr, "Missing output pixel format support\n");
		return -EINVAL;
	}

	/* Capture pixel format check */

	check = v4l2_pixel_format_check(demo->video_fd, demo->capture_type,
					demo->capture_pixel_format);
	if (!check) {
		fprintf(stderr, "Missing capture pixel format support\n");
		return -EINVAL;
	}

	/* Output format setup */

	v4l2_format_setup_base(&demo->output_format, demo->output_type);
	v4l2_format_setup_pixel(&demo->output_format, demo->output_width,
				demo->output_height, demo->output_pixel_format);

	/* Let's assume that JPEG fits in width * height bytes. */
	size = demo->output_width * demo->output_height;
	v4l2_format_setup_sizeimage(&demo->output_format, 0, size);

	ret = v4l2_format_try(demo->video_fd, &demo->output_format);
	if (ret) {
		fprintf(stderr, "Failed to try output format\n");
		return ret;
	}

	ret = v4l2_format_set(demo->video_fd, &demo->output_format);
	if (ret) {
		fprintf(stderr, "Failed to set output format\n");
		return ret;
	}

	/* Capture format setup */

	v4l2_format_setup_base(&demo->capture_format, demo->capture_type);
	v4l2_format_setup_pixel(&demo->capture_format, demo->capture_width,
				demo->capture_height,
				demo->capture_pixel_format);

	ret = v4l2_format_try(demo->video_fd, &demo->capture_format);
	if (ret) {
		fprintf(stderr, "Failed to try capture format\n");
		return ret;
	}

	ret = v4l2_format_set(demo->video_fd, &demo->capture_format);
	if (ret) {
		fprintf(stderr, "Failed to set capture format\n");
		return ret;
	}

	/* Output buffers setup */

	count = sizeof(demo->output_buffers) / sizeof(demo->output_buffers[0]);

	ret = v4l2_buffers_request(demo->video_fd, demo->output_type,
				   demo->memory, count);
	if (ret) {
		fprintf(stderr, "Failed to allocate output buffers\n");
		return ret;
	}

	printf("Allocated %d output buffers\n", count);

	for (i = 0; i < count; i++) {
		ret = cedrus_demo_buffer_setup(demo, &demo->output_buffers[i],
					       demo->output_type, i,
					       planes_count);
		if (ret) {
			/* TODO: Cleanup previous allocations on error. */
			return ret;
		}
	}

	demo->output_buffers_count = count;

	/* Capture buffers setup */

	count = sizeof(demo->capture_buffers) /
		sizeof(demo->capture_buffers[0]);

	ret = v4l2_buffers_request(demo->video_fd, demo->capture_type,
				   demo->memory, count);
	if (ret) {
		fprintf(stderr, "Failed to allocate capture buffers\n");
		/* TODO: Cleanup previous allocations on error. */
		return ret;
	}

	printf("Allocated %d capture buffers\n", count);

	for (i = 0; i < count; i++) {
		ret = cedrus_demo_buffer_setup(demo, &demo->capture_buffers[i],
					       demo->capture_type, i,
					       planes_count);
		if (ret) {
			/* TODO: Cleanup previous allocations on error. */
			return ret;
		}
	}

	demo->capture_buffers_count = count;

	return 0;
}

void cedrus_demo_cleanup(struct cedrus_demo *demo)
{
	unsigned int count;
	unsigned int i;

	if (!demo)
		return;

	/* Output buffers cleanup */

	for (i = 0; i < demo->output_buffers_count; i++)
		cedrus_demo_buffer_cleanup(demo, &demo->output_buffers[i]);

	v4l2_buffers_destroy(demo->video_fd, demo->output_type, demo->memory);

	/* Capture buffers cleanup */

	for (i = 0; i < demo->capture_buffers_count; i++)
		cedrus_demo_buffer_cleanup(demo, &demo->capture_buffers[i]);

	v4l2_buffers_destroy(demo->video_fd, demo->capture_type, demo->memory);

	if (demo->allocator == CEDRUS_DEMO_ALLOCATOR_DMA_HEAP)
		close(demo->dma_heap_fd);
}

int cedrus_demo_run(struct cedrus_demo *demo)
{
	struct cedrus_demo_buffer *buffer;
	struct v4l2_buffer buffer_dequeue;
	struct timeval timeout = { 0, 300000 };
	unsigned int count;
	int ret;

	if (!demo)
		return -EINVAL;

	ret = cedrus_demo_buffer_current(demo, demo->capture_type, &buffer);
	if (ret)
		return ret;

	ret = v4l2_buffer_queue(demo->video_fd, &buffer->buffer);
	if (ret) {
		fprintf(stderr, "Failed to queue capture buffer\n");
		return ret;
	}

	ret = cedrus_demo_buffer_current(demo, demo->output_type, &buffer);
	if (ret)
		return ret;

	ret = v4l2_buffer_queue(demo->video_fd, &buffer->buffer);
	if (ret) {
		fprintf(stderr, "Failed to queue output buffer\n");
		return ret;
	}

	ret = v4l2_stream_on(demo->video_fd, demo->capture_type);
	if (ret) {
		fprintf(stderr, "Failed to start capture stream\n");
		return ret;
	}

	ret = v4l2_stream_on(demo->video_fd, demo->output_type);
	if (ret) {
		fprintf(stderr, "Failed to start output stream\n");
		return ret;
	}

	ret = v4l2_poll(demo->video_fd, &timeout);
	if (ret <= 0) {
		fprintf(stderr, "Error waiting for decode\n");
		return ret;
	}

	v4l2_buffer_setup_base(&buffer_dequeue, demo->capture_type,
			       demo->memory);

	ret = v4l2_buffer_dequeue(demo->video_fd, &buffer_dequeue);
	if (ret) {
		fprintf(stderr, "Failed to dequeue capture buffer\n");
		return ret;
	}

	if (buffer_dequeue.index != demo->capture_buffer_index)
		fprintf(stderr,
			"Dequeued unexpected capture buffer (%d vs %d)\n",
			buffer_dequeue.index, demo->capture_buffer_index);

	v4l2_buffer_setup_base(&buffer_dequeue, demo->output_type,
			       demo->memory);

	ret = v4l2_buffer_dequeue(demo->video_fd, &buffer_dequeue);
	if (ret) {
		fprintf(stderr, "Failed to dequeue output buffer\n");
		return ret;
	}

	if (buffer_dequeue.index != demo->output_buffer_index)
		fprintf(stderr,
			"Dequeued unexpected output buffer (%d vs %d)\n",
			buffer_dequeue.index, demo->output_buffer_index);

	ret = v4l2_stream_off(demo->video_fd, demo->capture_type);
	if (ret)
		return ret;

	ret = v4l2_stream_off(demo->video_fd, demo->output_type);
	if (ret)
		return ret;

	return 0;
}

int main(int argc, char *argv[])
{
	struct cedrus_demo demo = { 0 };
	struct perf perf = { 0 };
	struct cedrus_demo_buffer *buffer;
	struct stat input_stat;
	unsigned int input_size;
	unsigned int output_size;
	unsigned int size;
	void *input_data;
	void *output_data;
	char *input_path;
	char *output_path;
	int input_fd;
	int output_fd;
	int ret;

	if (argc < 2)
		return 1;

	input_path = argv[1];
	output_path = "./output.yuv";

	/* Input */

	input_fd = open(input_path, O_RDONLY);
	if (input_fd < 0) {
		fprintf(stderr, "Failed to open input file\n");
		return 1;
	}

	memset(&input_stat, 0, sizeof(input_stat));

	ret = fstat(input_fd, &input_stat);
	if (ret) {
		fprintf(stderr, "Failed to stat input file\n");
		return 1;
	}

	input_size = input_stat.st_size;

	/* Setup */

	ret = cedrus_demo_open(&demo);
	if (ret)
		return 1;

	ret = cedrus_demo_setup(&demo, CEDRUS_DEMO_ALLOCATOR_DMA_HEAP);
	if (ret)
		return 1;

	/* Input Read */

	ret = cedrus_demo_buffer_access(&demo, demo.output_type, 0,
					&input_data, &input_size);
	if (ret)
		return 1;

	ret = cedrus_demo_buffer_sync_begin(&demo, demo.output_type);
	if (ret)
		return 1;

	perf_before(&perf);
	ret = read(input_fd, input_data, input_size);
	perf_after(&perf);

	if (ret < input_size) {
		fprintf(stderr, "Failed to read from input file\n");
		return 1;
	}

	printf("Read %u bytes from input file\n", input_size);

	perf_print(&perf, "input read");

	ret = cedrus_demo_buffer_sync_finish(&demo, demo.output_type);
	if (ret)
		return 1;

	/* Decode */

	perf_before(&perf);
	ret = cedrus_demo_run(&demo);
	perf_after(&perf);

	if (ret)
		return 1;

	perf_print(&perf, "decode");

	/* Output */

	output_fd = open(output_path, O_RDWR | O_TRUNC | O_CREAT, 0644);
	if (output_fd < 0) {
		fprintf(stderr, "Failed to open output file\n");
		return 1;
	}

	/* Output Write */

	ret = cedrus_demo_buffer_access(&demo, demo.capture_type, 0,
					&output_data, &output_size);
	if (ret)
		return 1;

	ret = cedrus_demo_buffer_sync_begin(&demo, demo.capture_type);
	if (ret)
		return 1;

	perf_before(&perf);
	ret = write(output_fd, output_data, output_size);
	perf_after(&perf);

	if (ret < output_size) {
		fprintf(stderr, "Failed to write data to output file\n");
		return 1;
	}

	printf("Wrote %u bytes to output file\n", output_size);

	perf_print(&perf, "output write");

	ret = cedrus_demo_buffer_sync_finish(&demo, demo.capture_type);
	if (ret)
		return 1;

	/* Cleanup */

	cedrus_demo_cleanup(&demo);

	cedrus_demo_close(&demo);

	close(input_fd);
	close(output_fd);

	return 0;
}
