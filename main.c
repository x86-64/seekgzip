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
#include "seekgzip.h"

#define CHUNK 16384		 /* file input buffer size */

static void seekgzip_perror(int ret)
{
	switch (ret) {
	case SEEKGZIP_ERROR:
		fprintf(stderr, "ERROR: An unknown error occurred.\n");
		break;
	case SEEKGZIP_OPENERROR:
		fprintf(stderr, "ERROR: Failed to open a file.\n");
		break;
	case SEEKGZIP_READERROR:
		fprintf(stderr, "ERROR: Failed to read a file.\n");
		break;
	case SEEKGZIP_WRITEERROR:
		fprintf(stderr, "ERROR: Failed to write a file.\n");
		break;
	case SEEKGZIP_DATAERROR:
		fprintf(stderr, "ERROR: The file is corrupted.\n");
		break;
	case SEEKGZIP_OUTOFMEMORY:
		fprintf(stderr, "ERROR: Out of memory.\n");
		break;
	case SEEKGZIP_IMCOMPATIBLE:
		fprintf(stderr, "ERROR: The imcompatible file.\n");
		break;
	case SEEKGZIP_ZLIBERROR:
		fprintf(stderr, "ERROR: An error occurred in zlib.\n");
		break;
	}
}

int main(int argc, char *argv[])
{
	int ret = 0;

	if (argc != 3) {
		printf("This utility manages an index for random (seekable) access to a gzip file.\n");
		printf("USAGE:\n");
		printf("	%s -b <FILE>\n", argv[0]);
		printf("		Build an index file \"$FILE.idx\" for the gzip file $FILE.\n");
		printf("	%s <FILE> [BEGIN-END]\n", argv[0]);
		printf("		Output the content of the gzip file $FILE of offset range [BEGIN-END].\n");
		return 0;

	} else if (strcmp(argv[1], "-b") == 0) {
		const char *target = argv[2];

		printf("Building an index: %s.idx\n", target);
		printf("Filesize up to: %d bit\n", (int)sizeof(off_t) * 8);
		printf("WARNING: if program fail to write index to file, it would silently ignore that\n");

		seekgzip_t* zs = seekgzip_open(target, &ret);
		if (zs == NULL) {
			seekgzip_perror(ret);
			return 1;
		}
		seekgzip_close(zs);
	return 0;

	} else {
		char *arg = argv[2], *p = NULL;
		off_t begin = 0, end = (off_t)-1;
		seekgzip_t* zs = seekgzip_open(argv[1], NULL);
		if (zs == NULL) {
			fprintf(stderr, "ERROR: Failed to open the index file.\n");
			return 1;
		}

		p = strchr(arg, '-');
		if (p == NULL) {
			begin =(off_t)strtoull(arg, NULL, 10);
			end = begin+1;
		} else if (p == arg) {
			begin = 0;
			end = (off_t)strtoull(p+1, NULL, 10);
		} else if (p == arg + strlen(arg) - 1) {
			*p = 0;
			begin = (off_t)strtoull(arg, NULL, 10);
		} else {
			*p++ = 0;
			begin =(off_t)strtoull(arg, NULL, 10);
			end =(off_t)strtoull(p, NULL, 10);
		}

		seekgzip_seek(zs, begin);

		while (begin < end) {
			int read;
			char buffer[CHUNK];
			off_t size = (end - begin);
			if (CHUNK < size) {
				size = CHUNK;
			}
			read = seekgzip_read(zs, buffer, (int)size);
			if (0 < read) {
				begin += read;
				
		if(fwrite(buffer, read, sizeof(char), stdout) == 0)
			continue;
			} else if (read == 0) {
				break;
			} else {
				fprintf(stderr, "ERROR: An error occurred while reading the gzip file.\n");
				ret = 1;
				break;
			}
		}
	
		seekgzip_close(zs);
		return ret;
	}
}

