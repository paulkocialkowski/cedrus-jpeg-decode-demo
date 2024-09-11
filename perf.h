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

#ifndef _PERF_H_
#define _PERF_H_

#include <time.h>

#define timespec_ns(t) \
	(t.tv_sec * 1000000000UL + t.tv_nsec)
#define timespec_diff(tb, ta) \
	(timespec_ns(ta) - timespec_ns(tb))

struct perf {
	struct timespec before;
	struct timespec after;
};

void perf_before(struct perf *perf);
void perf_after(struct perf *perf);
void perf_print(struct perf *perf, const char *step);

#endif
