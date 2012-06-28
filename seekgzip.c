/*
 *		SeekGzip utility/library.
 *
 * Copyright (c) 2010-2011, Naoaki Okazaki
 * All rights reserved.
 * 
 * For conditions of distribution and use, see copyright notice in README
 * or zlib.h.
 * 
 * The core algorithm for random access originates from zran.c in zlib/gzip
 * distribution. This code simply implements a data structure and algorithm
 * for indices, wraps the functionality of random access as a library, and
 * provides a command-line utility.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <zlib.h>
#include <utime.h>
#include <sys/stat.h>
#include "seekgzip.h"

#define SEEKGZIP_OPTIMIZATION

#define __HALF_MAX_SIGNED(type) ((type)1 << (sizeof(type)*8-2))
#define __MAX_SIGNED(type) (__HALF_MAX_SIGNED(type) - 1 + __HALF_MAX_SIGNED(type))
#define __MIN_SIGNED(type) (-1 - __MAX_SIGNED(type))
#define __MIN(type) ((type)-1 < 1?__MIN_SIGNED(type):(type)0)
#define __MAX(type) ((type)~__MIN(type))

int  seekgzip_index_alloc(seekgzip_t *sz);
void seekgzip_index_free(seekgzip_t *sz);

struct tag_seekgzip {
	char                  *path_data;
	char                  *path_index;
	FILE                  *fp;
	struct access         *index;
	off_t                  offset;
	off_t                  totin;
	off_t                  totout;
	int                    errorcode;
};

/*===== Begin of the portion of zran.c ===== {{{*/

/* zran.c -- example of zlib/gzip stream indexing and random access
 * Copyright (C) 2005 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
   Version 1.0  29 May 2005  Mark Adler */

/* Illustrate the use of Z_BLOCK, inflatePrime(), and inflateSetDictionary()
   for random access of a compressed file.  A file containing a zlib or gzip
   stream is provided on the command line.  The compressed stream is decoded in
   its entirety, and an index built with access points about every SPAN bytes
   in the uncompressed output.  The compressed file is left open, and can then
   be read randomly, having to decompress on the average SPAN/2 uncompressed
   bytes before getting to the desired block of data.

   An access point can be created at the start of any deflate block, by saving
   the starting file offset and bit of that block, and the 32K bytes of
   uncompressed data that precede that block.  Also the uncompressed offset of
   that block is saved to provide a referece for locating a desired starting
   point in the uncompressed stream.  build_index() works by decompressing the
   input zlib or gzip stream a block at a time, and at the end of each block
   deciding if enough uncompressed data has gone by to justify the creation of
   a new access point.  If so, that point is saved in a data structure that
   grows as needed to accommodate the points.

   To use the index, an offset in the uncompressed data is provided, for which
   the latest accees point at or preceding that offset is located in the index.
   The input file is positioned to the specified location in the index, and if
   necessary the first few bits of the compressed data is read from the file.
   inflate is initialized with those bits and the 32K of uncompressed data, and
   the decompression then proceeds until the desired offset in the file is
   reached.  Then the decompression continues to read the desired uncompressed
   data from the file.

   Another approach would be to generate the index on demand.  In that case,
   requests for random access reads from the compressed data would try to use
   the index, but if a read far enough past the end of the index is required,
   then further index entries would be generated and added.

   There is some fair bit of overhead to starting inflation for the random
   access, mainly copying the 32K byte dictionary.  So if small pieces of the
   file are being accessed, it would make sense to implement a cache to hold
   some lookahead and avoid many calls to extract() for small lengths.

   Another way to build an index would be to use inflateCopy().  That would
   not be constrained to have access points at block boundaries, but requires
   more memory per access point, and also cannot be saved to file due to the
   use of pointers in the state.  The approach here allows for storage of the
   index in a file.
 */

#define SPAN 1048576L	   /* desired distance between access points */
#define WINSIZE 32768U	  /* sliding window size */
#define CHUNK 16384		 /* file input buffer size */

/* access point entry */
struct point {
	off_t out;		  /* corresponding offset in uncompressed data */
	off_t in;		   /* offset in input file of first full byte */
	int bits;		   /* number of bits (1-7) from byte at in - 1, or 0 */
	unsigned char window[WINSIZE];  /* preceding 32K of uncompressed data */
};

/* access point list */
struct access {
	uintmax_t nelements;		   /* number of list entries filled in */
	uintmax_t allocated;		   /* number of list entries allocated */
	struct point *list; /* allocated list */
};

/* Add an entry to the access point list.  If out of memory, deallocate the
   existing list and return NULL. */
static struct access *addpoint(struct access *index, int bits,
	off_t in, off_t out, unsigned left, unsigned char *window)
{
	struct point *next;

	/* if list is full, make it bigger */
	if (index->nelements == index->allocated) {
		index->allocated = index->allocated != 0 ? index->allocated : 1; 
		index->allocated <<= 1;
		index->list = (struct point*)realloc(index->list, sizeof(struct point) * index->allocated);
		if (index->list == NULL)
		return NULL;
	}

	/* fill in entry and increment how many we have */
	next = index->list + index->nelements;
	next->bits = bits;
	next->in = in;
	next->out = out;
	if (left)
		memcpy(next->window, window + WINSIZE - left, left);
	if (left < WINSIZE)
		memcpy(next->window + left, window, WINSIZE - left);
	index->nelements++;

	/* return list, possibly reallocated */
	return index;
}

#ifdef  SEEKGZIP_OPTIMIZATION
struct point *findpoint(struct access *index, off_t offset)
{
	int half, len = index->nelements;
	struct point *first = &index->list[0], *middle;

	/* equivalent to std::upper_bound() */
	while (0 < len) {
		half = (len >> 1);
		middle = first + half;
		if (offset < middle->out) {
			len = half;
		} else {
			first = middle + 1;
			len = len - half - 1;
		}
	}

	/* decrement the point */
	return (first == &index->list[0] ? NULL : first-1);
}
#endif/*SEEKGZIP_OPTIMIZATION*/

/* Make one entire pass through the compressed stream and build an index, with
   access points about every span bytes of uncompressed output -- span is
   chosen to balance the speed of random access against the memory requirements
   of the list, about 32K bytes per access point.  Note that data after the end
   of the first zlib or gzip stream in the file is ignored.  build_index()
   returns the number of access points on success (>= 1), Z_MEM_ERROR for out
   of memory, Z_DATA_ERROR for an error in the input file, or Z_ERRNO for a
   file read error.  On success, *built points to the resulting index. */
static int build_index(FILE *in, off_t span, struct access **built, seekgzip_t *sz)
{
	int ret;
	off_t totin, totout;		/* our own total counters to avoid 4GB limit */
	off_t last;				 /* totout value of last access point */
	struct access *index = *built; /* access points being generated */
	z_stream strm;
	unsigned char input[CHUNK];
	unsigned char window[WINSIZE];

	/* initialize inflate */
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.avail_in = 0;
	strm.next_in = Z_NULL;
	ret = inflateInit2(&strm, 47);	  /* automatic zlib or gzip decoding */
	if (ret != Z_OK)
		return ret;
	
	// Rewind to beginning  of file
	if( (ret = fseeko(in, 0, SEEK_SET)) == -1)
		goto build_index_error;

	/* inflate the input, maintain a sliding window, and build an index -- this
	   also validates the integrity of the compressed data using the check
	   information at the end of the gzip or zlib stream */
	totin = totout = last = 0;
	strm.avail_out = 0;
	do {
		/* get some compressed data from input file */
		strm.avail_in = fread(input, 1, CHUNK, in);
		if (ferror(in)) {
			ret = Z_ERRNO;
			goto build_index_error;
		}
		if (strm.avail_in == 0) {
			ret = Z_DATA_ERROR;
			goto build_index_error;
		}
		strm.next_in = input;

		/* process all of that, or until end of stream */
		do {
			/* reset sliding window if necessary */
			if (strm.avail_out == 0) {
				strm.avail_out = WINSIZE;
				strm.next_out = window;
			}

			/* inflate until out of input, output, or at end of block --
			   update the total input and output counters */
			totin += strm.avail_in;
			totout += strm.avail_out;
			ret = inflate(&strm, Z_BLOCK);	  /* return at end of block */
			totin -= strm.avail_in;
			totout -= strm.avail_out;
			if (ret == Z_NEED_DICT)
				ret = Z_DATA_ERROR;
			if (ret == Z_MEM_ERROR || ret == Z_DATA_ERROR)
				goto build_index_error;
			if (ret == Z_STREAM_END)
				break;

			/* if at end of block, consider adding an index entry (note that if
			   data_type indicates an end-of-block, then all of the
			   uncompressed data from that block has been delivered, and none
			   of the compressed data after that block has been consumed,
			   except for up to seven bits) -- the totout == 0 provides an
			   entry point after the zlib or gzip header, and assures that the
			   index always has at least one access point; we avoid creating an
			   access point after the last block by checking bit 6 of data_type
			 */
			if ((strm.data_type & 128) && !(strm.data_type & 64) &&
				(totout == 0 || totout - last > span)) {
				index = addpoint(index, strm.data_type & 7, totin,
								 totout, strm.avail_out, window);
				if (index == NULL) {
					ret = Z_MEM_ERROR;
					goto build_index_error;
				}
				last = totout;
			}
		} while (strm.avail_in != 0);
	} while (ret != Z_STREAM_END);

	/* clean up and return index (release unused entries in list) */
	(void)inflateEnd(&strm);
	index->list = (struct point*)realloc(index->list, sizeof(struct point) * index->nelements);
	index->allocated = index->nelements;
	*built = index;
	sz->totin  = totin;
	sz->totout = totout;
	return index->allocated;

	/* return error */
  build_index_error:
	(void)inflateEnd(&strm);
	return ret;
}

/* Use the index to read len bytes from offset into buf, return bytes read or
   negative for error (Z_DATA_ERROR or Z_MEM_ERROR).  If data is requested past
   the end of the uncompressed data, then extract() will return a value less
   than len, indicating how much as actually read into buf.  This function
   should not return a data error unless the file was modified since the index
   was generated.  extract() may also return Z_ERRNO if there is an error on
   reading or seeking the input file. */
static int extract(FILE *in, struct access *index, off_t offset,
				  unsigned char *buf, int len)
{
	int ret, skip;
	z_stream strm;
	struct point *here;
	unsigned char input[CHUNK];
	unsigned char discard[WINSIZE];

	/* proceed only if something reasonable to do */
	if (len < 0)
		return 0;

	/* find where in stream to start */
#ifdef  SEEKGZIP_OPTIMIZATION
	here = findpoint(index, offset);
	if (here == NULL) {
		/* possibly out of range. */
		return 0;
	}
#else
	here = index->list;
	ret = index->nelements;
	while (--ret && here[1].out <= offset)
		here++;
#endif/*SEEKGZIP_OPTIMIZATION*/

	/* initialize file and inflate state to start there */
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.avail_in = 0;
	strm.next_in = Z_NULL;
	ret = inflateInit2(&strm, -15);		 /* raw inflate */
	if (ret != Z_OK)
		return ret;
	ret = fseeko(in, here->in - (here->bits ? 1 : 0), SEEK_SET);
	if (ret == -1)
		goto extract_ret;
	if (here->bits) {
		ret = getc(in);
		if (ret == -1) {
			ret = ferror(in) ? Z_ERRNO : Z_DATA_ERROR;
			goto extract_ret;
		}
		(void)inflatePrime(&strm, here->bits, ret >> (8 - here->bits));
	}
	(void)inflateSetDictionary(&strm, here->window, WINSIZE);

	/* skip uncompressed bytes until offset reached, then satisfy request */
	offset -= here->out;
	strm.avail_in = 0;
	skip = 1;							   /* while skipping to offset */
	do {
		/* define where to put uncompressed data, and how much */
		if (offset == 0 && skip) {		  /* at offset now */
			strm.avail_out = len;
			strm.next_out = buf;
			skip = 0;					   /* only do this once */
		}
		if (offset > WINSIZE) {			 /* skip WINSIZE bytes */
			strm.avail_out = WINSIZE;
			strm.next_out = discard;
			offset -= WINSIZE;
		}
		else if (offset != 0) {			 /* last skip */
			strm.avail_out = (unsigned)offset;
			strm.next_out = discard;
			offset = 0;
		}

		/* uncompress until avail_out filled, or end of stream */
		do {
			if (strm.avail_in == 0) {
				strm.avail_in = fread(input, 1, CHUNK, in);
				if (ferror(in)) {
					ret = Z_ERRNO;
					goto extract_ret;
				}
				if (strm.avail_in == 0) {
					ret = Z_DATA_ERROR;
					goto extract_ret;
				}
				strm.next_in = input;
			}
			ret = inflate(&strm, Z_NO_FLUSH);	   /* normal inflate */
			if (ret == Z_NEED_DICT)
				ret = Z_DATA_ERROR;
			if (ret == Z_MEM_ERROR || ret == Z_DATA_ERROR)
				goto extract_ret;
			if (ret == Z_STREAM_END)
				break;
		} while (strm.avail_out != 0);

		/* if reach end of stream, then don't keep trying to get more */
		if (ret == Z_STREAM_END)
			break;

		/* do until offset reached and requested data read, or stream ends */
	} while (skip);

	/* compute number of uncompressed bytes read after offset */
	ret = skip ? 0 : len - strm.avail_out;

	/* clean up and return bytes read or error */
  extract_ret:
	(void)inflateEnd(&strm);
	return ret;
}

/*===== End of the portion of zran.c ===== }}}*/

static char *get_index_file(const char *target)
{
	char *idx = (char*)malloc(strlen(target) + 4 + 1);
	if (idx == NULL) {
		return NULL;
	}
	strcpy(idx, target);
	strcat(idx, ".idx");
	return idx;	
}

static int write_uint32(gzFile gz, uint32_t v)
{
	return gzwrite(gz, &v, sizeof(v));
}

static uint32_t read_uint32(gzFile gz)
{
	uint32_t v;
	gzread(gz, &v, sizeof(v));
	return v;
}

void seekgzip_index_free(seekgzip_t *sz){
	if(sz->index == NULL)
		return;
	
	if(sz->index->list != NULL)
		free(sz->index->list);
	
	free(sz->index);
	sz->index = NULL;
}

int seekgzip_index_alloc(seekgzip_t *sz){
	if(sz->index != NULL)
		seekgzip_index_free(sz);
	
	if( (sz->index = (struct access *)malloc(sizeof(struct access))) == NULL)
		return SEEKGZIP_OUTOFMEMORY;
	
	sz->index->nelements = 0;
	sz->index->allocated = 0;
	sz->index->list	     = NULL;
	return SEEKGZIP_SUCCESS;
}

int seekgzip_index_checkutime(seekgzip_t *sz){
	int                    ret;
	struct stat            stats_data;
	struct stat            stats_index;
	
	if( (ret = stat(sz->path_data, &stats_data)) != 0)
		return ret;
	if( (ret = stat(sz->path_index, &stats_index)) != 0)
		return ret;
	
	return (stats_data.st_mtime == stats_index.st_mtime) ?
		0 : 1;
}

int seekgzip_index_setutime(seekgzip_t *sz){
	int                    ret;
	struct stat            stats;
	struct utimbuf         times;
	
	if( (ret = stat(sz->path_data, &stats)) != 0)
		return ret;
	
	times.actime  = stats.st_atime;
	times.modtime = stats.st_mtime;
	
	if( (ret = utime(sz->path_index, &times)) != 0)
		return ret;
	
	return 0;
}

int seekgzip_index_build(seekgzip_t *sz)
{
	int len, ret = SEEKGZIP_SUCCESS;

	// Build an index for the file.
	len = build_index(sz->fp, SPAN, &sz->index, sz);
	if (len < 0) {
		switch (len) {
		case Z_MEM_ERROR:
			ret = SEEKGZIP_OUTOFMEMORY;
		case Z_DATA_ERROR:
			ret = SEEKGZIP_DATAERROR;
		case Z_ERRNO:
			ret = SEEKGZIP_READERROR;
		default:
			ret = SEEKGZIP_ERROR;
		}
	
		// invalid index, so - free it
		seekgzip_index_free(sz);
	}
	return ret;
}

int seekgzip_index_save(seekgzip_t *sz){
	int ret = SEEKGZIP_SUCCESS;
	uintmax_t i;
	gzFile gz;

	// Open the index file for writing.
	gz = gzopen(sz->path_index, "wb");
	if (gz == NULL)
		return SEEKGZIP_OPENERROR;

	// Write a header.
	gzwrite(gz, "ZSE2", 4);
	write_uint32(gz, (uint32_t)sizeof(off_t));
	write_uint32(gz, (uint32_t)sz->index->nelements);
	gzwrite(gz, &sz->totin,  sizeof(off_t));
	gzwrite(gz, &sz->totout, sizeof(off_t));
	
	// Write out entry points.
	for (i = 0;i < sz->index->nelements;++i) {
		gzwrite(gz, &sz->index->list[i].out, sizeof(off_t));
		gzwrite(gz, &sz->index->list[i].in, sizeof(off_t));
		gzwrite(gz, &sz->index->list[i].bits, sizeof(int));
		gzwrite(gz, sz->index->list[i].window, WINSIZE);
	}

	gzclose(gz);
	
	seekgzip_index_setutime(sz);
	return ret;
}

int seekgzip_index_load(seekgzip_t *sz){
	int ret = SEEKGZIP_SUCCESS;
	uintmax_t i;
	gzFile gz;
	
	if( (ret = seekgzip_index_alloc(sz)) != SEEKGZIP_SUCCESS)
		return ret;
	
	// Check index mod time
	switch( (ret = seekgzip_index_checkutime(sz)) ){
		case 0: // match
			break;
		case 1: // not match
			return SEEKGZIP_EXPIREDINDEX;
		default:
			return SEEKGZIP_OPENERROR;
	}

	// Open the index file for reading.
	gz = gzopen(sz->path_index, "rb");
	if (gz == NULL)
		return SEEKGZIP_OPENERROR;

	// Read the magic string.
	if (gzgetc(gz) != 'Z' || gzgetc(gz) != 'S' || gzgetc(gz) != 'E' || gzgetc(gz) != '2'){
		ret = SEEKGZIP_IMCOMPATIBLE;
		goto error_exit;
	}

	// Check the size of off_t.
	if (read_uint32(gz) != sizeof(off_t)) {
		ret = SEEKGZIP_IMCOMPATIBLE;
		goto error_exit;
	}

	// Read the number of entry points.
	sz->index->nelements = sz->index->allocated = read_uint32(gz);
	if(__MAX(uintmax_t) / sizeof(struct point) <= sz->index->nelements){
		ret = SEEKGZIP_IMCOMPATIBLE;
		goto error_exit;
	}

	// Allocate an array for entry points.
	sz->index->list = (struct point*)malloc(sizeof(struct point) * sz->index->nelements);
	if (sz->index->list == NULL) {
		ret = SEEKGZIP_OUTOFMEMORY;
		goto error_exit;
	}
	
	// Read size of unpacked file
	gzread(gz, &sz->totin,  sizeof(off_t));
	gzread(gz, &sz->totout, sizeof(off_t));
	
	// Read entry points.
	for (i = 0; i < sz->index->nelements; ++i) {
		gzread(gz, &sz->index->list[i].out, sizeof(off_t));
		gzread(gz, &sz->index->list[i].in, sizeof(off_t));
		gzread(gz, &sz->index->list[i].bits, sizeof(int));
		gzread(gz, sz->index->list[i].window, WINSIZE);
	}

error_exit:
	// Close the index filiiiie.
	if (gzclose(gz) != 0) {
		ret = SEEKGZIP_ZLIBERROR;
		goto error_exit;
	}
	return ret;
}

seekgzip_t* seekgzip_open(const char *target, int flags)
{
	seekgzip_t *sz;
	
	if( (sz = (seekgzip_t *)malloc(sizeof(seekgzip_t))) == NULL)
		return NULL;
	
	sz->offset = 0;
	sz->errorcode = 0;
	sz->index = NULL;

	// Open the target gzip file for reading.
	sz->fp = fopen(target, "rb");
	if (sz->fp == NULL) {
		sz->errorcode = SEEKGZIP_OPENERROR;
		goto error_exit;
	}
	
	if( (sz->path_data = strdup(target)) == NULL){
		sz->errorcode = SEEKGZIP_OUTOFMEMORY;
		goto error_exit;
	}

	// Prepare the name for the index file.
	sz->path_index = get_index_file(target);
	if (sz->path_index == NULL) {
		sz->errorcode = SEEKGZIP_OUTOFMEMORY;
		goto error_exit;
	}

	// Load index
	sz->errorcode = seekgzip_index_load(sz);
	switch(sz->errorcode){
		case SEEKGZIP_SUCCESS:
			break;
		case SEEKGZIP_OPENERROR:
		case SEEKGZIP_IMCOMPATIBLE:
		case SEEKGZIP_EXPIREDINDEX:
			// build index and save it

			sz->errorcode = seekgzip_index_build(sz);
			if( sz->errorcode == SEEKGZIP_SUCCESS ){
				seekgzip_index_save(sz); // return value is not important, maybe we cannot write to file, so
							 // we rebuild index on every program start. (should be warning somehow shown)
			}
			break;

		default:
			goto error_exit;
	}

error_exit:
	return sz;
}

void seekgzip_close(seekgzip_t* sz)
{
	if (sz == NULL)
		return;
	
	seekgzip_index_free(sz);
	if (sz->fp != NULL){
		fclose(sz->fp);
		sz->fp = NULL;
	}
	if (sz->path_index != NULL){
		free(sz->path_index);
		sz->path_index = NULL;
	}
	if (sz->path_data != NULL){
		free(sz->path_data);
		sz->path_data = NULL;
	}
	free(sz);
}

void seekgzip_seek(seekgzip_t *sz, off_t offset)
{
	sz->offset = offset;
}

off_t seekgzip_tell(seekgzip_t *sz)
{
	return sz->offset;
}

off_t seekgzip_unpacked_length(seekgzip_t *sz)
{
	return sz->totout;
}

off_t seekgzip_packed_length(seekgzip_t *sz)
{
	return sz->totin;
}

int seekgzip_read(seekgzip_t* sz, void *buffer, int size)
{
	int len = extract(sz->fp, sz->index, sz->offset, (unsigned char*)buffer, size);
	if (0 < len) {
		sz->offset += len;
	}
	return len;
}

int seekgzip_error(seekgzip_t* sz)
{
	if(sz == NULL)
		return SEEKGZIP_OUTOFMEMORY;
	
	return sz->errorcode;
}
