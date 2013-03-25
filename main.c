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

/*
 * pcompress - Do a chunked parallel compression/decompression of a file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <strings.h>
#include <limits.h>
#include <unistd.h>
#include <signal.h>
#if defined(sun) || defined(__sun)
#include <sys/byteorder.h>
#else
#include <byteswap.h>
#endif
#include <libgen.h>
#include <utils.h>
#include <pcompress.h>
#include <allocator.h>
#include <rabin_dedup.h>
#include <lzp.h>
#include <transpose.h>
#include <delta2/delta2.h>
#include <crypto/crypto_utils.h>
#include <crypto_xsalsa20.h>
#include <ctype.h>

/*
 * We use 5MB chunks by default.
 */
#define	DEFAULT_CHUNKSIZE	(5 * 1024 * 1024)
#define	EIGHTY_PCT(x) ((x) - ((x)/5))

struct wdata {
	struct cmp_data **dary;
	int wfd;
	int nprocs;
	int64_t chunksize;
};


static void * writer_thread(void *dat);
static int init_algo(const char *algo, int bail);
extern uint32_t lzma_crc32(const uint8_t *buf, uint64_t size, uint32_t crc);

static compress_func_ptr _compress_func;
static compress_func_ptr _decompress_func;
static init_func_ptr _init_func;
static deinit_func_ptr _deinit_func;
static stats_func_ptr _stats_func;
static props_func_ptr _props_func;

static int main_cancel;
static int adapt_mode = 0;
static int pipe_mode = 0;
static int nthreads = 0;
static int hide_mem_stats = 1;
static int hide_cmp_stats = 1;
static int enable_rabin_scan = 0;
static int enable_rabin_global = 0;
static int enable_delta_encode = 0;
static int enable_delta2_encode = 0;
static int enable_rabin_split = 1;
static int enable_fixed_scan = 0;
static int lzp_preprocess = 0;
static int encrypt_type = 0;
static unsigned int chunk_num;
static uint64_t largest_chunk, smallest_chunk, avg_chunk;
static const char *exec_name;
static const char *algo = NULL;
static int do_compress = 0;
static int do_uncompress = 0;
static int cksum_bytes, mac_bytes;
static int cksum = 0, t_errored = 0;
static int rab_blk_size = 0, keylen;
static crypto_ctx_t crypto_ctx;
static char *pwd_file = NULL, *f_name = NULL;

static void
usage(void)
{
	fprintf(stderr,
	    "\nPcompress Version %s\n\n"
	    "Usage:\n"
	    "1) To compress a file:\n"
	    "   %s -c <algorithm> [-l <compress level>] [-s <chunk size>] <file>\n"
	    "   Where <algorithm> can be the folowing:\n"
	    "   lzfx   - Very fast and small algorithm based on LZF.\n"
	    "   lz4    - Ultra fast, high-throughput algorithm reaching RAM B/W at level1.\n"
	    "   zlib   - The base Zlib format compression (not Gzip).\n"
	    "   lzma   - The LZMA (Lempel-Ziv Markov) algorithm from 7Zip.\n"
	    "   lzmaMt - Multithreaded version of LZMA. This is a faster version but\n"
	    "            uses more memory for the dictionary. Thread count is balanced\n"
	    "            between chunk processing threads and algorithm threads.\n"
	    "   bzip2  - Bzip2 Algorithm from libbzip2.\n"
	    "   ppmd   - The PPMd algorithm excellent for textual data. PPMd requires\n"
	    "            at least 64MB X CPUs more memory than the other modes.\n"
#ifdef ENABLE_PC_LIBBSC
	    "   libbsc - A Block Sorting Compressor using the Burrows Wheeler Transform\n"
	    "            like Bzip2 but runs faster and gives better compression than\n"
	    "            Bzip2 (See: libbsc.com).\n"
#endif
	    "   adapt  - Adaptive mode where ppmd or bzip2 will be used per chunk,\n"
	    "            depending on which one produces better compression. This mode\n"
	    "            is obviously fairly slow and requires lots of memory.\n"
	    "   adapt2 - Adaptive mode which includes ppmd and lzma. This requires\n"
	    "            more memory than adapt mode, is slower and potentially gives\n"
	    "            the best compression.\n"
	    "   none   - No compression. This is only meaningful with -D and -E so Dedupe\n"
	    "            can be done for post-processing with an external utility.\n"
	    "   <chunk_size> - This can be in bytes or can use the following suffixes:\n"
	    "            g - Gigabyte, m - Megabyte, k - Kilobyte.\n"
	    "            Larger chunks produce better compression at the cost of memory.\n"
	    "   <compress_level> - Can be a number from 0 meaning minimum and 14 meaning\n"
	    "            maximum compression.\n\n"
	    "2) To decompress a file compressed using above command:\n"
	    "   %s -d <compressed file> <target file>\n"
	    "3) To operate as a pipe, read from stdin and write to stdout:\n"
	    "   %s -p ...\n"
	    "4) Attempt Rabin fingerprinting based deduplication on chunks:\n"
	    "   %s -D ...\n"
	    "   %s -D -r ... - Do NOT split chunks at a rabin boundary. Default is to split.\n\n"
	    "5) Perform Delta Encoding in addition to Identical Dedup:\n"
	    "   %s -E ... - This also implies '-D'. This checks for at least 60%% similarity.\n"
	    "   The flag can be repeated as in '-EE' to indicate at least 40%% similarity.\n\n"
	    "6) Number of threads can optionally be specified: -t <1 - 256 count>\n"
	    "7) Other flags:\n"
	    "   '-L'    - Enable LZP pre-compression. This improves compression ratio of all\n"
	    "             algorithms with some extra CPU and very low RAM overhead.\n"
	    "   '-P'    - Enable Adaptive Delta Encoding. It can improve compresion ratio for\n"
	    "             data containing tables of numerical values especially if those are in\n"
	    "             an arithmetic series.\n"
	    "   NOTE    - Both -L and -P can be used together to give maximum benefit on most.\n"
	    "             datasets.\n"
	    "   '-S' <cksum>\n"
	    "           - Specify chunk checksum to use:\n\n",
	    UTILITY_VERSION, exec_name, exec_name, exec_name, exec_name, exec_name, exec_name);
	list_checksums(stderr, "             ");
	fprintf(stderr, "\n"
	    "   '-F'    - Perform Fixed-Block Deduplication. Faster than '-D' in some cases\n"
	    "             but with lower deduplication ratio.\n"
	    "   '-B' <1..5>\n"
	    "           - Specify an average Dedupe block size. 1 - 4K, 2 - 8K ... 5 - 64K.\n"
	    "   '-M'    - Display memory allocator statistics\n"
	    "   '-C'    - Display compression statistics\n\n");
	fprintf(stderr, "\n"
	    "8) Encryption flags:\n"
	    "   '-e <ALGO>'\n"
	    "           - Encrypt chunks with the given encrption algorithm. The ALGO parameter\n"
	    "             can be one of AES or SALSA20. Both are used in CTR stream encryption\n"
	    "             mode. The password can be prompted from the user or read from a file.\n"
	    "             Unique keys are generated every time pcompress is run even when giving\n"
	    "             the same password. Default key length is 256-bits (see -k below).\n"
	    "   '-w <pathname>'\n"
	    "           - Provide a file which contains the encryption password. This file must\n"
	    "             be readable and writable since it is zeroed out after the password is\n"
	    "             read.\n"
	    "   '-k <key length>\n"
	    "           - Specify key length. Can be 16 for 128 bit or 32 for 256 bit. Default\n"
	    "             is 32 for 256 bit keys.\n\n");
}

void
show_compression_stats(uint64_t chunksize)
{
	fprintf(stderr, "\nCompression Statistics\n");
	fprintf(stderr, "======================\n");
	fprintf(stderr, "Total chunks           : %u\n", chunk_num);
	fprintf(stderr, "Best compressed chunk  : %s(%.2f%%)\n",
	    bytes_to_size(smallest_chunk), (double)smallest_chunk/(double)chunksize*100);
	fprintf(stderr, "Worst compressed chunk : %s(%.2f%%)\n",
	    bytes_to_size(largest_chunk), (double)largest_chunk/(double)chunksize*100);
	avg_chunk /= chunk_num;
	fprintf(stderr, "Avg compressed chunk   : %s(%.2f%%)\n\n",
	    bytes_to_size(avg_chunk), (double)avg_chunk/(double)chunksize*100);
}

void
Int_Handler(int signo)
{
	if (f_name != NULL) {
		unlink(f_name);
	}
	exit(1);
}

/*
 * Wrapper functions to pre-process the buffer and then call the main compression routine.
 * At present only LZP pre-compression is used below. Some extra metadata is added:
 * 
 * Byte 0: A flag to indicate which pre-processor was used.
 * Byte 1 - Byte 8: Size of buffer after pre-processing
 * 
 * It is possible for a buffer to be only pre-processed and not compressed by the final
 * algorithm if the final one fails to compress for some reason. However the vice versa
 * is not allowed.
 */
int
preproc_compress(compress_func_ptr cmp_func, void *src, uint64_t srclen, void *dst,
	uint64_t *dstlen, int level, uchar_t chdr, void *data, algo_props_t *props)
{
	uchar_t *dest = (uchar_t *)dst, type = 0;
	int64_t result;
	uint64_t _dstlen;
	DEBUG_STAT_EN(double strt, en);

	_dstlen = *dstlen;
	if (lzp_preprocess) {
		int hashsize;

		hashsize = lzp_hash_size(level);
		result = lzp_compress((const uchar_t *)src, (uchar_t *)dst, srclen,
				      hashsize, LZP_DEFAULT_LZPMINLEN, 0);
		if (result < 0 || result == srclen) {
			if (!enable_delta2_encode)
				return (-1);
		} else {
			type |= PREPROC_TYPE_LZP;
			srclen = result;
			memcpy(src, dst, srclen);
		}

	} else if (!enable_delta2_encode)  {
		/*
		 * Execution won't come here but just in case ...
		 */
		fprintf(stderr, "Invalid preprocessing mode\n");
		return (-1);
	}

	if (enable_delta2_encode && props->delta2_span > 0) {
		_dstlen = srclen;
		result = delta2_encode((uchar_t *)src, srclen, (uchar_t *)dst,
				       &_dstlen, props->delta2_span);
		if (result != -1) {
			memcpy(src, dst, _dstlen);
			srclen = _dstlen;
			type |= PREPROC_TYPE_DELTA2;
		}
	}

	*dest = type;
	*((uint64_t *)(dest + 1)) = htonll(srclen);
	_dstlen = srclen;
	DEBUG_STAT_EN(strt = get_wtime_millis());
	result = cmp_func(src, srclen, dest+9, &_dstlen, level, chdr, data);
	DEBUG_STAT_EN(en = get_wtime_millis());

	if (result > -1 && _dstlen < srclen) {
		*dest |= PREPROC_COMPRESSED;
		*dstlen = _dstlen + 9;
		DEBUG_STAT_EN(fprintf(stderr, "Chunk compression speed %.3f MB/s\n", get_mb_s(srclen, strt, en)));
	} else {
		DEBUG_STAT_EN(fprintf(stderr, "Chunk did not compress.\n"));
		memcpy(dest+1, src, srclen);
		*dstlen = srclen + 1;
		/*
		 * If compression failed but one of the pre-processing succeeded then
		 * type flags will be non-zero. In that case we still indicate a success
		 * result so that decompression will reverse the pre-processing. The
		 * type flags will indicate that compression was not done and the
		 * decompress routine will not be called.
		 */
		if (type > 0)
			result = 0;
	}
	return (result);
}

int
preproc_decompress(compress_func_ptr dec_func, void *src, uint64_t srclen, void *dst,
	uint64_t *dstlen, int level, uchar_t chdr, void *data, algo_props_t *props)
{
	uchar_t *sorc = (uchar_t *)src, type;
	int64_t result;
	uint64_t _dstlen = *dstlen;
	DEBUG_STAT_EN(double strt, en);

	type = *sorc;
	++sorc;
	--srclen;
	if (type & PREPROC_COMPRESSED) {
		*dstlen = ntohll(*((uint64_t *)(sorc)));
		sorc += 8;
		srclen -= 8;
		DEBUG_STAT_EN(strt = get_wtime_millis());
		result = dec_func(sorc, srclen, dst, dstlen, level, chdr, data);
		DEBUG_STAT_EN(en = get_wtime_millis());

		if (result < 0) return (result);
		DEBUG_STAT_EN(fprintf(stderr, "Chunk decompression speed %.3f MB/s\n", get_mb_s(srclen, strt, en)));
		memcpy(src, dst, *dstlen);
		srclen = *dstlen;
	} else {
		src = sorc;
	}

	if (type & PREPROC_TYPE_DELTA2) {
		result = delta2_decode((uchar_t *)src, srclen, (uchar_t *)dst, &_dstlen);
		if (result != -1) {
			memcpy(src, dst, _dstlen);
			srclen = _dstlen;
			*dstlen = _dstlen;
	} else {
			return (result);
		}
	}

	if (type & PREPROC_TYPE_LZP) {
		int hashsize;
		hashsize = lzp_hash_size(level);
		result = lzp_decompress((const uchar_t *)src, (uchar_t *)dst, srclen,
					hashsize, LZP_DEFAULT_LZPMINLEN, 0);
		if (result < 0) {
			fprintf(stderr, "LZP decompression failed.\n");
			return (-1);
		}
		*dstlen = result;
	}

	if (!(type & (PREPROC_COMPRESSED | PREPROC_TYPE_DELTA2 | PREPROC_TYPE_LZP)) && type > 0) {
		fprintf(stderr, "Invalid preprocessing flags: %d\n", type);
		return (-1);
	}
	return (0);
}

/*
 * This routine is called in multiple threads. Calls the decompression handler
 * as encoded in the file header. For adaptive mode the handler adapt_decompress()
 * in turns looks at the chunk header and calls the actual decompression
 * routine.
 */
static void *
perform_decompress(void *dat)
{
	struct cmp_data *tdat = (struct cmp_data *)dat;
	uint64_t _chunksize;
	uint64_t dedupe_index_sz, dedupe_data_sz, dedupe_index_sz_cmp, dedupe_data_sz_cmp;
	int rv = 0;
	unsigned int blknum;
	uchar_t checksum[CKSUM_MAX_BYTES];
	uchar_t HDR;
	uchar_t *cseg;

redo:
	sem_wait(&tdat->start_sem);
	if (unlikely(tdat->cancel)) {
		tdat->len_cmp = 0;
		sem_post(&tdat->cmp_done_sem);
		return (0);
	}

	/*
	 * If the last read returned a 0 quit.
	 */
	if (tdat->rbytes == 0) {
		tdat->len_cmp = 0;
		goto cont;
	}

	cseg = tdat->compressed_chunk + cksum_bytes + mac_bytes;
	HDR = *cseg;
	cseg += CHUNK_FLAG_SZ;
	_chunksize = tdat->chunksize;
	if (HDR & CHSIZE_MASK) {
		uchar_t *rseg;

		tdat->rbytes -= ORIGINAL_CHUNKSZ;
		tdat->len_cmp -= ORIGINAL_CHUNKSZ;
		rseg = tdat->compressed_chunk + tdat->rbytes;
		_chunksize = ntohll(*((int64_t *)rseg));
	}

	/*
	 * If this was encrypted:
	 * Verify HMAC first before anything else and then decrypt compressed data.
	 */
	if (encrypt_type) {
		unsigned int len;
		DEBUG_STAT_EN(double strt, en);

		DEBUG_STAT_EN(strt = get_wtime_millis());
		len = mac_bytes;
		deserialize_checksum(checksum, tdat->compressed_chunk + cksum_bytes, mac_bytes);
		memset(tdat->compressed_chunk + cksum_bytes, 0, mac_bytes);
		hmac_reinit(&tdat->chunk_hmac);
		hmac_update(&tdat->chunk_hmac, (uchar_t *)&tdat->len_cmp_be, sizeof (tdat->len_cmp_be));
		hmac_update(&tdat->chunk_hmac, tdat->compressed_chunk, tdat->rbytes);
		if (HDR & CHSIZE_MASK) {
			uchar_t *rseg;
			rseg = tdat->compressed_chunk + tdat->rbytes;
			hmac_update(&tdat->chunk_hmac, rseg, ORIGINAL_CHUNKSZ);
		}
		hmac_final(&tdat->chunk_hmac, tdat->checksum, &len);
		if (memcmp(checksum, tdat->checksum, len) != 0) {
			/*
			 * HMAC verification failure is fatal.
			 */
			fprintf(stderr, "Chunk %d, HMAC verification failed\n", tdat->id);
			main_cancel = 1;
			tdat->len_cmp = 0;
			t_errored = 1;
			sem_post(&tdat->cmp_done_sem);
			return (NULL);
		}
		DEBUG_STAT_EN(en = get_wtime_millis());
		DEBUG_STAT_EN(fprintf(stderr, "HMAC Verification speed %.3f MB/s\n",
			      get_mb_s(tdat->rbytes + sizeof (tdat->len_cmp_be), strt, en)));

		/*
		 * Encryption algorithm should not change the size and
		 * encryption is in-place.
		 */
		DEBUG_STAT_EN(strt = get_wtime_millis());
		rv = crypto_buf(&crypto_ctx, cseg, cseg, tdat->len_cmp, tdat->id);
		if (rv == -1) {
			/*
			 * Decryption failure is fatal.
			 */
			main_cancel = 1;
			tdat->len_cmp = 0;
			sem_post(&tdat->cmp_done_sem);
			return (NULL);
		}
		DEBUG_STAT_EN(en = get_wtime_millis());
		DEBUG_STAT_EN(fprintf(stderr, "Decryption speed %.3f MB/s\n",
			      get_mb_s(tdat->len_cmp, strt, en)));
	} else if (mac_bytes > 0) {
		/*
		 * Verify header CRC32 in non-crypto mode.
		 */
		uint32_t crc1, crc2;

		crc1 = htonl(*((uint32_t *)(tdat->compressed_chunk + cksum_bytes)));
		memset(tdat->compressed_chunk + cksum_bytes, 0, mac_bytes);
		crc2 = lzma_crc32((uchar_t *)&tdat->len_cmp_be, sizeof (tdat->len_cmp_be), 0);
		crc2 = lzma_crc32(tdat->compressed_chunk,
		    cksum_bytes + mac_bytes + CHUNK_FLAG_SZ, crc2);
		if (HDR & CHSIZE_MASK) {
			uchar_t *rseg;
			rseg = tdat->compressed_chunk + tdat->rbytes;
			crc2 = lzma_crc32(rseg, ORIGINAL_CHUNKSZ, crc2);
		}

		if (crc1 != crc2) {
			/*
			 * Header CRC32 verification failure is fatal.
			 */
			fprintf(stderr, "Chunk %d, Header CRC verification failed\n", tdat->id);
			main_cancel = 1;
			tdat->len_cmp = 0;
			t_errored = 1;
			sem_post(&tdat->cmp_done_sem);
			return (NULL);
		}

		/*
		 * Now that header CRC32 was verified, recover the stored message
		 * digest.
		 */
		deserialize_checksum(tdat->checksum, tdat->compressed_chunk, cksum_bytes);
	}

	if ((enable_rabin_scan || enable_fixed_scan || enable_rabin_global) &&
	    (HDR & CHUNK_FLAG_DEDUP)) {
		uchar_t *cmpbuf, *ubuf;
		/* Extract various sizes from dedupe header. */
		parse_dedupe_hdr(cseg, &blknum, &dedupe_index_sz, &dedupe_data_sz,
				&dedupe_index_sz_cmp, &dedupe_data_sz_cmp, &_chunksize);
		memcpy(tdat->uncompressed_chunk, cseg, RABIN_HDR_SIZE);

		/*
		 * Uncompress the data chunk first and then uncompress the index.
		 * The uncompress routines can use extra bytes at the end for temporary
		 * state/dictionary info. Since data chunk directly follows index
		 * uncompressing index first corrupts the data.
		 */
		cmpbuf = cseg + RABIN_HDR_SIZE + dedupe_index_sz_cmp;
		ubuf = tdat->uncompressed_chunk + RABIN_HDR_SIZE + dedupe_index_sz;
		if (HDR & COMPRESSED) {
			if (HDR & CHUNK_FLAG_PREPROC) {
				rv = preproc_decompress(tdat->decompress, cmpbuf, dedupe_data_sz_cmp,
				    ubuf, &_chunksize, tdat->level, HDR, tdat->data, tdat->props);
			} else {
				DEBUG_STAT_EN(double strt, en);

				DEBUG_STAT_EN(strt = get_wtime_millis());
				rv = tdat->decompress(cmpbuf, dedupe_data_sz_cmp, ubuf, &_chunksize,
				    tdat->level, HDR, tdat->data);
				DEBUG_STAT_EN(en = get_wtime_millis());
				DEBUG_STAT_EN(fprintf(stderr, "Chunk %d decompression speed %.3f MB/s\n",
						      tdat->id, get_mb_s(_chunksize, strt, en)));
			}
			if (rv == -1) {
				tdat->len_cmp = 0;
				fprintf(stderr, "ERROR: Chunk %d, decompression failed.\n", tdat->id);
				t_errored = 1;
				goto cont;
			}
		} else {
			memcpy(ubuf, cmpbuf, _chunksize);
		}

		rv = 0;
		cmpbuf = cseg + RABIN_HDR_SIZE;
		ubuf = tdat->uncompressed_chunk + RABIN_HDR_SIZE;

		if (dedupe_index_sz >= 90 && dedupe_index_sz > dedupe_index_sz_cmp) {
			/* Index should be at least 90 bytes to have been compressed. */
			rv = lzma_decompress(cmpbuf, dedupe_index_sz_cmp, ubuf,
			    &dedupe_index_sz, tdat->rctx->level, 0, tdat->rctx->lzma_data);
		} else {
			memcpy(ubuf, cmpbuf, dedupe_index_sz);
		}

		/*
		 * Recover from transposed index.
		 */
		transpose(ubuf, cmpbuf, dedupe_index_sz, sizeof (uint32_t), COL);
		memcpy(ubuf, cmpbuf, dedupe_index_sz);

	} else {
		if (HDR & COMPRESSED) {
			if (HDR & CHUNK_FLAG_PREPROC) {
				rv = preproc_decompress(tdat->decompress, cseg, tdat->len_cmp,
				    tdat->uncompressed_chunk, &_chunksize, tdat->level, HDR, tdat->data,
				    tdat->props);
			} else {
				DEBUG_STAT_EN(double strt, en);

				DEBUG_STAT_EN(strt = get_wtime_millis());
				rv = tdat->decompress(cseg, tdat->len_cmp, tdat->uncompressed_chunk,
				    &_chunksize, tdat->level, HDR, tdat->data);
				DEBUG_STAT_EN(en = get_wtime_millis());
				DEBUG_STAT_EN(fprintf(stderr, "Chunk decompression speed %.3f MB/s\n",
						get_mb_s(_chunksize, strt, en)));
			}
		} else {
			memcpy(tdat->uncompressed_chunk, cseg, _chunksize);
		}
	}
	tdat->len_cmp = _chunksize;

	if (rv == -1) {
		tdat->len_cmp = 0;
		fprintf(stderr, "ERROR: Chunk %d, decompression failed.\n", tdat->id);
		t_errored = 1;
		goto cont;
	}
	/* Rebuild chunk from dedup blocks. */
	if ((enable_rabin_scan || enable_fixed_scan) && (HDR & CHUNK_FLAG_DEDUP)) {
		dedupe_context_t *rctx;
		uchar_t *tmp;

		rctx = tdat->rctx;
		reset_dedupe_context(tdat->rctx);
		rctx->cbuf = tdat->compressed_chunk;
		dedupe_decompress(rctx, tdat->uncompressed_chunk, &(tdat->len_cmp));
		if (!rctx->valid) {
			fprintf(stderr, "ERROR: Chunk %d, dedup recovery failed.\n", tdat->id);
			rv = -1;
			tdat->len_cmp = 0;
			t_errored = 1;
			goto cont;
		}
		_chunksize = tdat->len_cmp;
		tmp = tdat->uncompressed_chunk;
		tdat->uncompressed_chunk = tdat->compressed_chunk;
		tdat->compressed_chunk = tmp;
		tdat->cmp_seg = tdat->uncompressed_chunk;
	} else {
		/*
		 * This chunk was not deduplicated, however we still need to down the
		 * semaphore in order to maintain proper thread coordination. We do this after
		 * decompression to achieve better concurrency. Decompression does not need
		 * to wait for the previous thread's dedupe recovery to complete.
		 */
		if (enable_rabin_global) {
			sem_wait(tdat->rctx->index_sem);
		}
	}

	if (!encrypt_type) {
		/*
		 * Re-compute checksum of original uncompressed chunk.
		 * If it does not match we set length of chunk to 0 to indicate
		 * exit to the writer thread.
		 */
		compute_checksum(checksum, cksum, tdat->uncompressed_chunk, _chunksize, tdat->cksum_mt, 1);
		if (memcmp(checksum, tdat->checksum, cksum_bytes) != 0) {
			tdat->len_cmp = 0;
			fprintf(stderr, "ERROR: Chunk %d, checksums do not match.\n", tdat->id);
			t_errored = 1;
		}
	}

cont:
	sem_post(&tdat->cmp_done_sem);
	goto redo;
}

/*
 * File decompression routine.
 *
 * Compressed file Format
 * ----------------------
 * File Header:
 * Algorithm string:  8 bytes.
 * Version number:    2 bytes.
 * Global Flags:      2 bytes.
 * Chunk size:        8 bytes.
 * Compression Level: 4 bytes.
 *
 * Chunk Header:
 * Compressed length: 8 bytes.
 * Checksum:          Upto 64 bytes.
 * Chunk flags:       1 byte.
 * 
 * Chunk Flags, 8 bits:
 * I  I  I  I  I  I  I  I
 * |  |     |     |  |  |
 * |  '-----'     |  |  `- 0 - Uncompressed
 * |     |        |  |     1 - Compressed
 * |     |        |  |   
 * |     |        |  `---- 1 - Chunk was Deduped
 * |     |        `------- 1 - Chunk was pre-compressed
 * |     |
 * |     |                 1 - Bzip2 (Adaptive Mode)
 * |     `---------------- 2 - Lzma (Adaptive Mode)
 * |                       3 - PPMD (Adaptive Mode)
 * |
 * `---------------------- 1 - Chunk size flag (if original chunk is of variable length)
 *
 * A file trailer to indicate end.
 * Zero Compressed length: 8 zero bytes.
 */
#define UNCOMP_BAIL err = 1; goto uncomp_done

static int
start_decompress(const char *filename, const char *to_filename)
{
	char algorithm[ALGO_SZ];
	struct stat sbuf;
	struct wdata w;
	int compfd = -1, p, dedupe_flag;
	int uncompfd = -1, err, np, bail;
	int nprocs = 1, thread = 0, level;
	unsigned int i;
	short version, flags;
	int64_t chunksize, compressed_chunksize;
	struct cmp_data **dary, *tdat;
	pthread_t writer_thr;
	algo_props_t props;

	err = 0;
	flags = 0;
	thread = 0;
	dary = NULL;
	init_algo_props(&props);

	/*
	 * Open files and do sanity checks.
	 */
	if (!pipe_mode) {
		if ((compfd = open(filename, O_RDONLY, 0)) == -1)
			err_exit(1, "Cannot open: %s", filename);

		if (fstat(compfd, &sbuf) == -1)
			err_exit(1, "Cannot stat: %s", filename);
		if (sbuf.st_size == 0)
			return (1);

		if ((uncompfd = open(to_filename, O_WRONLY|O_CREAT|O_TRUNC, S_IRUSR | S_IWUSR)) == -1) {
			close(compfd);
			err_exit(1, "Cannot open: %s", to_filename);
		}
	} else {
		compfd = fileno(stdin);
		if (compfd == -1) {
			perror("fileno ");
			UNCOMP_BAIL;
		}
		uncompfd = fileno(stdout);
		if (uncompfd == -1) {
			perror("fileno ");
			UNCOMP_BAIL;
		}
	}

	/*
	 * Read file header pieces and verify.
	 */
	if (Read(compfd, algorithm, ALGO_SZ) < ALGO_SZ) {
		perror("Read: ");
		UNCOMP_BAIL;
	}
	if (init_algo(algorithm, 0) != 0) {
		fprintf(stderr, "%s is not a pcompressed file.\n", filename);
		UNCOMP_BAIL;
	}
	algo = algorithm;

	if (Read(compfd, &version, sizeof (version)) < sizeof (version) ||
	    Read(compfd, &flags, sizeof (flags)) < sizeof (flags) ||
	    Read(compfd, &chunksize, sizeof (chunksize)) < sizeof (chunksize) ||
	    Read(compfd, &level, sizeof (level)) < sizeof (level)) {
		perror("Read: ");
		UNCOMP_BAIL;
	}

	version = ntohs(version);
	flags = ntohs(flags);
	chunksize = ntohll(chunksize);
	level = ntohl(level);

	/*
	 * Check for ridiculous values (malicious tampering or otherwise).
	 */
	if (version > VERSION) {
		fprintf(stderr, "Cannot handle newer archive version %d, capability %d\n",
			version, VERSION);
		err = 1;
		goto uncomp_done;
	}
	if (chunksize > EIGHTY_PCT(get_total_ram())) {
		fprintf(stderr, "Chunk size must not exceed 80%% of total RAM.\n");
		err = 1;
		goto uncomp_done;
	}
	if (level > MAX_LEVEL || level < 0) {
		fprintf(stderr, "Invalid compression level in header: %d\n", level);
		err = 1;
		goto uncomp_done;
	}
	if (version < VERSION-3) {
		fprintf(stderr, "Unsupported version: %d\n", version);
		err = 1;
		goto uncomp_done;
	}

	compressed_chunksize = chunksize + CHUNK_HDR_SZ + zlib_buf_extra(chunksize);

	if (_props_func) {
		_props_func(&props, level, chunksize);
		if (chunksize + props.buf_extra > compressed_chunksize) {
			compressed_chunksize += (chunksize + props.buf_extra - 
			    compressed_chunksize);
		}
	}

	dedupe_flag = RABIN_DEDUPE_SEGMENTED; // Silence the compiler
	if (flags & FLAG_DEDUP) {
		enable_rabin_scan = 1;
		dedupe_flag = RABIN_DEDUPE_SEGMENTED;

		if (flags & FLAG_DEDUP_FIXED) {
			if (version > 7) {
				if (pipe_mode) {
					fprintf(stderr, "Global Deduplication is not supported with pipe mode.\n");
					err = 1;
					goto uncomp_done;
				}
				enable_rabin_global = 1;
				dedupe_flag = RABIN_DEDUPE_FILE_GLOBAL;
			} else {
				fprintf(stderr, "Invalid file deduplication flags.\n");
				err = 1;
				goto uncomp_done;
			}
		}
	} else if (flags & FLAG_DEDUP_FIXED) {
		enable_fixed_scan = 1;
		dedupe_flag = RABIN_DEDUPE_FIXED;
	}

	if (flags & FLAG_SINGLE_CHUNK) {
		props.is_single_chunk = 1;
	}

	cksum = flags & CKSUM_MASK;

	/*
	 * Backward compatibility check for SKEIN in archives version 5 or below.
	 * In newer versions BLAKE uses same IDs as SKEIN.
	 */
	if (version <= 5) {
		if (cksum == CKSUM_BLAKE256) cksum = CKSUM_SKEIN256;
		if (cksum == CKSUM_BLAKE512) cksum = CKSUM_SKEIN512;
	}
	if (get_checksum_props(NULL, &cksum, &cksum_bytes, &mac_bytes, 1) == -1) {
		fprintf(stderr, "Invalid checksum algorithm code: %d. File corrupt ?\n", cksum);
		UNCOMP_BAIL;
	}

	/*
	 * Archives older than 5 did not support MACs.
	 */
	if (version < 5)
		mac_bytes = 0;

	/*
	 * If encryption is enabled initialize crypto.
	 */
	if (flags & MASK_CRYPTO_ALG) {
		int saltlen, noncelen;
		uchar_t *salt1, *salt2;
		uchar_t nonce[MAX_NONCE], n1[MAX_NONCE];
		uchar_t pw[MAX_PW_LEN];
		int pw_len;
		mac_ctx_t hdr_mac;
		uchar_t hdr_hash1[mac_bytes], hdr_hash2[mac_bytes];
		unsigned int hlen;
		unsigned short d1;
		unsigned int d2;
		uint64_t d3;

		/*
		 * In encrypted files we do not have a normal digest. The HMAC
		 * is computed over header and encrypted data.
		 */
		cksum_bytes = 0;
		pw_len = -1;
		compressed_chunksize += mac_bytes;
		encrypt_type = flags & MASK_CRYPTO_ALG;
		if (version < 7)
			keylen = OLD_KEYLEN;

		if (encrypt_type == CRYPTO_ALG_AES) {
			noncelen = 8;
		} else if (encrypt_type == CRYPTO_ALG_SALSA20) {
			noncelen = XSALSA20_CRYPTO_NONCEBYTES;
		} else {
			fprintf(stderr, "Invalid Encryption algorithm code: %d. File corrupt ?\n", encrypt_type);
			UNCOMP_BAIL;
		}
		if (Read(compfd, &saltlen, sizeof (saltlen)) < sizeof (saltlen)) {
			perror("Read: ");
			UNCOMP_BAIL;
		}
		saltlen = ntohl(saltlen);
		salt1 = (uchar_t *)malloc(saltlen);
		salt2 = (uchar_t *)malloc(saltlen);
		if (Read(compfd, salt1, saltlen) < saltlen) {
			free(salt1);  free(salt2);
			perror("Read: ");
			UNCOMP_BAIL;
		}
		deserialize_checksum(salt2, salt1, saltlen);

		if (Read(compfd, n1, noncelen) < noncelen) {
			memset(salt2, 0, saltlen);
			free(salt2);
			memset(salt1, 0, saltlen);
			free(salt1);
			perror("Read: ");
			UNCOMP_BAIL;
		}

		if (encrypt_type == CRYPTO_ALG_AES) {
			*((uint64_t *)nonce) = ntohll(*((uint64_t *)n1));

		} else if (encrypt_type == CRYPTO_ALG_SALSA20) {
			deserialize_checksum(nonce, n1, noncelen);
		}

		if (version > 6) {
			if (Read(compfd, &keylen, sizeof (keylen)) < sizeof (keylen)) {
				memset(salt2, 0, saltlen);
				free(salt2);
				memset(salt1, 0, saltlen);
				free(salt1);
				perror("Read: ");
				UNCOMP_BAIL;
			}
			keylen = ntohl(keylen);
		}

		if (Read(compfd, hdr_hash1, mac_bytes) < mac_bytes) {
			memset(salt2, 0, saltlen);
			free(salt2);
			memset(salt1, 0, saltlen);
			free(salt1);
			perror("Read: ");
			UNCOMP_BAIL;
		}
		deserialize_checksum(hdr_hash2, hdr_hash1, mac_bytes);

		if (!pwd_file) {
			pw_len = get_pw_string(pw,
				"Please enter decryption password", 0);
			if (pw_len == -1) {
				memset(salt2, 0, saltlen);
				free(salt2);
				memset(salt1, 0, saltlen);
				free(salt1);
				err_exit(0, "Failed to get password.\n");
			}
		} else {
			int fd, len;
			uchar_t zero[MAX_PW_LEN];

			/*
			 * Read password from a file and zero out the file after reading.
			 */
			memset(zero, 0, MAX_PW_LEN);
			fd = open(pwd_file, O_RDWR);
			if (fd != -1) {
				pw_len = lseek(fd, 0, SEEK_END);
				if (pw_len != -1) {
					if (pw_len > MAX_PW_LEN) pw_len = MAX_PW_LEN-1;
					lseek(fd, 0, SEEK_SET);
					len = Read(fd, pw, pw_len);
					if (len != -1 && len == pw_len) {
						pw[pw_len] = '\0';
						if (isspace(pw[pw_len - 1]))
							pw[pw_len-1] = '\0';
						lseek(fd, 0, SEEK_SET);
						Write(fd, zero, pw_len);
					} else {
						pw_len = -1;
					}
				}
			}
			if (pw_len == -1) {
				perror(" ");
				memset(salt2, 0, saltlen);
				free(salt2);
				memset(salt1, 0, saltlen);
				free(salt1);
				close(uncompfd); unlink(to_filename);
				err_exit(0, "Failed to get password.\n");
			}
			close(fd);
		}

		if (init_crypto(&crypto_ctx, pw, pw_len, encrypt_type, salt2,
		    saltlen, keylen, nonce, DECRYPT_FLAG) == -1) {
			memset(salt2, 0, saltlen);
			free(salt2);
			memset(salt1, 0, saltlen);
			free(salt1);
			memset(pw, 0, MAX_PW_LEN);
			close(uncompfd); unlink(to_filename);
			err_exit(0, "Failed to initialize crypto\n");
		}
		memset(salt2, 0, saltlen);
		free(salt2);
		memset(pw, 0, MAX_PW_LEN);
		memset(nonce, 0, noncelen);

		/*
		 * Verify file header HMAC.
		 */
		if (hmac_init(&hdr_mac, cksum, &crypto_ctx) == -1) {
			close(uncompfd); unlink(to_filename);
			err_exit(0, "Cannot initialize header hmac.\n");
		}
		hmac_update(&hdr_mac, (uchar_t *)algo, ALGO_SZ);
		d1 = htons(version);
		hmac_update(&hdr_mac, (uchar_t *)&d1, sizeof (version));
		d1 = htons(flags);
		hmac_update(&hdr_mac, (uchar_t *)&d1, sizeof (flags));
		d3 = htonll(chunksize);
		hmac_update(&hdr_mac, (uchar_t *)&d3, sizeof (chunksize));
		d2 = htonl(level);
		hmac_update(&hdr_mac, (uchar_t *)&d2, sizeof (level));
		if (version > 6) {
			d2 = htonl(saltlen);
			hmac_update(&hdr_mac, (uchar_t *)&d2, sizeof (saltlen));
			hmac_update(&hdr_mac, salt1, saltlen);
			hmac_update(&hdr_mac, n1, noncelen);
			d2 = htonl(keylen);
			hmac_update(&hdr_mac, (uchar_t *)&d2, sizeof (keylen));
		}
		hmac_final(&hdr_mac, hdr_hash1, &hlen);
		hmac_cleanup(&hdr_mac);
		memset(salt1, 0, saltlen);
		free(salt1);
		memset(n1, 0, noncelen);
		if (memcmp(hdr_hash2, hdr_hash1, mac_bytes) != 0) {
			close(uncompfd); unlink(to_filename);
			err_exit(0, "Header verification failed! File tampered or wrong password.\n");
		}
	} else if (version >= 5) {
		uint32_t crc1, crc2;
		unsigned short d1;
		unsigned int d2;
		uint64_t ch;

		/*
		 * Verify file header CRC32 in non-crypto mode.
		 */
		if (Read(compfd, &crc1, sizeof (crc1)) < sizeof (crc1)) {
			perror("Read: ");
			UNCOMP_BAIL;
		}
		crc1 = htonl(crc1);
		mac_bytes = sizeof (uint32_t);

		crc2 = lzma_crc32((uchar_t *)algo, ALGO_SZ, 0);
		d1 = htons(version);
		crc2 = lzma_crc32((uchar_t *)&d1, sizeof (version), crc2);
		d1 = htons(flags);
		crc2 = lzma_crc32((uchar_t *)&d1, sizeof (version), crc2);
		ch = htonll(chunksize);
		crc2 = lzma_crc32((uchar_t *)&ch, sizeof (ch), crc2);
		d2 = htonl(level);
		crc2 = lzma_crc32((uchar_t *)&d2, sizeof (level), crc2);
		if (crc1 != crc2) {
			close(uncompfd); unlink(to_filename);
			err_exit(0, "Header verification failed! File tampered or wrong password.\n");
		}
	}

	nprocs = sysconf(_SC_NPROCESSORS_ONLN);
	if (nthreads > 0 && nthreads < nprocs)
		nprocs = nthreads;
	else
		nthreads = nprocs;

	set_threadcounts(&props, &nthreads, nprocs, DECOMPRESS_THREADS);
	if (props.is_single_chunk)
		nthreads = 1;
	fprintf(stderr, "Scaling to %d thread", nthreads * props.nthreads);
	if (nthreads * props.nthreads > 1) fprintf(stderr, "s");
	fprintf(stderr, "\n");
	nprocs = nthreads;
	slab_cache_add(compressed_chunksize);
	slab_cache_add(chunksize);
	slab_cache_add(sizeof (struct cmp_data));

	dary = (struct cmp_data **)slab_calloc(NULL, nprocs, sizeof (struct cmp_data *));
	for (i = 0; i < nprocs; i++) {
		dary[i] = (struct cmp_data *)slab_alloc(NULL, sizeof (struct cmp_data));
		if (!dary[i]) {
			fprintf(stderr, "Out of memory\n");
			UNCOMP_BAIL;
		}
		tdat = dary[i];
		tdat->compressed_chunk = NULL;
		tdat->uncompressed_chunk = NULL;
		tdat->chunksize = chunksize;
		tdat->compress = _compress_func;
		tdat->decompress = _decompress_func;
		tdat->cancel = 0;
		tdat->decompressing = 1;
		if (props.is_single_chunk) {
			tdat->cksum_mt = 1;
			if (version == 6) {
				tdat->cksum_mt = 2; // Indicate old format parallel hash
			}
		} else {
			tdat->cksum_mt = 0;
		}
		tdat->level = level;
		tdat->data = NULL;
		tdat->props = &props;
		sem_init(&(tdat->start_sem), 0, 0);
		sem_init(&(tdat->cmp_done_sem), 0, 0);
		sem_init(&(tdat->write_done_sem), 0, 1);
		sem_init(&(tdat->index_sem), 0, 0);

		if (_init_func) {
			if (_init_func(&(tdat->data), &(tdat->level), props.nthreads, chunksize,
			    version, DECOMPRESS) != 0) {
				UNCOMP_BAIL;
			}
		}
		if (enable_rabin_scan || enable_fixed_scan || enable_rabin_global) {
			tdat->rctx = create_dedupe_context(chunksize, compressed_chunksize, rab_blk_size,
			    algo, &props, enable_delta_encode, dedupe_flag, version, DECOMPRESS, 0,
			    NULL);
			if (tdat->rctx == NULL) {
				UNCOMP_BAIL;
			}
			if (enable_rabin_global) {
				if ((tdat->rctx->out_fd = open(to_filename, O_RDONLY, 0)) == -1) {
					perror("Unable to get new read handle to output file");
					UNCOMP_BAIL;
				}
			}
			tdat->rctx->index_sem = &(tdat->index_sem);
		} else {
			tdat->rctx = NULL;
		}

		if (encrypt_type) {
			if (hmac_init(&tdat->chunk_hmac, cksum, &crypto_ctx) == -1) {
				fprintf(stderr, "Cannot initialize chunk hmac.\n");
				UNCOMP_BAIL;
			}
		}
		if (pthread_create(&(tdat->thr), NULL, perform_decompress,
		    (void *)tdat) != 0) {
			perror("Error in thread creation: ");
			UNCOMP_BAIL;
		}
	}
	thread = 1;

	if (enable_rabin_global) {
		for (i = 0; i < nprocs; i++) {
			tdat = dary[i];
			tdat->rctx->index_sem_next = &(dary[(i + 1) % nprocs]->index_sem);
		}
	}
	// When doing global dedupe first thread does not wait to start dedupe recovery.
	sem_post(&(dary[0]->index_sem));

	if (encrypt_type) {
		/* Erase encryption key bytes stored as a plain array. No longer reqd. */
		crypto_clean_pkey(&crypto_ctx);
	}

	w.dary = dary;
	w.wfd = uncompfd;
	w.nprocs = nprocs;
	w.chunksize = chunksize;
	if (pthread_create(&writer_thr, NULL, writer_thread, (void *)(&w)) != 0) {
		perror("Error in thread creation: ");
		UNCOMP_BAIL;
	}

	/*
	 * Now read from the compressed file in variable compressed chunk size.
	 * First the size is read from the chunk header and then as many bytes +
	 * checksum size are read and passed to decompression thread.
	 * Chunk sequencing is ensured.
	 */
	chunk_num = 0;
	np = 0;
	bail = 0;
	while (!bail) {
		int64_t rb;

		if (main_cancel) break;
		for (p = 0; p < nprocs; p++) {
			np = p;
			tdat = dary[p];
			sem_wait(&tdat->write_done_sem);
			if (main_cancel) break;
			tdat->id = chunk_num;
			if (tdat->rctx) tdat->rctx->id = tdat->id;

			/*
			 * First read length of compressed chunk.
			 */
			rb = Read(compfd, &tdat->len_cmp, sizeof (tdat->len_cmp));
			if (rb != sizeof (tdat->len_cmp)) {
				if (rb < 0) perror("Read: ");
				else
					fprintf(stderr, "Incomplete chunk %d header,"
					    "file corrupt\n", chunk_num);
				UNCOMP_BAIL;
			}
			tdat->len_cmp_be = tdat->len_cmp; // Needed for HMAC
			tdat->len_cmp = htonll(tdat->len_cmp);

			/*
			 * Check for ridiculous length.
			 */
			if (tdat->len_cmp > chunksize + 256) {
				fprintf(stderr, "Compressed length too big for chunk: %d\n",
				    chunk_num);
				UNCOMP_BAIL;
			}

			/*
			 * Zero compressed len means end of file.
			 */
			if (tdat->len_cmp == 0) {
				bail = 1;
				break;
			}

			/*
			 * Delayed allocation. Allocate chunks if not already done. The compressed
			 * file format does not provide any info on how many chunks are there in
			 * order to allow pipe mode operation. So delayed allocation during
			 * decompression allows to avoid allocating per-thread chunks which will
			 * never be used. This can happen if chunk count < thread count.
			 */
			if (!tdat->compressed_chunk) {
				tdat->compressed_chunk = (uchar_t *)slab_alloc(NULL,
				    compressed_chunksize);
				if ((enable_rabin_scan || enable_fixed_scan))
					tdat->uncompressed_chunk = (uchar_t *)slab_alloc(NULL,
					    compressed_chunksize);
				else
					tdat->uncompressed_chunk = (uchar_t *)slab_alloc(NULL,
					    chunksize);
				if (!tdat->compressed_chunk || !tdat->uncompressed_chunk) {
					fprintf(stderr, "Out of memory\n");
					UNCOMP_BAIL;
				}
				tdat->cmp_seg = tdat->uncompressed_chunk;
			}

			if (tdat->len_cmp > largest_chunk)
				largest_chunk = tdat->len_cmp;
			if (tdat->len_cmp < smallest_chunk)
				smallest_chunk = tdat->len_cmp;
			avg_chunk += tdat->len_cmp;

			/*
			 * Now read compressed chunk including the checksum.
			 */
			tdat->rbytes = Read(compfd, tdat->compressed_chunk,
			    tdat->len_cmp + cksum_bytes + mac_bytes + CHUNK_FLAG_SZ);
			if (main_cancel) break;
			if (tdat->rbytes < tdat->len_cmp + cksum_bytes + mac_bytes + CHUNK_FLAG_SZ) {
				if (tdat->rbytes < 0) {
					perror("Read: ");
					UNCOMP_BAIL;
				} else {
					fprintf(stderr, "Incomplete chunk %d, file corrupt.\n",
					    chunk_num);
					UNCOMP_BAIL;
				}
			}
			sem_post(&tdat->start_sem);
			++chunk_num;
		}
	}


	if (!main_cancel) {
		for (p = 0; p < nprocs; p++) {
			if (p == np) continue;
			tdat = dary[p];
			sem_wait(&tdat->write_done_sem);
		}
	}
uncomp_done:
	if (t_errored) err = t_errored;
	if (thread) {
		for (i = 0; i < nprocs; i++) {
			tdat = dary[i];
			tdat->cancel = 1;
			tdat->len_cmp = 0;
			sem_post(&tdat->start_sem);
			sem_post(&tdat->cmp_done_sem);
			pthread_join(tdat->thr, NULL);
		}
		pthread_join(writer_thr, NULL);
	}

	/*
	 * Ownership and mode of target should be same as original.
	 */
	fchmod(uncompfd, sbuf.st_mode);
	if (fchown(uncompfd, sbuf.st_uid, sbuf.st_gid) == -1)
		perror("Chown ");
	if (dary != NULL) {
		for (i = 0; i < nprocs; i++) {
			if (!dary[i]) continue;
			if (dary[i]->uncompressed_chunk)
				slab_free(NULL, dary[i]->uncompressed_chunk);
			if (dary[i]->compressed_chunk)
				slab_free(NULL, dary[i]->compressed_chunk);
			if (_deinit_func)
				_deinit_func(&(dary[i]->data));
			if ((enable_rabin_scan || enable_fixed_scan)) {
				destroy_dedupe_context(dary[i]->rctx);
			}
			slab_free(NULL, dary[i]);
		}
		slab_free(NULL, dary);
	}
	if (!pipe_mode) {
		if (compfd != -1) close(compfd);
		if (uncompfd != -1) close(uncompfd);
	}

	if (!hide_cmp_stats) show_compression_stats(chunksize);
	slab_cleanup(hide_mem_stats);

	return (err);
}

static void *
perform_compress(void *dat) {
	struct cmp_data *tdat = (struct cmp_data *)dat;
	typeof (tdat->chunksize) _chunksize, len_cmp, dedupe_index_sz, index_size_cmp;
	int type, rv;
	uchar_t *compressed_chunk;
	int64_t rbytes;

redo:
	sem_wait(&tdat->start_sem);
	if (unlikely(tdat->cancel)) {
		tdat->len_cmp = 0;
		sem_post(&tdat->cmp_done_sem);
		return (0);
	}

	compressed_chunk = tdat->compressed_chunk + CHUNK_FLAG_SZ;
	rbytes = tdat->rbytes;
	dedupe_index_sz = 0;

	/* Perform Dedup if enabled. */
	if ((enable_rabin_scan || enable_fixed_scan)) {
		dedupe_context_t *rctx;

		/*
		 * Compute checksum of original uncompressed chunk. When doing dedup
		 * cmp_seg hold original data instead of uncompressed_chunk. We dedup
		 * into uncompressed_chunk so that compress transforms uncompressed_chunk
		 * back into cmp_seg. Avoids an extra memcpy().
		 */
		if (!encrypt_type)
			compute_checksum(tdat->checksum, cksum, tdat->cmp_seg, tdat->rbytes,
					 tdat->cksum_mt, 1);

		rctx = tdat->rctx;
		reset_dedupe_context(tdat->rctx);
		rctx->cbuf = tdat->uncompressed_chunk;
		dedupe_index_sz = dedupe_compress(tdat->rctx, tdat->cmp_seg, &(tdat->rbytes), 0,
						  NULL, tdat->cksum_mt);
		if (!rctx->valid) {
			memcpy(tdat->uncompressed_chunk, tdat->cmp_seg, rbytes);
			tdat->rbytes = rbytes;
		}
	} else {
		/*
		 * Compute checksum of original uncompressed chunk.
		 */
		if (!encrypt_type)
			compute_checksum(tdat->checksum, cksum, tdat->uncompressed_chunk,
					 tdat->rbytes, tdat->cksum_mt, 1);
	}

	/*
	 * If doing dedup we compress rabin index and deduped data separately.
	 * The rabin index array values can pollute the compressor's dictionary thereby
	 * reducing compression effectiveness of the data chunk. So we separate them.
	 */
	if ((enable_rabin_scan || enable_fixed_scan) && tdat->rctx->valid) {
		_chunksize = tdat->rbytes - dedupe_index_sz - RABIN_HDR_SIZE;
		index_size_cmp = dedupe_index_sz;

		rv = 0;

		/*
		 * Do a matrix transpose of the index table with the hope of improving
		 * compression ratio subsequently.
		 */
		transpose(tdat->uncompressed_chunk + RABIN_HDR_SIZE,
		    compressed_chunk + RABIN_HDR_SIZE, dedupe_index_sz,
		    sizeof (uint32_t), ROW);
		memcpy(tdat->uncompressed_chunk + RABIN_HDR_SIZE,
		    compressed_chunk + RABIN_HDR_SIZE, dedupe_index_sz);

		if (dedupe_index_sz >= 90) {
			/* Compress index if it is at least 90 bytes. */
			rv = lzma_compress(tdat->uncompressed_chunk + RABIN_HDR_SIZE,
			    dedupe_index_sz, compressed_chunk + RABIN_HDR_SIZE,
			    &index_size_cmp, tdat->rctx->level, 255, tdat->rctx->lzma_data);

			/* 
			 * If index compression fails or does not produce a smaller result
			 * retain it as is. In that case compressed size == original size
			 * and it will be handled correctly during decompression.
			 */
			if (rv != 0 || index_size_cmp >= dedupe_index_sz) {
				index_size_cmp = dedupe_index_sz;
				goto plain_index;
			}
		} else {
plain_index:
			memcpy(compressed_chunk + RABIN_HDR_SIZE,
			    tdat->uncompressed_chunk + RABIN_HDR_SIZE, dedupe_index_sz);
		}

		index_size_cmp += RABIN_HDR_SIZE;
		dedupe_index_sz += RABIN_HDR_SIZE;
		memcpy(compressed_chunk, tdat->uncompressed_chunk, RABIN_HDR_SIZE);
		/* Compress data chunk. */
		if (lzp_preprocess || enable_delta2_encode) {
			rv = preproc_compress(tdat->compress, tdat->uncompressed_chunk + dedupe_index_sz,
			    _chunksize, compressed_chunk + index_size_cmp, &_chunksize,
			    tdat->level, 0, tdat->data, tdat->props);
		} else {
			DEBUG_STAT_EN(double strt, en);

			DEBUG_STAT_EN(strt = get_wtime_millis());
			rv = tdat->compress(tdat->uncompressed_chunk + dedupe_index_sz, _chunksize,
			    compressed_chunk + index_size_cmp, &_chunksize, tdat->level, 0, tdat->data);
			DEBUG_STAT_EN(en = get_wtime_millis());
			DEBUG_STAT_EN(fprintf(stderr, "Chunk compression speed %.3f MB/s\n",
					      get_mb_s(_chunksize, strt, en)));
		}

		/* Can't compress data just retain as-is. */
		if (rv < 0)
			memcpy(compressed_chunk + index_size_cmp,
			    tdat->uncompressed_chunk + dedupe_index_sz, _chunksize);
		/* Now update rabin header with the compressed sizes. */
		update_dedupe_hdr(compressed_chunk, index_size_cmp - RABIN_HDR_SIZE, _chunksize);
		_chunksize += index_size_cmp;
	} else {
		_chunksize = tdat->rbytes;
		if (lzp_preprocess || enable_delta2_encode) {
			rv = preproc_compress(tdat->compress,
			    tdat->uncompressed_chunk, tdat->rbytes,
			    compressed_chunk, &_chunksize, tdat->level, 0, tdat->data,
			    tdat->props);
		} else {
			DEBUG_STAT_EN(double strt, en);

			DEBUG_STAT_EN(strt = get_wtime_millis());
			rv = tdat->compress(tdat->uncompressed_chunk, tdat->rbytes,
			    compressed_chunk, &_chunksize, tdat->level, 0, tdat->data);
			DEBUG_STAT_EN(en = get_wtime_millis());
			DEBUG_STAT_EN(fprintf(stderr, "Chunk compression speed %.3f MB/s\n",
					      get_mb_s(_chunksize, strt, en)));
		}
	}

	/*
	 * Sanity check to ensure compressed data is lesser than original.
	 * If at all compression expands/does not shrink data then the chunk
	 * will be left uncompressed. Also if the compression errored the
	 * chunk will be left uncompressed.
	 */
	tdat->len_cmp = _chunksize;
	if (_chunksize >= rbytes || rv < 0) {
		if (!(enable_rabin_scan || enable_fixed_scan) || !tdat->rctx->valid)
			memcpy(compressed_chunk, tdat->uncompressed_chunk, tdat->rbytes);
		type = UNCOMPRESSED;
		tdat->len_cmp = tdat->rbytes;
		if (rv < 0) rv = COMPRESS_NONE;
	} else {
		type = COMPRESSED;
	}

	/*
	 * Now perform encryption on the compressed data, if requested.
	 */
	if (encrypt_type) {
		int ret;
		DEBUG_STAT_EN(double strt, en);

		/*
		 * Encryption algorithm must not change the size and
		 * encryption is in-place.
		 */
		DEBUG_STAT_EN(strt = get_wtime_millis());
		ret = crypto_buf(&crypto_ctx, compressed_chunk, compressed_chunk,
			tdat->len_cmp, tdat->id);
		if (ret == -1) {
			/*
			 * Encryption failure is fatal.
			 */
			main_cancel = 1;
			tdat->len_cmp = 0;
			t_errored = 1;
			sem_post(&tdat->cmp_done_sem);
			return (0);
		}
		DEBUG_STAT_EN(en = get_wtime_millis());
		DEBUG_STAT_EN(fprintf(stderr, "Encryption speed %.3f MB/s\n",
			      get_mb_s(tdat->len_cmp, strt, en)));
	}

	if ((enable_rabin_scan || enable_fixed_scan) && tdat->rctx->valid) {
		type |= CHUNK_FLAG_DEDUP;
	}
	if (lzp_preprocess || enable_delta2_encode) {
		type |= CHUNK_FLAG_PREPROC;
	}

	/*
	 * Insert compressed chunk length and checksum into chunk header.
	 */
	len_cmp = tdat->len_cmp;
	*((typeof (len_cmp) *)(tdat->cmp_seg)) = htonll(tdat->len_cmp);
	if (!encrypt_type)
		serialize_checksum(tdat->checksum, tdat->cmp_seg + sizeof (tdat->len_cmp), cksum_bytes);
	tdat->len_cmp += CHUNK_FLAG_SZ;
	tdat->len_cmp += sizeof (len_cmp);
	tdat->len_cmp += (cksum_bytes + mac_bytes);
	rbytes = tdat->len_cmp - len_cmp; // HDR size for HMAC

	if (adapt_mode)
		type |= (rv << 4);

	/*
	 * If chunk is less than max chunksize, store this length as well.
	 */
	if (tdat->rbytes < tdat->chunksize) {
		type |= CHSIZE_MASK;
		*((typeof (tdat->rbytes) *)(tdat->cmp_seg + tdat->len_cmp)) = htonll(tdat->rbytes);
		tdat->len_cmp += ORIGINAL_CHUNKSZ;
		len_cmp += ORIGINAL_CHUNKSZ;
		*((typeof (len_cmp) *)(tdat->cmp_seg)) = htonll(len_cmp);
	}
	/*
	 * Set the chunk header flags.
	 */
	*(tdat->compressed_chunk) = type;

	/*
	 * If encrypting, compute HMAC for full chunk including header.
	 */
	if (encrypt_type) {
		uchar_t *mac_ptr;
		unsigned int hlen;
		uchar_t chash[mac_bytes];
		DEBUG_STAT_EN(double strt, en);

		/* Clean out mac_bytes to 0 for stable HMAC. */
		DEBUG_STAT_EN(strt = get_wtime_millis());
		mac_ptr = tdat->cmp_seg + sizeof (tdat->len_cmp) + cksum_bytes;
		memset(mac_ptr, 0, mac_bytes);
		hmac_reinit(&tdat->chunk_hmac);
		hmac_update(&tdat->chunk_hmac, tdat->cmp_seg, tdat->len_cmp);
		hmac_final(&tdat->chunk_hmac, chash, &hlen);
		serialize_checksum(chash, mac_ptr, hlen);
		DEBUG_STAT_EN(en = get_wtime_millis());
		DEBUG_STAT_EN(fprintf(stderr, "HMAC Computation speed %.3f MB/s\n",
			      get_mb_s(tdat->len_cmp, strt, en)));
	} else {
		/*
		 * Compute header CRC32 in non-crypto mode.
		 */
		uchar_t *mac_ptr;
		uint32_t crc;

		/* Clean out mac_bytes to 0 for stable CRC32. */
		mac_ptr = tdat->cmp_seg + sizeof (tdat->len_cmp) + cksum_bytes;
		memset(mac_ptr, 0, mac_bytes);
		crc = lzma_crc32(tdat->cmp_seg, rbytes, 0);
		if (type & CHSIZE_MASK)
			crc = lzma_crc32(tdat->cmp_seg + tdat->len_cmp - ORIGINAL_CHUNKSZ,
			    ORIGINAL_CHUNKSZ, crc);
		*((uint32_t *)mac_ptr) = htonl(crc);
	}
	
	sem_post(&tdat->cmp_done_sem);
	goto redo;
}

static void *
writer_thread(void *dat) {
	int p;
	struct wdata *w = (struct wdata *)dat;
	struct cmp_data *tdat;
	int64_t wbytes;

repeat:
	for (p = 0; p < w->nprocs; p++) {
		tdat = w->dary[p];
		sem_wait(&tdat->cmp_done_sem);
		if (tdat->len_cmp == 0) {
			goto do_cancel;
		}

		if (do_compress) {
			if (tdat->len_cmp > largest_chunk)
				largest_chunk = tdat->len_cmp;
			if (tdat->len_cmp < smallest_chunk)
				smallest_chunk = tdat->len_cmp;
			avg_chunk += tdat->len_cmp;
		}

		wbytes = Write(w->wfd, tdat->cmp_seg, tdat->len_cmp);
		if (unlikely(wbytes != tdat->len_cmp)) {
			perror("Chunk Write: ");
do_cancel:
			main_cancel = 1;
			tdat->cancel = 1;
			sem_post(&tdat->start_sem);
			if (tdat->rctx && enable_rabin_global)
				sem_post(tdat->rctx->index_sem_next);
			sem_post(&tdat->write_done_sem);
			return (0);
		}
		if (tdat->decompressing && tdat->rctx && enable_rabin_global) {
			sem_post(tdat->rctx->index_sem_next);
		}
		sem_post(&tdat->write_done_sem);
	}
	goto repeat;
}

/*
 * File compression routine. Can use as many threads as there are
 * logical cores unless user specified something different. There is
 * not much to gain from nthreads > n logical cores however.
 */
#define COMP_BAIL err = 1; goto comp_done

static int
start_compress(const char *filename, uint64_t chunksize, int level)
{
	struct wdata w;
	char tmpfile1[MAXPATHLEN], tmpdir[MAXPATHLEN];
	char to_filename[MAXPATHLEN];
	uint64_t compressed_chunksize, n_chunksize, file_offset;
	int64_t rbytes, rabin_count;
	short version, flags;
	struct stat sbuf;
	int compfd = -1, uncompfd = -1, err;
	int thread, bail, single_chunk;
	uint32_t i, nprocs, np, p, dedupe_flag;
	struct cmp_data **dary = NULL, *tdat;
	pthread_t writer_thr;
	uchar_t *cread_buf, *pos;
	dedupe_context_t *rctx;
	algo_props_t props;

	/*
	 * Compressed buffer size must include zlib/dedup scratch space and
	 * chunk header space.
	 * See http://www.zlib.net/manual.html#compress2
	 * 
	 * We do this unconditionally whether user mentioned zlib or not
	 * to keep it simple. While zlib scratch space is only needed at
	 * runtime, chunk header is stored in the file.
	 *
	 * See start_decompress() routine for details of chunk header.
	 * We also keep extra 8-byte space for the last chunk's size.
	 */
	compressed_chunksize = chunksize + CHUNK_HDR_SZ + zlib_buf_extra(chunksize);
	init_algo_props(&props);
	props.cksum = cksum;
	cread_buf = NULL;

	if (_props_func) {
		_props_func(&props, level, chunksize);
		if (chunksize + props.buf_extra > compressed_chunksize) {
			compressed_chunksize += (chunksize + props.buf_extra - 
			    compressed_chunksize);
		}
	}

	flags = 0;
	dedupe_flag = RABIN_DEDUPE_SEGMENTED; // Silence the compiler
	if (enable_rabin_scan || enable_fixed_scan || enable_rabin_global) {
		if (enable_rabin_global) {
			flags |= (FLAG_DEDUP | FLAG_DEDUP_FIXED);
			dedupe_flag = RABIN_DEDUPE_FILE_GLOBAL;
			if (pipe_mode) {
				return (1);
			}
		} else if (enable_rabin_scan) {
			flags |= FLAG_DEDUP;
			dedupe_flag = RABIN_DEDUPE_SEGMENTED;
		} else {
			flags |= FLAG_DEDUP_FIXED;
			dedupe_flag = RABIN_DEDUPE_FIXED;
		}
		/* Additional scratch space for dedup arrays. */
		if (chunksize + dedupe_buf_extra(chunksize, 0, algo, enable_delta_encode)
		    > compressed_chunksize) {
			compressed_chunksize += (chunksize +
			    dedupe_buf_extra(chunksize, 0, algo, enable_delta_encode)) -
			    compressed_chunksize;
		}
	}

	if (encrypt_type) {
		uchar_t pw[MAX_PW_LEN];
		int pw_len = -1;

		compressed_chunksize += mac_bytes;
		if (!pwd_file) {
			pw_len = get_pw_string(pw,
				"Please enter encryption password", 1);
			if (pw_len == -1) {
				err_exit(0, "Failed to get password.\n");
			}
		} else {
			int fd, len;
			uchar_t zero[MAX_PW_LEN];

			/*
			 * Read password from a file and zero out the file after reading.
			 */
			memset(zero, 0, MAX_PW_LEN);
			fd = open(pwd_file, O_RDWR);
			if (fd != -1) {
				pw_len = lseek(fd, 0, SEEK_END);
				if (pw_len != -1) {
					if (pw_len > MAX_PW_LEN) pw_len = MAX_PW_LEN-1;
					lseek(fd, 0, SEEK_SET);
					len = Read(fd, pw, pw_len);
					if (len != -1 && len == pw_len) {
						pw[pw_len] = '\0';
						if (isspace(pw[pw_len - 1]))
							pw[pw_len-1] = '\0';
						lseek(fd, 0, SEEK_SET);
						Write(fd, zero, pw_len);
					} else {
						pw_len = -1;
					}
				}
			}
			if (pw_len == -1) {
				err_exit(1, "Failed to get password.\n");
			}
			close(fd);
		}
		if (init_crypto(&crypto_ctx, pw, pw_len, encrypt_type, NULL,
		    0, keylen, 0, ENCRYPT_FLAG) == -1) {
			memset(pw, 0, MAX_PW_LEN);
			err_exit(0, "Failed to initialize crypto\n");
		}
		memset(pw, 0, MAX_PW_LEN);
	}

	err = 0;
	thread = 0;
	single_chunk = 0;
	rctx = NULL;
	slab_cache_add(chunksize);
	slab_cache_add(compressed_chunksize);
	slab_cache_add(sizeof (struct cmp_data));

	nprocs = sysconf(_SC_NPROCESSORS_ONLN);
	if (nthreads > 0 && nthreads < nprocs)
		nprocs = nthreads;
	else
		nthreads = nprocs;

	/* A host of sanity checks. */
	if (!pipe_mode) {
		if ((uncompfd = open(filename, O_RDWR, 0)) == -1)
			err_exit(1, "Cannot open: %s", filename);

		if (fstat(uncompfd, &sbuf) == -1) {
			close(uncompfd);
			err_exit(1, "Cannot stat: %s", filename);
		}

		if (!S_ISREG(sbuf.st_mode)) {
			close(uncompfd);
			err_exit(0, "File %s is not a regular file.\n", filename);
		}

		if (sbuf.st_size == 0) {
			close(uncompfd);
			return (1);
		}

		/*
		 * Adjust chunk size for small files. We then get an archive with
		 * a single chunk for the entire file.
		 */
		if (sbuf.st_size <= chunksize) {
			chunksize = sbuf.st_size;
			enable_rabin_split = 0; // Do not split for whole files.
			nthreads = 1;
			single_chunk = 1;
			props.is_single_chunk = 1;
			flags |= FLAG_SINGLE_CHUNK;
		} else {
			if (nthreads == 0 || nthreads > sbuf.st_size / chunksize) {
				nthreads = sbuf.st_size / chunksize;
				if (sbuf.st_size % chunksize)
					nthreads++;
			}
		}

		/*
		 * Create a temporary file to hold compressed data which is renamed at
		 * the end. The target file name is same as original file with the '.pz'
		 * extension appended.
		 */
		strcpy(tmpfile1, filename);
		strcpy(tmpfile1, dirname(tmpfile1));
		strcpy(tmpdir, tmpfile1);
		strcat(tmpfile1, "/.pcompXXXXXX");
		snprintf(to_filename, sizeof (to_filename), "%s" COMP_EXTN, filename);
		if ((compfd = mkstemp(tmpfile1)) == -1) {
			perror("mkstemp ");
			COMP_BAIL;
		}
		f_name = tmpfile1;
		signal(SIGINT, Int_Handler);
		signal(SIGTERM, Int_Handler);
	} else {
		char *tmp;
		struct stat st;

		/*
		 * Use stdin/stdout for pipe mode.
		 */
		compfd = fileno(stdout);
		if (compfd == -1) {
			perror("fileno ");
			COMP_BAIL;
		}
		uncompfd = fileno(stdin);
		if (uncompfd == -1) {
			perror("fileno ");
			COMP_BAIL;
		}

		/*
		 * Get a workable temporary dir. Required if global dedupe is enabled.
		 */
		tmp = getenv("TMPDIR");
		if (tmp == NULL) {
			tmp = getenv("HOME");
			if (tmp == NULL) {
				if (getcwd(tmpdir, MAXPATHLEN) == NULL) {
					tmp = "/tmp";
				} else {
					tmp = tmpdir;
				}
			}
		}
		if (stat(tmp, &st) == -1) {
			fprintf(stderr, "Unable to find writable temporary dir.\n");
			COMP_BAIL;
		}
		if (!S_ISDIR(st.st_mode)) {
			if (strcmp(tmp, "/tmp") != 0) {
			tmp = "/tmp";
			} else {
				fprintf(stderr, "Unable to find writable temporary dir.\n");
				COMP_BAIL;
			}
		}
		strcpy(tmpdir, tmp);
	}

	if (encrypt_type)
		flags |= encrypt_type;

	set_threadcounts(&props, &nthreads, nprocs, COMPRESS_THREADS);
	fprintf(stderr, "Scaling to %d thread", nthreads * props.nthreads);
	if (nthreads * props.nthreads > 1) fprintf(stderr, "s");
	nprocs = nthreads;
	fprintf(stderr, "\n");

	dary = (struct cmp_data **)slab_calloc(NULL, nprocs, sizeof (struct cmp_data *));
	if ((enable_rabin_scan || enable_fixed_scan))
		cread_buf = (uchar_t *)slab_alloc(NULL, compressed_chunksize);
	else
		cread_buf = (uchar_t *)slab_alloc(NULL, chunksize);
	if (!cread_buf) {
		fprintf(stderr, "Out of memory\n");
		COMP_BAIL;
	}

	for (i = 0; i < nprocs; i++) {
		dary[i] = (struct cmp_data *)slab_alloc(NULL, sizeof (struct cmp_data));
		if (!dary[i]) {
			fprintf(stderr, "Out of memory\n");
			COMP_BAIL;
		}
		tdat = dary[i];
		tdat->cmp_seg = NULL;
		tdat->chunksize = chunksize;
		tdat->compress = _compress_func;
		tdat->decompress = _decompress_func;
		tdat->uncompressed_chunk = (uchar_t *)1;
		tdat->cancel = 0;
		tdat->decompressing = 0;
		if (single_chunk)
			tdat->cksum_mt = 1;
		else
			tdat->cksum_mt = 0;
		tdat->level = level;
		tdat->data = NULL;
		tdat->props = &props;
		sem_init(&(tdat->start_sem), 0, 0);
		sem_init(&(tdat->cmp_done_sem), 0, 0);
		sem_init(&(tdat->write_done_sem), 0, 1);
		sem_init(&(tdat->index_sem), 0, 0);

		if (_init_func) {
			if (_init_func(&(tdat->data), &(tdat->level), props.nthreads, chunksize,
			    VERSION, COMPRESS) != 0) {
				COMP_BAIL;
			}
		}
		if (enable_rabin_scan || enable_fixed_scan || enable_rabin_global) {
			tdat->rctx = create_dedupe_context(chunksize, compressed_chunksize, rab_blk_size,
			    algo, &props, enable_delta_encode, dedupe_flag, VERSION, COMPRESS, sbuf.st_size,
			    tmpdir);
			if (tdat->rctx == NULL) {
				COMP_BAIL;
			}

			tdat->rctx->index_sem = &(tdat->index_sem);
		} else {
			tdat->rctx = NULL;
		}

		if (encrypt_type) {
			if (hmac_init(&tdat->chunk_hmac, cksum, &crypto_ctx) == -1) {
				fprintf(stderr, "Cannot initialize chunk hmac.\n");
				COMP_BAIL;
			}
		}
		if (pthread_create(&(tdat->thr), NULL, perform_compress,
		    (void *)tdat) != 0) {
			perror("Error in thread creation: ");
			COMP_BAIL;
		}
	}
	thread = 1;

	if (enable_rabin_global) {
		for (i = 0; i < nprocs; i++) {
			tdat = dary[i];
			tdat->rctx->index_sem_next = &(dary[(i + 1) % nprocs]->index_sem);
		}
	}
	// When doing global dedupe first thread does not wait to access the index.
	sem_post(&(dary[0]->index_sem));

	w.dary = dary;
	w.wfd = compfd;
	w.nprocs = nprocs;
	if (pthread_create(&writer_thr, NULL, writer_thread, (void *)(&w)) != 0) {
		perror("Error in thread creation: ");
		COMP_BAIL;
	}

	/*
	 * Write out file header. First insert hdr elements into mem buffer
	 * then write out the full hdr in one shot.
	 */
	flags |= cksum;
	memset(cread_buf, 0, ALGO_SZ);
	strncpy((char *)cread_buf, algo, ALGO_SZ);
	version = htons(VERSION);
	flags = htons(flags);
	n_chunksize = htonll(chunksize);
	level = htonl(level);
	pos = cread_buf + ALGO_SZ;
	memcpy(pos, &version, sizeof (version));
	pos += sizeof (version);
	memcpy(pos, &flags, sizeof (flags));
	pos += sizeof (flags);
	memcpy(pos, &n_chunksize, sizeof (n_chunksize));
	pos += sizeof (n_chunksize);
	memcpy(pos, &level, sizeof (level));
	pos += sizeof (level);

	/*
	 * If encryption is enabled, include salt, nonce and keylen in the header
	 * to be HMAC-ed (archive version 7 and greater).
	 */
	if (encrypt_type) {
		*((int *)pos) = htonl(crypto_ctx.saltlen);
		pos += sizeof (int);
		serialize_checksum(crypto_ctx.salt, pos, crypto_ctx.saltlen);
		pos += crypto_ctx.saltlen;
		if (encrypt_type == CRYPTO_ALG_AES) {
			*((uint64_t *)pos) = htonll(*((uint64_t *)crypto_nonce(&crypto_ctx)));
			pos += 8;

		} else if (encrypt_type == CRYPTO_ALG_SALSA20) {
			serialize_checksum(crypto_nonce(&crypto_ctx), pos, XSALSA20_CRYPTO_NONCEBYTES);
			pos += XSALSA20_CRYPTO_NONCEBYTES;
		}
		*((int *)pos) = htonl(keylen);
		pos += sizeof (int);
	}
	if (Write(compfd, cread_buf, pos - cread_buf) != pos - cread_buf) {
		perror("Write ");
		COMP_BAIL;
	}

	/*
	 * If encryption is enabled, compute header HMAC and write it.
	 */
	if (encrypt_type) {
		mac_ctx_t hdr_mac;
		uchar_t hdr_hash[mac_bytes];
		unsigned int hlen;

		if (hmac_init(&hdr_mac, cksum, &crypto_ctx) == -1) {
			fprintf(stderr, "Cannot initialize header hmac.\n");
			COMP_BAIL;
		}
		hmac_update(&hdr_mac, cread_buf, pos - cread_buf);
		hmac_final(&hdr_mac, hdr_hash, &hlen);
		hmac_cleanup(&hdr_mac);

		/* Erase encryption key bytes stored as a plain array. No longer reqd. */
		crypto_clean_pkey(&crypto_ctx);

		pos = cread_buf;
		serialize_checksum(hdr_hash, pos, hlen);
		pos += hlen;
		if (Write(compfd, cread_buf, pos - cread_buf) != pos - cread_buf) {
			perror("Write ");
			COMP_BAIL;
		}
	} else {
		/*
		 * Compute header CRC32 and store that. Only archive version 5 and above.
		 */
		uint32_t crc = lzma_crc32(cread_buf, pos - cread_buf, 0);
		*((uint32_t *)cread_buf) = htonl(crc);
		if (Write(compfd, cread_buf, sizeof (uint32_t)) != sizeof (uint32_t)) {
			perror("Write ");
			COMP_BAIL;
		}
	}

	/*
	 * Now read from the uncompressed file in 'chunksize' sized chunks, independently
	 * compress each chunk and write it out. Chunk sequencing is ensured.
	 */
	chunk_num = 0;
	np = 0;
	bail = 0;
	largest_chunk = 0;
	smallest_chunk = chunksize;
	avg_chunk = 0;
	rabin_count = 0;

	/*
	 * Read the first chunk into a spare buffer (a simple double-buffering).
	 */
	file_offset = 0;
	if (enable_rabin_split) {
		rctx = create_dedupe_context(chunksize, 0, 0, algo, &props, enable_delta_encode,
		    enable_fixed_scan, VERSION, COMPRESS, 0, NULL);
		rbytes = Read_Adjusted(uncompfd, cread_buf, chunksize, &rabin_count, rctx);
	} else {
		rbytes = Read(uncompfd, cread_buf, chunksize);
	}

	while (!bail) {
		uchar_t *tmp;

		if (main_cancel) break;
		for (p = 0; p < nprocs; p++) {
			np = p;
			tdat = dary[p];
			if (main_cancel) break;
			/* Wait for previous chunk compression to complete. */
			sem_wait(&tdat->write_done_sem);
			if (main_cancel) break;

			if (rbytes == 0) { /* EOF */
				bail = 1;
				break;
			}
			/*
			 * Delayed allocation. Allocate chunks if not already done.
			 */
			if (!tdat->cmp_seg) {
				if ((enable_rabin_scan || enable_fixed_scan)) {
					if (single_chunk)
						tdat->cmp_seg = (uchar_t *)1;
					else
						tdat->cmp_seg = (uchar_t *)slab_alloc(NULL,
							compressed_chunksize);
					tdat->uncompressed_chunk = (uchar_t *)slab_alloc(NULL,
						compressed_chunksize);
				} else {
					if (single_chunk)
						tdat->uncompressed_chunk = (uchar_t *)1;
					else
						tdat->uncompressed_chunk =
						    (uchar_t *)slab_alloc(NULL, chunksize);
					tdat->cmp_seg = (uchar_t *)slab_alloc(NULL,
						compressed_chunksize);
				}
				tdat->compressed_chunk = tdat->cmp_seg + COMPRESSED_CHUNKSZ +
				    cksum_bytes + mac_bytes;
				if (!tdat->cmp_seg || !tdat->uncompressed_chunk) {
					fprintf(stderr, "Out of memory\n");
					COMP_BAIL;
				}
			}

			/*
			 * Once previous chunk is done swap already read buffer and
			 * it's size into the thread data.
			 * Normally it goes into uncompressed_chunk, because that's what it is.
			 * With dedup enabled however, we do some jugglery to save additional
			 * memory usage and avoid a memcpy, so it goes into the compressed_chunk
			 * area:
			 * cmp_seg -> dedup -> uncompressed_chunk -> compression -> cmp_seg
			 */
			tdat->id = chunk_num;
			tdat->rbytes = rbytes;
			if ((enable_rabin_scan || enable_fixed_scan || enable_rabin_global)) {
				tmp = tdat->cmp_seg;
				tdat->cmp_seg = cread_buf;
				cread_buf = tmp;
				tdat->compressed_chunk = tdat->cmp_seg + COMPRESSED_CHUNKSZ +
				    cksum_bytes + mac_bytes;
				if (tdat->rctx) tdat->rctx->file_offset = file_offset;

				/*
				 * If there is data after the last rabin boundary in the chunk, then
				 * rabin_count will be non-zero. We carry over the data to the beginning
				 * of the next chunk.
				 */
				if (rabin_count) {
					memcpy(cread_buf,
					    tdat->cmp_seg + rabin_count, rbytes - rabin_count);
					tdat->rbytes = rabin_count;
					rabin_count = rbytes - rabin_count;
				}
			} else {
				tmp = tdat->uncompressed_chunk;
				tdat->uncompressed_chunk = cread_buf;
				cread_buf = tmp;
			}
			file_offset += tdat->rbytes;

			if (rbytes < chunksize) {
				if (rbytes < 0) {
					bail = 1;
					perror("Read: ");
					COMP_BAIL;
				}
			}
			/* Signal the compression thread to start */
			sem_post(&tdat->start_sem);
			++chunk_num;

			if (single_chunk) {
				rbytes = 0;
				continue;
			}

			/*
			 * Read the next buffer we want to process while previous
			 * buffer is in progress.
			 */
			if (enable_rabin_split) {
				rbytes = Read_Adjusted(uncompfd, cread_buf, chunksize, &rabin_count, rctx);
			} else {
				rbytes = Read(uncompfd, cread_buf, chunksize);
			}
		}
	}

	if (!main_cancel) {
		/* Wait for all remaining chunks to finish. */
		for (p = 0; p < nprocs; p++) {
			if (p == np) continue;
			tdat = dary[p];
			sem_wait(&tdat->write_done_sem);
		}
	} else {
		err = 1;
	}

comp_done:
	if (t_errored) err = t_errored;
	if (thread) {
		for (i = 0; i < nprocs; i++) {
			tdat = dary[i];
			tdat->cancel = 1;
			tdat->len_cmp = 0;
			sem_post(&tdat->start_sem);
			sem_post(&tdat->cmp_done_sem);
			pthread_join(tdat->thr, NULL);
			if (encrypt_type)
				hmac_cleanup(&tdat->chunk_hmac);
		}
		pthread_join(writer_thr, NULL);
	}

	if (err) {
		if (compfd != -1 && !pipe_mode)
			unlink(tmpfile1);
		if (filename)
			fprintf(stderr, "Error compressing file: %s\n", filename);
		else
			fprintf(stderr, "Error compressing\n");
	} else {
		/*
		* Write a trailer of zero chunk length.
		*/
		compressed_chunksize = 0;
		if (Write(compfd, &compressed_chunksize,
		    sizeof (compressed_chunksize)) < 0) {
			perror("Write ");
			err = 1;
		}

		/*
		 * Rename the temporary file to the actual compressed file
		 * unless we are in a pipe.
		 */
		if (!pipe_mode) {
			/*
			 * Ownership and mode of target should be same as original.
			 */
			fchmod(compfd, sbuf.st_mode);
			if (fchown(compfd, sbuf.st_uid, sbuf.st_gid) == -1)
				perror("chown ");

			if (rename(tmpfile1, to_filename) == -1) {
				perror("Cannot rename temporary file ");
				unlink(tmpfile1);
			}
		}
	}
	if (dary != NULL) {
		for (i = 0; i < nprocs; i++) {
			if (!dary[i]) continue;
			if (dary[i]->uncompressed_chunk != (uchar_t *)1)
				slab_free(NULL, dary[i]->uncompressed_chunk);
			if (dary[i]->cmp_seg != (uchar_t *)1)
				slab_free(NULL, dary[i]->cmp_seg);
			if ((enable_rabin_scan || enable_fixed_scan)) {
				destroy_dedupe_context(dary[i]->rctx);
			}
			if (_deinit_func)
				_deinit_func(&(dary[i]->data));
			slab_free(NULL, dary[i]);
		}
		slab_free(NULL, dary);
	}
	if (enable_rabin_split) destroy_dedupe_context(rctx);
	if (cread_buf != (uchar_t *)1)
		slab_free(NULL, cread_buf);
	if (!pipe_mode) {
		if (compfd != -1) close(compfd);
		if (uncompfd != -1) close(uncompfd);
	}

	if (!hide_cmp_stats) show_compression_stats(chunksize);
	_stats_func(!hide_cmp_stats);
	slab_cleanup(hide_mem_stats);

	return (err);
}

/*
 * Check the algorithm requested and set the callback routine pointers.
 */
static int
init_algo(const char *algo, int bail)
{
	int rv = 1;
	char algorithm[8];

	/* Copy given string into known length buffer to avoid memcmp() overruns. */
	strncpy(algorithm, algo, 8);
	_props_func = NULL;
	if (memcmp(algorithm, "zlib", 4) == 0) {
		_compress_func = zlib_compress;
		_decompress_func = zlib_decompress;
		_init_func = zlib_init;
		_deinit_func = zlib_deinit;
		_stats_func = zlib_stats;
		_props_func = zlib_props;
		rv = 0;

	} else if (memcmp(algorithm, "lzmaMt", 6) == 0) {
		_compress_func = lzma_compress;
		_decompress_func = lzma_decompress;
		_init_func = lzma_init;
		_deinit_func = lzma_deinit;
		_stats_func = lzma_stats;
		_props_func = lzma_mt_props;
		rv = 0;

	} else if (memcmp(algorithm, "lzma", 4) == 0) {
		_compress_func = lzma_compress;
		_decompress_func = lzma_decompress;
		_init_func = lzma_init;
		_deinit_func = lzma_deinit;
		_stats_func = lzma_stats;
		_props_func = lzma_props;
		rv = 0;

	} else if (memcmp(algorithm, "bzip2", 5) == 0) {
		_compress_func = bzip2_compress;
		_decompress_func = bzip2_decompress;
		_init_func = bzip2_init;
		_deinit_func = NULL;
		_stats_func = bzip2_stats;
		_props_func = bzip2_props;
		rv = 0;

	} else if (memcmp(algorithm, "ppmd", 4) == 0) {
		_compress_func = ppmd_compress;
		_decompress_func = ppmd_decompress;
		_init_func = ppmd_init;
		_deinit_func = ppmd_deinit;
		_stats_func = ppmd_stats;
		_props_func = ppmd_props;
		rv = 0;

	} else if (memcmp(algorithm, "lzfx", 4) == 0) {
		_compress_func = lz_fx_compress;
		_decompress_func = lz_fx_decompress;
		_init_func = lz_fx_init;
		_deinit_func = lz_fx_deinit;
		_stats_func = lz_fx_stats;
		_props_func = lz_fx_props;
		rv = 0;

	} else if (memcmp(algorithm, "lz4", 3) == 0) {
		_compress_func = lz4_compress;
		_decompress_func = lz4_decompress;
		_init_func = lz4_init;
		_deinit_func = lz4_deinit;
		_stats_func = lz4_stats;
		_props_func = lz4_props;
		rv = 0;

	} else if (memcmp(algorithm, "none", 4) == 0) {
		_compress_func = none_compress;
		_decompress_func = none_decompress;
		_init_func = none_init;
		_deinit_func = none_deinit;
		_stats_func = none_stats;
		_props_func = none_props;
		rv = 0;

	/* adapt2 and adapt ordering of the checks matter here. */
	} else if (memcmp(algorithm, "adapt2", 6) == 0) {
		_compress_func = adapt_compress;
		_decompress_func = adapt_decompress;
		_init_func = adapt2_init;
		_deinit_func = adapt_deinit;
		_stats_func = adapt_stats;
		_props_func = adapt_props;
		adapt_mode = 1;
		rv = 0;

	} else if (memcmp(algorithm, "adapt", 5) == 0) {
		_compress_func = adapt_compress;
		_decompress_func = adapt_decompress;
		_init_func = adapt_init;
		_deinit_func = adapt_deinit;
		_stats_func = adapt_stats;
		_props_func = adapt_props;
		adapt_mode = 1;
		rv = 0;
#ifdef ENABLE_PC_LIBBSC
	} else if (memcmp(algorithm, "libbsc", 6) == 0) {
		_compress_func = libbsc_compress;
		_decompress_func = libbsc_decompress;
		_init_func = libbsc_init;
		_deinit_func = libbsc_deinit;
		_stats_func = libbsc_stats;
		_props_func = libbsc_props;
		adapt_mode = 1;
		rv = 0;
#endif
	}

	return (rv);
}

int
main(int argc, char *argv[])
{
	char *filename = NULL;
	char *to_filename = NULL;
	int64_t chunksize = DEFAULT_CHUNKSIZE;
	int opt, level, num_rem, err;

	exec_name = get_execname(argv[0]);
	level = 6;
	err = 0;
	keylen = DEFAULT_KEYLEN;
	slab_init();
	init_pcompress();

	while ((opt = getopt(argc, argv, "dc:s:l:pt:MCDGEe:w:rLPS:B:Fk:")) != -1) {
		int ovr;

		switch (opt) {
		    case 'd':
			do_uncompress = 1;
			break;

		    case 'c':
			do_compress = 1;
			algo = optarg;
			if (init_algo(algo, 1) != 0) {
				err_exit(0, "Invalid algorithm %s\n", optarg);
			}
			break;

		    case 's':
			ovr = parse_numeric(&chunksize, optarg);
			if (ovr == 1)
				err_exit(0, "Chunk size too large %s\n", optarg);
			else if (ovr == 2)
				err_exit(0, "Invalid number %s\n", optarg);

			if (chunksize < MIN_CHUNK) {
				err_exit(0, "Minimum chunk size is %ld\n", MIN_CHUNK);
			}
			if (chunksize > EIGHTY_PCT(get_total_ram())) {
				err_exit(0, "Chunk size must not exceed 80%% of total RAM.\n");
			}
			break;

		    case 'l':
			level = atoi(optarg);
			if (level < 0 || level > MAX_LEVEL)
				err_exit(0, "Compression level should be in range 0 - 14\n");
			break;

		    case 'B':
			rab_blk_size = atoi(optarg);
			if (rab_blk_size < 1 || rab_blk_size > 5)
				err_exit(0, "Average Dedupe block size must be in range 1 (4k) - 5 (64k)\n");
			break;

		    case 'p':
			pipe_mode = 1;
			break;

		    case 't':
			nthreads = atoi(optarg);
			if (nthreads < 1 || nthreads > 256)
				err_exit(0, "Thread count should be in range 1 - 256\n");
			break;

		    case 'M':
			hide_mem_stats = 0;
			break;

		    case 'C':
			hide_cmp_stats = 0;
			break;

		    case 'D':
			enable_rabin_scan = 1;
			break;

		    case 'G':
			enable_rabin_global = 1;
			break;

		    case 'E':
			enable_rabin_scan = 1;
			if (!enable_delta_encode)
				enable_delta_encode = DELTA_NORMAL;
			else
				enable_delta_encode = DELTA_EXTRA;
			break;

		    case 'e':
			encrypt_type = get_crypto_alg(optarg);
			if (encrypt_type == 0) {
				err_exit(0, "Invalid encryption algorithm. Should be AES or SALSA20.\n", optarg);
			}
			break;

		    case 'w':
			pwd_file = strdup(optarg);
			break;

		    case 'F':
			enable_fixed_scan = 1;
			enable_rabin_split = 0;
			break;

		    case 'L':
			lzp_preprocess = 1;
			break;

		    case 'P':
			enable_delta2_encode = 1;
			break;

		    case 'r':
			enable_rabin_split = 0;
			break;

		    case 'k':
			keylen = atoi(optarg);
			if ((keylen != 16 && keylen != 32) || keylen > MAX_KEYLEN) {
				err_exit(0, "Encryption KEY length should be 16 or 32.\n", optarg);
			}
			break;

		    case 'S':
			if (get_checksum_props(optarg, &cksum, &cksum_bytes, &mac_bytes, 0) == -1) {
				err_exit(0, "Invalid checksum type %s\n", optarg);
			}
			break;

		    case '?':
		    default:
			usage();
			exit(1);
			break;
		}
	}

	if ((do_compress && do_uncompress) || (!do_compress && !do_uncompress)) {
		usage();
		exit(1);
	}

	/*
	 * Remaining mandatory arguments are the filenames.
	 */
	num_rem = argc - optind;
	if (pipe_mode && num_rem > 0 ) {
		fprintf(stderr, "Filename(s) unexpected for pipe mode\n");
		usage();
		exit(1);
	}

	if ((enable_rabin_scan || enable_fixed_scan) && !do_compress) {
		fprintf(stderr, "Deduplication is only used during compression.\n");
		usage();
		exit(1);
	}
	if (!enable_rabin_scan)
		enable_rabin_split = 0;

	if (enable_fixed_scan && (enable_rabin_scan || enable_delta_encode || enable_rabin_split)) {
		fprintf(stderr, "Rabin Deduplication and Fixed block Deduplication are mutually exclusive\n");
		exit(1);
	}

	if (!do_compress && encrypt_type) {
		fprintf(stderr, "Encryption only makes sense when compressing!\n");
		exit(1);

	} else if (pipe_mode && encrypt_type && !pwd_file) {
		fprintf(stderr, "Pipe mode requires password to be provided in a file.\n");
		exit(1);
	}

	/*
	 * Global Deduplication can use Rabin or Fixed chunking. Default, if not specified,
	 * is to use Rabin.
	 */
	if (enable_rabin_global && !enable_rabin_scan && !enable_fixed_scan) {
		enable_rabin_scan = 1;
		enable_rabin_split = 1;
	}

	if (enable_rabin_global && pipe_mode) {
		fprintf(stderr, "Global Deduplication is not supported in pipe mode.\n");
		exit(1);
	}

	if (enable_rabin_global && enable_delta_encode) {
		fprintf(stderr, "Global Deduplication does not support Delta Compression.\n");
		exit(1);
	}

	if (num_rem == 0 && !pipe_mode) {
		usage(); /* At least 1 filename needed. */
		exit(1);

	} else if (num_rem == 1) {
		if (do_compress) {
			char apath[MAXPATHLEN];

			if ((filename = realpath(argv[optind], NULL)) == NULL)
				err_exit(1, "%s", argv[optind]);
			/* Check if compressed file exists */
			strcpy(apath, filename);
			strcat(apath, COMP_EXTN);
			if ((to_filename = realpath(apath, NULL)) != NULL) {
				free(filename);
				err_exit(0, "Compressed file %s exists\n", to_filename);
			}
		} else {
			usage();
			exit(1);
		}
	} else if (num_rem == 2) {
		if (do_uncompress) {
			if ((filename = realpath(argv[optind], NULL)) == NULL)
				err_exit(1, "%s", argv[optind]);
			optind++;
			if ((to_filename = realpath(argv[optind], NULL)) != NULL) {
				free(filename);
				free(to_filename);
				err_exit(0, "File %s exists\n", argv[optind]);
			}
			to_filename = argv[optind];
		} else {
			usage();
			exit(1);
		}
	} else if (num_rem > 2) {
		fprintf(stderr, "Too many filenames.\n");
		usage();
		exit(1);
	}
	main_cancel = 0;

	if (cksum == 0)
		get_checksum_props(DEFAULT_CKSUM, &cksum, &cksum_bytes, &mac_bytes, 0);

	if (!encrypt_type) {
		/*
		 * If not encrypting we compute a header CRC32.
		 */
		mac_bytes = sizeof (uint32_t); // CRC32 in non-crypto mode
	} else {
		/*
		 * When encrypting we do not compute a normal digest. The HMAC
		 * is computed over header and encrypted data.
		 */
		cksum_bytes = 0;
	}

	/*
	 * Start the main routines.
	 */
	if (do_compress)
		err = start_compress(filename, chunksize, level);
	else if (do_uncompress)
		err = start_decompress(filename, to_filename);

	if (pwd_file)
		free(pwd_file);
	free(filename);
	free((void *)exec_name);
	return (err);
}
