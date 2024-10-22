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
#include <errno.h>

#include "demo.h"
#include "perf.h"

int demo_decoder_buffer_current(struct demo *demo, unsigned int type,
				struct demo_buffer **buffer)
{
	struct demo_decoder *decoder = &demo->decoder;
	unsigned int index;

	if (!demo || !buffer)
		return -EINVAL;

	if (type == decoder->output_type) {
		index = decoder->output_buffer_index;
		*buffer = &decoder->output_buffers[index];
	} else if (type == decoder->capture_type) {
		index = decoder->capture_buffer_index;
		*buffer = &decoder->capture_buffers[index];
	} else {
		return -EINVAL;
	}

	return 0;
}

int demo_decoder_buffer_cycle(struct demo *demo, unsigned int type)
{
	struct demo_decoder *decoder = &demo->decoder;

	if (!demo)
		return -EINVAL;

	if (type == decoder->output_type) {
		decoder->output_buffer_index++;
		decoder->output_buffer_index %= decoder->output_buffers_count;
	} else if (type == decoder->capture_type) {
		decoder->capture_buffer_index++;
		decoder->capture_buffer_index %= decoder->capture_buffers_count;
	} else {
		return -EINVAL;
	}

	return 0;
}

int demo_decoder_run(struct demo *demo)
{
	struct demo_decoder *decoder = &demo->decoder;
	struct perf perf = { 0 };
	struct demo_buffer *buffer;
	struct v4l2_buffer buffer_dequeue;
	struct timeval timeout = { 0, 300000 };
	unsigned int count;
	unsigned int i;
	int ret;

	if (!demo)
		return -EINVAL;

	perf_before(&perf);

	ret = demo_decoder_buffer_current(demo, decoder->capture_type, &buffer);
	if (ret)
		return ret;

	ret = v4l2_buffer_queue(decoder->video_fd, &buffer->buffer);
	if (ret) {
		fprintf(stderr, "Failed to queue capture buffer\n");
		return ret;
	}

	ret = demo_decoder_buffer_current(demo, decoder->output_type, &buffer);
	if (ret)
		return ret;

	/* Copy used length from camera source buffer. */
	if (demo->source == DEMO_SOURCE_CAMERA) {
		struct demo_camera *camera = &demo->camera;
		struct demo_buffer *camera_buffer =
			&camera->capture_buffers[decoder->capture_buffer_index];

		for (i = 0; i < buffer->planes_count; i++) {
			unsigned int size;

			v4l2_buffer_plane_length_used(&camera_buffer->buffer, i,
						      &size);
			v4l2_buffer_setup_plane_length_used(&buffer->buffer, i,
							    size);
		}
	}

	ret = v4l2_buffer_queue(decoder->video_fd, &buffer->buffer);
	if (ret) {
		fprintf(stderr, "Failed to queue output buffer\n");
		return ret;
	}

	ret = v4l2_stream_on(decoder->video_fd, decoder->capture_type);
	if (ret) {
		fprintf(stderr, "Failed to start capture stream\n");
		return ret;
	}

	ret = v4l2_stream_on(decoder->video_fd, decoder->output_type);
	if (ret) {
		fprintf(stderr, "Failed to start output stream\n");
		return ret;
	}

	ret = v4l2_poll(decoder->video_fd, &timeout);
	if (ret <= 0) {
		fprintf(stderr, "Error waiting for decode\n");
		return ret == 0 ? -ETIMEDOUT : ret;
	}

	v4l2_buffer_setup_base(&buffer_dequeue, decoder->capture_type,
			       decoder->capture_memory);

	ret = v4l2_buffer_dequeue(decoder->video_fd, &buffer_dequeue);
	if (ret) {
		fprintf(stderr, "Failed to dequeue capture buffer\n");
		return ret;
	}

	if (buffer_dequeue.index != decoder->capture_buffer_index)
		fprintf(stderr,
			"Dequeued unexpected capture buffer (%d vs %d)\n",
			buffer_dequeue.index, decoder->capture_buffer_index);

	v4l2_buffer_setup_base(&buffer_dequeue, decoder->output_type,
			       decoder->output_memory);

	ret = v4l2_buffer_dequeue(decoder->video_fd, &buffer_dequeue);
	if (ret) {
		fprintf(stderr, "Failed to dequeue output buffer\n");
		return ret;
	}

	if (buffer_dequeue.index != decoder->output_buffer_index)
		fprintf(stderr,
			"Dequeued unexpected output buffer (%d vs %d)\n",
			buffer_dequeue.index, decoder->output_buffer_index);

	ret = v4l2_stream_off(decoder->video_fd, decoder->capture_type);
	if (ret)
		return ret;

	ret = v4l2_stream_off(decoder->video_fd, decoder->output_type);
	if (ret)
		return ret;

	perf_after(&perf);

	perf_print(&perf, "decode");

	return 0;
}

int demo_decoder_setup(struct demo *demo)
{
	struct demo_decoder *decoder = &demo->decoder;
	unsigned int planes_count;
	unsigned int count;
	unsigned int size;
	unsigned int i;
	bool import_camera = false;
	bool check;
	int ret;

	if (decoder->video_fd < 0) {
		fprintf(stderr, "Failed to open decoder video device\n");
		return -ENODEV;
	}

	if (demo->source == DEMO_SOURCE_CAMERA)
		import_camera = true;

	if (demo->allocator == DEMO_ALLOCATOR_V4L2) {
		if (import_camera)
			decoder->output_memory = V4L2_MEMORY_DMABUF;
		else
			decoder->output_memory = V4L2_MEMORY_MMAP;

		decoder->capture_memory = V4L2_MEMORY_MMAP;
	} else if (demo->allocator == DEMO_ALLOCATOR_DMA_HEAP) {
		decoder->output_memory = V4L2_MEMORY_DMABUF;
		decoder->capture_memory = V4L2_MEMORY_DMABUF;
	} else {
		return -EINVAL;
	}

	decoder->output_width = demo->width;
	decoder->output_height = demo->height;
	decoder->output_pixel_format = V4L2_PIX_FMT_JPEG;

	decoder->capture_width = demo->width;
	decoder->capture_height = demo->height;
	decoder->capture_pixel_format = V4L2_PIX_FMT_NV16;

	decoder->output_type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	decoder->capture_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	planes_count = 1;

	/* Output pixel format check */

	check = v4l2_pixel_format_check(decoder->video_fd, decoder->output_type,
					decoder->output_pixel_format);
	if (!check) {
		fprintf(stderr, "Missing output pixel format support\n");
		return -EINVAL;
	}

	/* Capture pixel format check */

	check = v4l2_pixel_format_check(decoder->video_fd,
					decoder->capture_type,
					decoder->capture_pixel_format);
	if (!check) {
		fprintf(stderr, "Missing capture pixel format support\n");
		return -EINVAL;
	}

	/* Output format setup */

	v4l2_format_setup_base(&decoder->output_format, decoder->output_type);
	v4l2_format_setup_pixel(&decoder->output_format, decoder->output_width,
				decoder->output_height,
				decoder->output_pixel_format);

	if (import_camera) {
		struct demo_camera *camera = &demo->camera;
		v4l2_buffer_plane_length(&camera->capture_buffers[0].buffer, 0,
					 &size);
	} else {
		/* Let's assume that JPEG fits in width * height * 3 bytes. */
		size = decoder->output_width * decoder->output_height * 3;
	}

	v4l2_format_setup_sizeimage(&decoder->output_format, 0, size);

	ret = v4l2_format_try(decoder->video_fd, &decoder->output_format);
	if (ret) {
		fprintf(stderr, "Failed to try output format\n");
		return ret;
	}

	ret = v4l2_format_set(decoder->video_fd, &decoder->output_format);
	if (ret) {
		fprintf(stderr, "Failed to set output format\n");
		return ret;
	}

	/* Capture format setup */

	v4l2_format_setup_base(&decoder->capture_format, decoder->capture_type);
	v4l2_format_setup_pixel(&decoder->capture_format,
				decoder->capture_width, decoder->capture_height,
				decoder->capture_pixel_format);

	ret = v4l2_format_try(decoder->video_fd, &decoder->capture_format);
	if (ret) {
		fprintf(stderr, "Failed to try capture format\n");
		return ret;
	}

	ret = v4l2_format_set(decoder->video_fd, &decoder->capture_format);
	if (ret) {
		fprintf(stderr, "Failed to set capture format\n");
		return ret;
	}

	/* Output buffers setup */

	count = sizeof(decoder->output_buffers) /
		sizeof(decoder->output_buffers[0]);

	ret = v4l2_buffers_request(decoder->video_fd, decoder->output_type,
				   decoder->output_memory, count);
	if (ret) {
		fprintf(stderr, "Failed to allocate output buffers\n");
		return ret;
	}

	printf("Allocated %d output buffers for decoder\n", count);

	for (i = 0; i < count; i++) {
		ret = demo_buffer_setup(demo, &decoder->output_buffers[i],
					decoder->video_fd,
					decoder->output_memory,
					decoder->output_type, i, planes_count,
					import_camera);
		if (ret) {
			/* TODO: Cleanup previous allocations on error. */
			return ret;
		}
	}

	decoder->output_buffers_count = count;

	/* Capture buffers setup */

	count = sizeof(decoder->capture_buffers) /
		sizeof(decoder->capture_buffers[0]);

	ret = v4l2_buffers_request(decoder->video_fd, decoder->capture_type,
				   decoder->capture_memory, count);
	if (ret) {
		fprintf(stderr, "Failed to allocate capture buffers\n");
		/* TODO: Cleanup previous allocations on error. */
		return ret;
	}

	printf("Allocated %d capture buffers for decoder\n", count);

	for (i = 0; i < count; i++) {
		ret = demo_buffer_setup(demo, &decoder->capture_buffers[i],
					decoder->video_fd,
					decoder->capture_memory,
					decoder->capture_type, i, planes_count,
					false);
		if (ret) {
			/* TODO: Cleanup previous allocations on error. */
			return ret;
		}
	}

	decoder->capture_buffers_count = count;

	return 0;
}

void demo_decoder_cleanup(struct demo *demo)
{
	struct demo_decoder *decoder = &demo->decoder;
	unsigned int count;
	unsigned int i;

	/* Output buffers cleanup */

	for (i = 0; i < decoder->output_buffers_count; i++)
		demo_buffer_cleanup(&decoder->output_buffers[i]);

	v4l2_buffers_destroy(decoder->video_fd, decoder->output_type,
			     decoder->output_memory);

	/* Capture buffers cleanup */

	for (i = 0; i < decoder->capture_buffers_count; i++)
		demo_buffer_cleanup(&decoder->capture_buffers[i]);

	v4l2_buffers_destroy(decoder->video_fd, decoder->capture_type,
			     decoder->capture_memory);
}
