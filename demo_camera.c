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
#include <string.h>
#include <errno.h>

#include <linux/dma-buf.h>

#include "demo.h"

int demo_camera_buffer_current(struct demo *demo, struct demo_buffer **buffer)
{
	struct demo_camera *camera = &demo->camera;
	unsigned int index;

	if (!demo || !buffer)
		return -EINVAL;

	index = camera->capture_buffer_index;
	*buffer = &camera->capture_buffers[index];

	return 0;
}

int demo_camera_buffer_cycle(struct demo *demo)
{
	struct demo_camera *camera = &demo->camera;

	if (!demo)
		return -EINVAL;

	camera->capture_buffer_index++;
	camera->capture_buffer_index %= camera->capture_buffers_count;

	return 0;
}

int demo_camera_roll(struct demo *demo)
{
	struct demo_camera *camera = &demo->camera;
	struct demo_buffer *buffer;
	struct demo_buffer *buffer_next;
	struct v4l2_buffer buffer_dequeue;
	unsigned int index;
	unsigned int count;
	unsigned int i;
	int ret;

	if (!demo)
		return -EINVAL;

	for (i = 0; i < camera->capture_buffers_count; i++) {
		ret = demo_camera_buffer_current(demo, &buffer);
		if (ret)
			return ret;

		ret = v4l2_buffer_queue(camera->video_fd, &buffer->buffer);
		if (ret) {
			fprintf(stderr, "Failed to queue capture buffer\n");
			return ret;
		}

		ret = demo_camera_buffer_cycle(demo);
		if (ret)
			return ret;
	}

	ret = v4l2_stream_on(camera->video_fd, camera->capture_type);
	if (ret) {
		fprintf(stderr, "Failed to start capture stream\n");
		return ret;
	}

	buffer_next = NULL;

	/*
	 * Capture data in all buffers and re-capture first buffer to make sure
	 * 3A has settled.
	 */
	for (i = 0; i < camera->capture_buffers_count + 1; i++) {
		struct timeval timeout = { 4, 0 };

		if (buffer_next) {
			ret = v4l2_buffer_queue(camera->video_fd,
						&buffer_next->buffer);
			if (ret) {
				fprintf(stderr,
					"Failed to queue capture buffer\n");
				return ret;
			}
		}

		ret = v4l2_poll(camera->video_fd, &timeout);
		if (ret <= 0) {
			fprintf(stderr, "Error waiting for camera\n");
			return ret == 0 ? -ETIMEDOUT : ret;
		}

		v4l2_buffer_setup_base(&buffer_dequeue, camera->capture_type,
				       camera->capture_memory);

		ret = v4l2_buffer_dequeue(camera->video_fd, &buffer_dequeue);
		if (ret) {
			fprintf(stderr, "Failed to dequeue capture buffer\n");
			return ret;
		}

		index = buffer_dequeue.index;
		buffer_next = &camera->capture_buffers[index];
	}

	ret = v4l2_stream_off(camera->video_fd, camera->capture_type);
	if (ret)
		return ret;

	ret = demo_camera_buffer_current(demo, &buffer);
	if (ret)
		return ret;

	for (i = 0; i < camera->capture_buffers_count; i++) {
		buffer = &camera->capture_buffers[i];

		/* Sync CPU-written data for UVC camera. */
		ret = demo_buffer_sync(buffer, DMA_BUF_SYNC_WRITE |
				       DMA_BUF_SYNC_END);
		if (ret)
			return ret;
	}

	return 0;
}

int demo_camera_setup(struct demo *demo)
{
	struct demo_camera *camera = &demo->camera;
	unsigned int planes_count;
	unsigned int count;
	unsigned int size;
	unsigned int i;
	bool check;
	int ret;

	if (camera->video_fd < 0) {
		fprintf(stderr, "Failed to open camera video device\n");
		return -ENODEV;
	}

	if (demo->allocator == DEMO_ALLOCATOR_V4L2)
		camera->capture_memory = V4L2_MEMORY_MMAP;
	else if (demo->allocator == DEMO_ALLOCATOR_DMA_HEAP)
		camera->capture_memory = V4L2_MEMORY_DMABUF;
	else
		return -EINVAL;

	camera->capture_width = demo->width;
	camera->capture_height = demo->height;
	camera->capture_pixel_format = V4L2_PIX_FMT_MJPEG;

	camera->capture_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	planes_count = 1;

	/* Capture pixel format check */

	check = v4l2_pixel_format_check(camera->video_fd,
					camera->capture_type,
					camera->capture_pixel_format);
	if (!check) {
		fprintf(stderr, "Missing capture pixel format support\n");
		return -EINVAL;
	}

	/* Capture format setup */

	v4l2_format_setup_base(&camera->capture_format, camera->capture_type);
	v4l2_format_setup_pixel(&camera->capture_format,
				camera->capture_width, camera->capture_height,
				camera->capture_pixel_format);

	/* Let's assume that JPEG fits in width * height * 3 bytes. */
	size = camera->capture_width * camera->capture_height * 3;
	v4l2_format_setup_sizeimage(&camera->capture_format, 0, size);

	ret = v4l2_format_try(camera->video_fd, &camera->capture_format);
	if (ret) {
		fprintf(stderr, "Failed to try capture format\n");
		return ret;
	}

	ret = v4l2_format_set(camera->video_fd, &camera->capture_format);
	if (ret) {
		fprintf(stderr, "Failed to set capture format\n");
		return ret;
	}

	/* Capture buffers setup */

	count = sizeof(camera->capture_buffers) /
		sizeof(camera->capture_buffers[0]);

	ret = v4l2_buffers_request(camera->video_fd, camera->capture_type,
				   camera->capture_memory, count);
	if (ret) {
		fprintf(stderr, "Failed to allocate capture buffers\n");
		/* TODO: Cleanup previous allocations on error. */
		return ret;
	}

	printf("Allocated %d capture buffers for camera\n", count);

	for (i = 0; i < count; i++) {
		ret = demo_buffer_setup(demo, &camera->capture_buffers[i],
					camera->video_fd,
					camera->capture_memory,
					camera->capture_type, i, planes_count,
					false);
		if (ret) {
			/* TODO: Cleanup previous allocations on error. */
			return ret;
		}
	}

	camera->capture_buffers_count = count;

	return 0;
}

void demo_camera_cleanup(struct demo *demo)
{
	struct demo_camera *camera = &demo->camera;
	unsigned int count;
	unsigned int i;

	/* Capture buffers cleanup */

	for (i = 0; i < camera->capture_buffers_count; i++)
		demo_buffer_cleanup(&camera->capture_buffers[i]);

	v4l2_buffers_destroy(camera->video_fd, camera->capture_type,
			     camera->capture_memory);
}
