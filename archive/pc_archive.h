/*
 * This file is a part of Pcompress, a chunked parallel multi-
 * algorithm lossless compression and decompression program.
 *
 * Copyright (C) 2012-2013 Moinak Ghosh. All rights reserved.
 * Use is subject to license terms.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.
 * If not, see <http://www.gnu.org/licenses/>.
 *
 * moinakg@belenix.org, http://moinakg.wordpress.com/
 *      
 */

#ifndef	_PC_ARCHIVE_H
#define	_PC_ARCHIVE_H

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pcompress.h>
#include <pc_arc_filter.h>

#ifdef	__cplusplus
extern "C" {
#endif

typedef struct {
	char *fpath;
	int typeflag;
	size_t size;
} archive_list_entry_t;

/*
 * Archiving related functions.
 */
int setup_archiver(pc_ctx_t *pctx, struct stat *sbuf);
int start_archiver(pc_ctx_t *pctx);
int setup_extractor(pc_ctx_t *pctx);
int start_extractor(pc_ctx_t *pctx);
int64_t archiver_read(void *ctx, void *buf, uint64_t count);
int64_t archiver_write(void *ctx, void *buf, uint64_t count);
int archiver_close(void *ctx);
int init_archive_mod();
int insert_filter_data(filter_func_ptr func, void *filter_private, const char *ext);
void init_filters(struct filter_flags *ff);

#ifdef	__cplusplus
}
#endif

#endif	
