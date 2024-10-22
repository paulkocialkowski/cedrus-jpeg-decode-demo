# Copyright (C) 2024 Paul Kocialkowski <contact@paulk.fr>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

PROJECT = cedrus-jpeg-decode-demo

BINARY = $(PROJECT)
SOURCES = demo.c demo_decoder.c demo_camera.c dma_buf.c dma_heap.c v4l2.c media.c perf.c
OBJECTS = $(SOURCES:.c=.o)
DEPENDS = $(SOURCES:.c=.d)

CC = gcc
CFLAGS =
LDFLAGS = -ludev

all: $(BINARY)

$(OBJECTS): %.o: %.c
	@echo " CC     $<"
	@$(CC) $(CFLAGS) -MMD -MF $*.d -c $< -o $@

$(BINARY): $(OBJECTS)
	@echo " LINK   $@"
	@$(CC) $(CFLAGS) -o $@ $(OBJECTS) $(LDFLAGS)

.PHONY: clean
clean:
	@echo " CLEAN"
	@rm -f $(BINARY) $(OBJECTS) $(DEPENDS)

-include $(DEPENDS)
