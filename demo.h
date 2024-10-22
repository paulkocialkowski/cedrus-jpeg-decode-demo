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

#ifndef _DEMO_H_
#define _DEMO_H_

#include "v4l2.h"

enum demo_allocator {
	DEMO_ALLOCATOR_V4L2,
	DEMO_ALLOCATOR_DMA_HEAP,
};

enum demo_source {
	DEMO_SOURCE_FILE,
	DEMO_SOURCE_CAMERA,
};

struct demo_buffer {
	struct v4l2_buffer buffer;
	struct v4l2_plane planes[4];
	unsigned int planes_count;

	void *data[4];
	int dma_buf_fd[4];
};

struct demo_decoder {
	int video_fd;

	unsigned int output_memory;
	unsigned int output_type;
	unsigned int output_width;
	unsigned int output_height;
	unsigned int output_pixel_format;
	struct v4l2_format output_format;

	struct demo_buffer output_buffers[3];
	unsigned int output_buffers_count;
	unsigned int output_buffer_index;

	unsigned int capture_memory;
	unsigned int capture_type;
	unsigned int capture_width;
	unsigned int capture_height;
	unsigned int capture_pixel_format;
	struct v4l2_format capture_format;

	struct demo_buffer capture_buffers[3];
	unsigned int capture_buffers_count;
	unsigned int capture_buffer_index;
};

struct demo_camera {
	int video_fd;

	unsigned int capture_memory;
	unsigned int capture_type;
	unsigned int capture_width;
	unsigned int capture_height;
	unsigned int capture_pixel_format;
	struct v4l2_format capture_format;

	struct demo_buffer capture_buffers[3];
	unsigned int capture_buffers_count;
	unsigned int capture_buffer_index;
};

struct demo_file {
	int fd;
	unsigned int size;
};

struct demo {
	int source;
	int allocator;

	int dma_heap_fd;

	unsigned int width;
	unsigned int height;

	struct demo_file file;
	struct demo_decoder decoder;
	struct demo_camera camera;
};

int demo_buffer_sync(struct demo_buffer *buffer, long flags);
int demo_buffer_sync_begin(struct demo_buffer *buffer);
int demo_buffer_sync_finish(struct demo_buffer *buffer);

int demo_buffer_setup(struct demo *demo, struct demo_buffer *buffer,
		      int video_fd, unsigned int memory, unsigned int type,
		      unsigned int index, unsigned int planes_count,
		      bool import_camera);
void demo_buffer_cleanup(struct demo_buffer *buffer);

int demo_decoder_buffer_current(struct demo *demo, unsigned int type,
				struct demo_buffer **buffer);
int demo_decoder_buffer_cycle(struct demo *demo, unsigned int type);
int demo_decoder_run(struct demo *demo);
int demo_decoder_setup(struct demo *demo);
void demo_decoder_cleanup(struct demo *demo);

int demo_camera_buffer_current(struct demo *demo, struct demo_buffer **buffer);
int demo_camera_buffer_cycle(struct demo *demo);
int demo_camera_roll(struct demo *demo);
int demo_camera_setup(struct demo *demo);
void demo_camera_cleanup(struct demo *demo);

#endif
