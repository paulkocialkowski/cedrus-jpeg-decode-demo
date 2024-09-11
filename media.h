/*
 * Copyright (C) 2020 Paul Kocialkowski <contact@paulk.fr>
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

#ifndef _MEDIA_H_
#define _MEDIA_H_

#include <stdbool.h>
#include <time.h>

int media_device_info(int media_fd, struct media_device_info *device_info);
int media_topology_get(int media_fd, struct media_v2_topology *topology);
struct media_v2_entity *media_topology_entity_find_by_function(struct media_v2_topology *topology,
							       unsigned int function);
struct media_v2_interface *media_topology_interface_find_by_id(struct media_v2_topology *topology,
							       unsigned int id);
struct media_v2_pad *media_topology_pad_find_by_entity(struct media_v2_topology *topology,
						       unsigned int entity_id,
						       unsigned int flags);
struct media_v2_pad *media_topology_pad_find_by_id(struct media_v2_topology *topology,
						   unsigned int id);
struct media_v2_link *media_topology_link_find_by_pad(struct media_v2_topology *topology,
						      unsigned int pad_id,
						      unsigned int pad_flags);
struct media_v2_link *media_topology_link_find_by_entity(struct media_v2_topology *topology,
							 unsigned int entity_id,
							 unsigned int pad_flags);
int media_request_alloc(int media_fd);
int media_request_queue(int request_fd);
int media_request_reinit(int request_fd);
int media_request_poll(int request_fd, struct timeval *timeout);

#endif
