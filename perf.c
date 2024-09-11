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
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

#include "perf.h"

void perf_before(struct perf *perf)
{
	clock_gettime(CLOCK_MONOTONIC, &perf->before);
}

void perf_after(struct perf *perf)
{
	clock_gettime(CLOCK_MONOTONIC, &perf->after);
}

void perf_print(struct perf *perf, const char *step)
{
	uint64_t diff = timespec_diff(perf->before, perf->after) / 1000UL;

	printf("+ Perf time for step %s: %"PRIu64" us\n", step, diff);
}
