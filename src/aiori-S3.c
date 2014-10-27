/* -*- mode: c; indent-tabs-mode: t; -*-
 * vim:noexpandtab:
 *
 * Editing with tabs allows different users to pick their own indentation
 * appearance without changing the file.
 */

/*
 * Copyright (c) 2009, Los Alamos National Security, LLC All rights reserved.
 * Copyright 2009. Los Alamos National Security, LLC. This software was produced
 * under U.S. Government contract DE-AC52-06NA25396 for Los Alamos National
 * Laboratory (LANL), which is operated by Los Alamos National Security, LLC for
 * the U.S. Department of Energy. The U.S. Government has rights to use,
 * reproduce, and distribute this software.  NEITHER THE GOVERNMENT NOR LOS
 * ALAMOS NATIONAL SECURITY, LLC MAKES ANY WARRANTY, EXPRESS OR IMPLIED, OR
 * ASSUMES ANY LIABILITY FOR THE USE OF THIS SOFTWARE.  If software is
 * modified to produce derivative works, such modified software should be
 * clearly marked, so as not to confuse it with the version available from
 * LANL.
 *
 * Additionally, redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following conditions are
 * met:
 *
 * •   Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * •   Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * •   Neither the name of Los Alamos National Security, LLC, Los Alamos National
 * Laboratory, LANL, the U.S. Government, nor the names of its contributors may be
 * used to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY LOS ALAMOS NATIONAL SECURITY, LLC AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL LOS ALAMOS NATIONAL SECURITY, LLC OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 */

/******************************************************************************\
*
* Implement of abstract I/O interface for HDFS.
*
* HDFS has the added concept of a "File System Handle" which has to be
* connected before files are opened.  We store this in the IOR_param_t
* object that is always passed to our functions.  The thing that callers
* think of as the "fd" is an hdfsFile, (a pointer).
*
\******************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>				/* strnstr() */

#include <errno.h>
#include <assert.h>
/*
#ifdef HAVE_LUSTRE_LUSTRE_USER_H
#include <lustre/lustre_user.h>
#endif
*/

#include "ior.h"
#include "aiori.h"
#include "iordef.h"

#include <curl/curl.h>

#include <libxml/parser.h>      // from libxml2
#include <libxml/tree.h>

#include "aws4c.h"              // extended vers of "aws4c" lib for S3 via libcurl
#include "aws4c_extra.h"        // utilities, e.g. for parsing XML in responses


/**************************** P R O T O T Y P E S *****************************/
static void *S3_Create(char *, IOR_param_t *);
static void *S3_Open(char *, IOR_param_t *);
static IOR_offset_t S3_Xfer(int, void *, IOR_size_t *,
                               IOR_offset_t, IOR_param_t *);
static void S3_Close(void *, IOR_param_t *);
static void S3_Delete(char *, IOR_param_t *);
static void S3_SetVersion(IOR_param_t *);
static void S3_Fsync(void *, IOR_param_t *);
static IOR_offset_t S3_GetFileSize(IOR_param_t *, MPI_Comm, char *);

/************************** D E C L A R A T I O N S ***************************/

ior_aiori_t s3_aiori = {
	"S3",
	S3_Create,
	S3_Open,
	S3_Xfer,
	S3_Close,
	S3_Delete,
	S3_SetVersion,
	S3_Fsync,
	S3_GetFileSize
};

/* modelled on similar macros in iordef.h */
#define CURL_ERR(MSG, CURL_ERRNO, PARAM)										\
	do {																					\
		fprintf(stdout, "ior ERROR: %s: %s (curl-errno=%d) (%s:%d)\n",	\
				  MSG, curl_easy_strerror(CURL_ERRNO), CURL_ERRNO,			\
				  __FILE__, __LINE__);												\
		fflush(stdout);																\
		MPI_Abort((PARAM)->testComm, -1);										\
	} while (0)
	

#define CURL_WARN(MSG, CURL_ERRNO)													\
	do {																						\
		fprintf(stdout, "ior WARNING: %s: %s (curl-errno=%d) (%s:%d)\n",	\
				  MSG, curl_easy_strerror(CURL_ERRNO), CURL_ERRNO,				\
				  __FILE__, __LINE__);													\
		fflush(stdout);																	\
	} while (0)
	


/* buffer is used to generate URLs, err_msgs, etc */
#define            BUFF_SIZE  1024
static char        buff[BUFF_SIZE];

const int          ETAG_SIZE = 32;

CURLcode           rc;

/* Any objects we create or delete will be under this bucket */
const char* bucket_name = "ior";


/***************************** F U N C T I O N S ******************************/




/* ---------------------------------------------------------------------------
 * "Connect" to an S3 object-file-system.  We're really just initializing
 * libcurl.  We need this done before any interactions.  It is easy for
 * ior_aiori.open/create to assure that we connect, if we haven't already
 * done so.  However, there's not a simple way to assure that we
 * "disconnect" at the end.  For now, we'll make a special call at the end
 * of ior.c
 *
 * NOTE: It's okay to call this thing whenever you need to be sure the curl
 *       handle is initialized.
 *
 * NOTE: Our custom version of aws4c can be configured so that connections
 *       are reused, instead of opened and closed on every operation.  We
 *       do configure it that way, but you still need to call these
 *       connect/disconnet functions, in order to insure that aws4c has
 *       been configured.
 * ---------------------------------------------------------------------------
 */

static
void
s3_connect( IOR_param_t* param ) {
	if (param->verbose >= VERBOSE_2) {
		printf("-> s3_connect\n"); /* DEBUGGING */
	}

	if ( param->curl_flags & IOR_CURL_INIT ) {
		if (param->verbose >= VERBOSE_2) {
			printf("<- s3_connect  [nothing to do]\n"); /* DEBUGGING */
		}
		return;
	}

	// --- Done once-only (per rank).  Perform all first-time inits.
	//
	// The aws library requires a config file, as illustrated below.  We
	// assume that the user running the test has an entry in this file,
	// using their login moniker (i.e. `echo $USER`) as the key, as
	// suggested in the example:
	//
	//     <user>:<s3_login_id>:<s3_private_key>
	//
	// This file must not be readable by other than user.
	//
	// NOTE: These inits could be done in init_IORParam_t(), in ior.c, but
	//       would require conditional compilation, there.

	aws_read_config(getenv("USER"));  // requires ~/.awsAuth
	aws_reuse_connections(1);
	aws_set_debug(param->verbose >= 4);

	// initalize IOBufs.  These are basically dynamically-extensible
	// linked-lists.  "growth size" controls the increment of new memory
	// allocated, whenever storage is used up.
	param->io_buf = aws_iobuf_new();
	aws_iobuf_growth_size(param->io_buf, 1024*1024*1);

	param->etags = aws_iobuf_new();
	aws_iobuf_growth_size(param->etags, 1024*1024*8);

	// our hosts are currently 10.140.0.15 - 10.140 0.18
	snprintf(buff, BUFF_SIZE, "10.140.0.%d:9020", 15 + (rank % 4));
	s3_set_host(buff);


	// make sure test-bucket exists
	s3_set_bucket((char*)bucket_name);
   AWS4C_CHECK( s3_head(param->io_buf, "") );
   if ( param->io_buf->code == 404 ) {					// "404 Not Found"
      printf("  bucket '%s' doesn't exist\n", bucket_name);

      AWS4C_CHECK( s3_put(param->io_buf, "") );	/* creates URL as bucket + obj */
      AWS4C_CHECK_OK(     param->io_buf );		// assure "200 OK"
		printf("created bucket '%s'\n", bucket_name);
	}
   else {														// assure "200 OK"
      AWS4C_CHECK_OK( param->io_buf );
	}

	// don't perform these inits more than once
	param->curl_flags |= IOR_CURL_INIT;


	if (param->verbose >= VERBOSE_2) {
		printf("<- s3_connect  [success]\n");
	}
}

static
void
s3_disconnect( IOR_param_t* param ) {
	if (param->verbose >= VERBOSE_2) {
		printf("-> s3_disconnect\n");
	}

	// nothing to do here, if using new aws4c ...

	if (param->verbose >= VERBOSE_2) {
		printf("<- s3_disconnect\n");
	}
}


/* ---------------------------------------------------------------------------
 * direct support for the IOR S3 interface
 * ---------------------------------------------------------------------------
 */

/*
 * One doesn't "open" an object, in REST semantics.  All we really care
 * about is whether caller expects the object to have zero-size, when we
 * return.  If so, we have to delete it, then recreate it empty.
 *
 * NOTE: Similarly, there's no file-descriptor to return.  On the other
 *       hand, we keep needing the file *NAME*.  Therefore, we will return
 *       the file-name, and let IOR pass it around to our functions, in
 *       place of its usual file-descriptor argument.
 *
 * ISSUE: If the object is going to receive "appends" (supported in EMC S3
 *       extensions), the object has to exist before the first append
 *       operation.  On the other hand, There appears to be a bug in the
 *       EMC implementation, such that if an object ever receives appends,
 *       and then is deleted, and then recreated, the recreated object will
 *       always return "500 Server Error" on GET (whether it has been
 *       appended or not).
 *
 *       Therefore, a safer thing to do here is write zero-length contents,
 *       instead of deleting.
 */

static
void *
S3_Create_Or_Open(char*         testFileName,
						IOR_param_t*  param,
						unsigned char createFile ) {

	if (param->verbose >= VERBOSE_2) {
		printf("-> S3_Create_Or_Open\n");
	}

	/* initialize curl, if needed */
	s3_connect( param );

	/* Check for unsupported flags */
	if ( param->openFlags & IOR_EXCL ) {
		fprintf( stdout, "Opening in Exclusive mode is not implemented in S3\n" );
	}
	if ( param->useO_DIRECT == TRUE ) {
		fprintf( stdout, "Direct I/O mode is not implemented in S3\n" );
	}


	/* check whether object needs reset to zero-length */
	int needs_reset = 0;
	if ( param->openFlags & IOR_TRUNC )
		needs_reset = 1;
	else if (createFile) {
		AWS4C_CHECK( s3_head(param->io_buf, testFileName) );
		if ( ! AWS4C_OK(param->io_buf) )
			needs_reset = 1;
	}


	if ( param->open == WRITE ) {

		/* initializations for N:N writes */
		if ( param->filePerProc ) {

			/* maybe reset to zero-length */
			if (needs_reset) {
				aws_iobuf_reset(param->io_buf);
				AWS4C_CHECK( s3_put(param->io_buf, testFileName) );
			}

			// MPI_CHECK(MPI_Barrier(param->testComm), "barrier error");
		}


		/* initializations for N:1 writes */
		else {

			/* rank0 initiates multi-part upload. The response from the server
				includes an "uploadId", which must be used by all ranks, when
				uploading parts. */
			if (rank == 0) {

				// rank0 handles truncate
				if ( needs_reset) { 
					aws_iobuf_reset(param->io_buf);
					AWS4C_CHECK( s3_put(param->io_buf, testFileName) );
				}

				// POST request with URL+"?uploads" initiates multi-part upload
				snprintf(buff, BUFF_SIZE, "%s?uploads", testFileName);
				IOBuf* response = aws_iobuf_new();
				AWS4C_CHECK( s3_post2(param->io_buf, buff, NULL, response) );

				// parse XML returned from server, into a tree structure
				aws_iobuf_realloc(response);
				xmlDocPtr doc = xmlReadMemory(response->first->buf,
														response->first->len,
														NULL, NULL, 0);
				if (doc == NULL)
					ERR_SIMPLE("Rank0 Failed to find POST response\n");

				// navigate parsed XML-tree to find UploadId
				xmlNode* root_element = xmlDocGetRootElement(doc);
				const char* upload_id = find_element_named(root_element, (char*)"UploadId");
				if (! upload_id)
					ERR_SIMPLE("couldn't find 'UploadId' in returned XML\n");

				if (param->verbose >= VERBOSE_4)
					printf("got UploadId = '%s'\n", upload_id);

				const size_t upload_id_len = strlen(upload_id);
				if (upload_id_len > MAX_UPLOAD_ID_SIZE) {
					snprintf(buff, BUFF_SIZE,
								"UploadId length %d exceeds expected max (%d)",
								upload_id_len, MAX_UPLOAD_ID_SIZE);
					ERR_SIMPLE(buff);
				}

				// save the UploadId we found
				memcpy(param->UploadId, upload_id, upload_id_len);
				param->UploadId[upload_id_len] = 0;

				// free storage for parsed XML tree
				xmlFreeDoc(doc);
				aws_iobuf_free(response);

				// share UploadId across all ranks
				MPI_Bcast(param->UploadId, MAX_UPLOAD_ID_SIZE, MPI_BYTE, 0, param->testComm);
			}
			else
				// recv UploadID from Rank 0
				MPI_Bcast(param->UploadId, MAX_UPLOAD_ID_SIZE, MPI_BYTE, 0, param->testComm);
		}
	}


	if (param->verbose >= VERBOSE_2) {
		printf("<- S3_Create_Or_Open\n");
	}
	return ((void *) testFileName );
}


static
void *
S3_Create( char *testFileName, IOR_param_t * param ) {
	if (param->verbose >= VERBOSE_2) {
		printf("-> S3_Create\n");
	}

	if (param->verbose >= VERBOSE_2) {
		printf("<- S3_Create\n");
	}
	return S3_Create_Or_Open( testFileName, param, TRUE );
}

static
void *
S3_Open( char *testFileName, IOR_param_t * param ) {
	if (param->verbose >= VERBOSE_2) {
		printf("-> S3_Open\n");
	}

	if ( param->openFlags & IOR_CREAT ) {
		if (param->verbose >= VERBOSE_2) {
			printf("<- S3_Open( ... TRUE)\n");
		}
		return S3_Create_Or_Open( testFileName, param, TRUE );
	}
	else {
		if (param->verbose >= VERBOSE_2) {
			printf("<- S3_Open( ... FALSE)\n");
		}
		return S3_Create_Or_Open( testFileName, param, FALSE );
	}
}

/*
 * transfer (more) data to an object.  <file> is just the obj name.
 *
 * For N:1, param->offset is understood as offset for a given client to
 * write into the "file".  This translates to a byte-range in the HTTP
 * request.
 *
 * Each write-request returns an ETag which is a hash of the data.  (The
 * ETag could also be computed directly, if we wanted.)  We must save the
 * etags for later use by S3_close().
 *
 * WARNING: "Pure" S3 doesn't allow byte-ranges for writes to an object.
 *      Thus, you also can not append to an object.  In the context of IOR,
 *      this causes objects to have only the size of the most-recent write.
 *      Thus, If the IOR "transfer-size" is different from the IOR
 *      "block-size", the files will be smaller than the amount of data
 *      that was written to them.
 *
 *      EMC does support "append" to an object.  In order to allow this,
 *      you must enable the EMC-extensions in the aws4c library, by calling
 *      s3_set_emc_compatibility() with a non-zero argument.
 *
 * NOTE: I don't think REST allows us to read/write an amount other than
 *       the size we request.  Maybe our callback-handlers (above) could
 *       tell us?  For now, this is assuming we only have to send one
 *       request, to transfer any amount of data.  (But see above, re EMC
 *       support for "append".)
 */

static
IOR_offset_t
S3_Xfer(int          access,
        void*        file,
        IOR_size_t*  buffer,
        IOR_offset_t length,
        IOR_param_t* param) {

	if (param->verbose >= VERBOSE_2) {
		printf("-> S3_Xfer(acc:%d, target:%s, buf:0x%llx, len:%llu, 0x%llx)\n",
				 access, (char*)file, buffer, length, param);
	}

	char*      fname = (char*)file; /* see NOTE above S3_Create_Or_Open() */
	size_t     remaining = (size_t)length;
	char*      data_ptr = (char *)buffer;
	off_t      offset = param->offset;


	if (access == WRITE) {	/* WRITE */

		if (verbose >= VERBOSE_4) {
			fprintf( stdout, "task %d writing to offset %lld\n",
						rank,
						param->offset + length - remaining);
		}


		if (param->filePerProc) { // N:N

			// DEBUGGING: can we use the new emc_put_append() to append to an object?
			s3_enable_EMC_extensions(1);
			s3_set_byte_range(-1,-1); // produces header "Range: bytes=-1-"

			// For performance, we append <data_ptr> directly into the linked list
			// of data in param->io_buf.  We are "appending" rather than
			// "extending", so the added buffer is seen as written data, rather
			// than empty storage.

			aws_iobuf_reset(param->io_buf);
			aws_iobuf_append_static(param->io_buf, data_ptr, remaining);
			AWS4C_CHECK( s3_put(param->io_buf, file) );

			// drop ptrs to <data_ptr>, in param->io_buf 
			aws_iobuf_reset(param->io_buf);
		}
		else {                    // N:1

			// Ordering of the part-numbers imposes a global ordering on
			// the components of the final object.  param->part_number
			// is incremented by 1 per write, on each rank.  This lets us
			// use it to compute a global part-numbering.
			//
			// NOTE: 's3curl.pl --debug' shows StringToSign having partNumber
			//       first, even if I put uploadId first in the URL.  Maybe
			//       that's what the server will do.  GetStringToSign() in
			//       aws4c is not clever about this, so we spoon-feed args in
			//       the proper order.

			size_t part_number = (param->part_number++ * numTasksWorld) + rank;
			snprintf(buff, BUFF_SIZE,
						"%s?partNumber=%d&uploadId=%s",
						fname, part_number, param->UploadId);

			// For performance, we append <data_ptr> directly into the linked list
			// of data in param->io_buf.  We are "appending" rather than
			// "extending", so the added buffer is seen as written data, rather
			// than empty storage.
			//
			// aws4c parses some header-fields automatically for us (into members
			// of the IOBuf).  After s3_put2(), we can just read the etag from
			// param->io_buf->eTag.  The server actually returns literal
			// quote-marks, at both ends of the string.

			aws_iobuf_reset(param->io_buf);
			aws_iobuf_append_static(param->io_buf, data_ptr, remaining);
			AWS4C_CHECK( s3_put(param->io_buf, buff) );

			if (verbose >= VERBOSE_4) {
				printf("rank %d: read ETag = '%s'\n", rank, param->io_buf->eTag);
				if (strlen(param->io_buf->eTag) != ETAG_SIZE+2) { /* quotes at both ends */
					fprintf(stderr, "Rank %d: ERROR: expected ETag to be %d hex digits\n",
							  rank, ETAG_SIZE);
					exit(1);
				}
			}

			// save the eTag for later
			//
			//		memcpy(etag, param->io_buf->eTag +1, strlen(param->io_buf->eTag) -2);
			//		etag[ETAG_SIZE] = 0;
			aws_iobuf_append(param->etags,
								  param->io_buf->eTag +1,
								  strlen(param->io_buf->eTag) -2);
			// DEBUGGING
			if (verbose >= VERBOSE_4) {
				printf("rank %d: part %d = ETag %s\n", rank, part_number, param->io_buf->eTag);
			}

			// drop ptrs to <data_ptr>, in param->io_buf 
			aws_iobuf_reset(param->io_buf);
		}


		if ( param->fsyncPerWrite == TRUE ) {
			WARN("S3 doesn't support 'fsync'" ); /* does it? */
		}

	}
	else {				/* READ or CHECK */

		if (verbose >= VERBOSE_4) {
			fprintf( stdout, "task %d reading from offset %lld\n",
						rank,
						param->offset + length - remaining );
		}

		// read specific byte-range from the object
		s3_set_byte_range(offset, remaining);

		// For performance, we append <data_ptr> directly into the linked
		// list of data in param->io_buf.  In this case (i.e. reading),
		// we're "extending" rather than "appending".  That means the
		// buffer represents empty storage, which will be filled by the
		// libcurl writefunction, invoked via aws4c.

		aws_iobuf_reset(param->io_buf);
		aws_iobuf_extend_static(param->io_buf, data_ptr, remaining);
		AWS4C_CHECK( s3_get(param->io_buf, file) );

		// drop ptrs to <data_ptr>, in param->io_buf 
		aws_iobuf_reset(param->io_buf);
	}


	if (param->verbose >= VERBOSE_2) {
		printf("<- S3_Xfer\n");
	}
	return ( length );
}

/*
 * Does this even mean anything, for HTTP/S3 ?
 *
 * I believe all interactions with the server are considered complete at
 * the time we get a response, e.g. from s3_put().  Therefore, fsync is
 * kind of meaningless, for REST/S3.
 *
 * In future, we could extend our interface so as to allow a non-blocking
 * semantics, for example with the libcurl "multi" interface, and/or by
 * adding threaded callback handlers to obj_put().  *IF* we do that, *THEN*
 * we should revisit 'fsync'.
 *
 * Another special case is multi-part upload, where many parallel clients
 * may be writing to the same "file".  (It looks like param->filePerProc
 * would be the flag to check, for this.)  Maybe when you called 'fsync',
 * you meant that you wanted *all* the clients to be complete?  That's not
 * really what fsync would do.  In the N:1 case, this is accomplished by
 * S3_Close().  If you really wanted this behavior from S3_Fsync, we could
 * have S3_Fsync call S3_close.
 *
 * As explained above, we may eventually want to consider the following:
 *
 *      (1) thread interaction with any handlers that are doing ongoing
 *      interactions with the socket, to make sure they have finished all
 *      actions and gotten responses.
 *
 *      (2) MPI barrier for all clients involved in a multi-part upload.
 *      Presumably, for IOR, when we are doing N:1, all clients are
 *      involved in that transfer, so this would amount to a barrier on
 *      MPI_COMM_WORLD.
 */

static
void
S3_Fsync( void *fd, IOR_param_t * param ) {
	if (param->verbose >= VERBOSE_2) {
		printf("-> S3_Fsync  [no-op]\n");
	}

	if (param->verbose >= VERBOSE_2) {
		printf("<- S3_Fsync\n");
	}
}


/*
 * It seems the only kind of "close" that ever needs doing for S3 is in the
 * case of multi-part upload (i.e. N:1).  In this case, all the parties to
 * the upload must provide their ETags to a single party (e.g. rank 0 in an
 * MPI job).  Then the rank doing the closing can generate XML and complete
 * the upload.
 *
 * ISSUE: The S3 spec says that a multi-part upload can have at most 10,000
 *        parts.  Does EMC allow more than this?  (NOTE the spec also says
 *        parts must be at leaast 5MB, but EMC definitely allows smaller
 *        parts than that.)
 *
 * ISSUE: All Etags must be sent from a single rank, in a single
 *        transaction.  If the issue above (regarding 10k Etags) is
 *        resolved by a discovery that EMC supports more than 10k ETags,
 *        then, for large-enough files (or small-enough transfer-sizes) an
 *        N:1 write may generate more ETags than the single closing rank
 *        can hold in memory.  In this case, there are several options,
 *        outlined
 *
 *

 * See S3_Fsync() for some possible considerations.
 */

static
void
S3_Close( void *fd, IOR_param_t * param ) {

	if (param->verbose >= VERBOSE_2) {
		printf("-> S3_Close\n");
	}
	char* fname = (char*)fd; /* see NOTE above S3_Create_Or_Open() */

	if (param->open == WRITE) {

		// closing N:1 write
		if (!param->filePerProc) {

			MPI_Datatype mpi_size_t;
			if (sizeof(size_t) == sizeof(int))
				mpi_size_t = MPI_INT;
			else if (sizeof(size_t) == sizeof(long))
				mpi_size_t = MPI_LONG;
			else
				mpi_size_t = MPI_LONG_LONG;

			// Everybody should have the same number of ETags (?)
			size_t etag_data_size = param->etags->write_count; /* size of local ETag data */
			size_t etag_count = etag_data_size / ETAG_SIZE;		/* number of local etags */
			size_t etag_count_max = 0;									/* highest number on any proc */

			MPI_Allreduce(&etag_count, &etag_count_max,
				           1, mpi_size_t, MPI_MAX, param->testComm);
			if (etag_count != etag_count_max) {
				printf("Rank %d: etag count mismatch: max:%d, mine:%d\n",
				       rank, etag_count_max, etag_count);
				MPI_Abort(param->testComm, 1);
			}

			// collect ETag data at Rank0
			aws_iobuf_realloc(param->etags);                   /* force single contiguous buffer */
			char* etag_data = param->etags->first->buf;			/* ptr to contiguous data */

			if (rank != 0) {
				MPI_Gather(etag_data, etag_data_size, MPI_BYTE,
				           NULL,      etag_data_size, MPI_BYTE, 0, MPI_COMM_WORLD);
			}
			else {
				char* etag_ptr;
				int   rnk;
				int   i;

				char* etag_vec = (char*)malloc((numTasksWorld * etag_data_size) +1);
				if (! etag_vec) {
					fprintf(stderr, "rank 0 failed to malloc %d bytes\n",
				           numTasksWorld * etag_data_size);
					MPI_Abort(param->testComm, 1);
				}
				MPI_Gather(etag_data, etag_data_size, MPI_BYTE,
				           etag_vec,  etag_data_size, MPI_BYTE, 0, MPI_COMM_WORLD);

				// --- debugging: show the gathered etag data
				//     (This shows the raw concatenated etag-data from each node.)
				if (param->verbose >= VERBOSE_4) {
					printf("rank 0: gathered %d etags from all ranks:\n", etag_count);
					etag_ptr=etag_vec;
					for (rnk=0; rnk<numTasksWorld; ++rnk) {
						printf("\t[%d]: '", rnk);

						int ii;
						for (ii=0; ii<etag_data_size; ++ii)	/* NOT null-terminated! */
							printf("%c", etag_ptr[ii]);

						printf("'\n");
						etag_ptr += etag_data_size;
					}
				}

				// --- create XML containing ETags in an IOBuf for "close" request
				IOBuf* xml = aws_iobuf_new();
				aws_iobuf_growth_size(xml, 1024 * 8);

				// write XML header ...
				aws_iobuf_append_str(xml, "<CompleteMultipartUpload>\n");

				// add XML for *all* the parts.  The XML must be ordered by
				// part-number.  Each rank wrote <etag_count> parts.  The etags
				// for each rank are staored as a continguous block of text, with
				// the blocks stored in rank order in etag_vec.  We must therefore
				// access them in the worst possible way, regarding locality.
				//
				// NOTE: If we knew ahead of time how many parts each rank was
				//       going to write, we could assign part-number ranges, per
				//       rank, and then have nice locality here.
				//
				//       Alternatively, we could have everyone format their own
				//       XML text and send that, instead of just the tags.  This
				//       would increase the amount of data being sent, but would
				//       reduce the work for rank0 to format everything.
			
				int part = 0;
				for (i=0; i<etag_count; ++i) {
					etag_ptr=etag_vec + (i * ETAG_SIZE);

					for (rnk=0; rnk<numTasksWorld; ++rnk) {

						// etags were saved as contiguous text.  Extract the next one.
						char etag[ETAG_SIZE +1];
						memcpy(etag, etag_ptr, ETAG_SIZE);
						etag[ETAG_SIZE] = 0;

						// write XML for next part, with Etag ...
						snprintf(buff, BUFF_SIZE,
						         "  <Part>\n"
						         "    <PartNumber>%d</PartNumber>\n"
						         "    <ETag>%s</ETag>\n"
						         "  </Part>\n",
						         part, etag);

						aws_iobuf_append_str(xml, buff);

						etag_ptr += etag_data_size;
						++ part;
					}
				}

				// write XML tail ...
				aws_iobuf_append_str(xml, "</CompleteMultipartUpload>\n");

				// DEBUGGING: show the XML we constructed
				if (param->verbose >= VERBOSE_4)
					debug_iobuf(xml, 1, 1);

				// --- POST our XML to the server.
				snprintf(buff, BUFF_SIZE,
				         "%s?uploadId=%s",
				         fname, param->UploadId);

#if 1
				AWS4C_CHECK   ( s3_post(xml, buff) );
				AWS4C_CHECK_OK( xml );
#else
				IOBuf* response = aws_iobuf_new();
				aws_iobuf_reset(response);
				AWS4C_CHECK( s3_post2(xml, buff, NULL, response) );
				if (! AWS4C_OK(param->io_buf) ) {
					fprintf(stderr, "rank %d: POST '%s' failed: %s\n",
					        rank, buff, param->io_buf->result);

					int sz;
					for (sz = aws_iobuf_getline(response, buff, BUFF_SIZE);
					     sz;
					     sz = aws_iobuf_getline(response, buff, BUFF_SIZE)) {
						printf("-- %s\n", buff);
					}
					MPI_Abort(param->testComm, 1);
				}
				aws_iobuf_free(response);
#endif
				aws_iobuf_free(xml);
			}


			// Don't you non-zero ranks go trying to stat the N:1 file until
			// rank0 has finished the S3 multi-part finalize.  It will not appear
			// to exist, until then.
			MPI_CHECK(MPI_Barrier(param->testComm), "barrier error");
		}

		// After writing, reset the CURL connection, so that caches won't be
		// used for reads.
		aws_reset_connection();
	}


	if (param->verbose >= VERBOSE_2) {
		printf("<- S3_Close\n");
	}
}

/*
 * Delete an object through the S3 interface.
 */

static
void
S3_Delete( char *testFileName, IOR_param_t * param ) {

	if (param->verbose >= VERBOSE_2) {
		printf("-> S3_Delete(%s)\n", testFileName);
	}

	/* maybe initialize curl */
	s3_connect( param );

	AWS4C_CHECK( s3_delete(param->io_buf, testFileName) );

	if (param->verbose >= VERBOSE_2)
		printf("<- S3_Delete\n");
}

/*
 * Determine API version.
 */

static
void
S3_SetVersion( IOR_param_t * param ) {
	if (param->verbose >= VERBOSE_2) {
		printf("-> S3_SetVersion\n");
	}

	strcpy( param->apiVersion, param->api );

	if (param->verbose >= VERBOSE_2) {
		printf("<- S3_SetVersion\n");
	}
}

/*
 * HTTP HEAD returns meta-data for a "file".
 *
 * QUESTION: What should the <size> parameter be, on a HEAD request?  Does
 * it matter?  We don't know how much data they are going to send, but
 * obj_get_callback protects us from overruns.  Will someone complain if we
 * request more data than the header actually takes?
 */

static
IOR_offset_t
S3_GetFileSize(IOR_param_t * param,
			   MPI_Comm      testComm,
			   char *        testFileName) {

	if (param->verbose >= VERBOSE_2) {
		printf("-> S3_GetFileSize(%s)\n", testFileName);
	}

	IOR_offset_t aggFileSizeFromStat; /* i.e. "long long int" */
	IOR_offset_t tmpMin, tmpMax, tmpSum;


	/* make sure curl is connected, and inits are done */
	s3_connect( param );

	/* send HEAD request.  aws4c parses some headers into IOBuf arg. */
	AWS4C_CHECK( s3_head(param->io_buf, testFileName) );
	if ( ! AWS4C_OK(param->io_buf) ) {
		fprintf(stderr, "rank %d: couldn't stat '%s': %s\n",
				  rank, testFileName, param->io_buf->result);
		MPI_Abort(param->testComm, 1);
	}
	aggFileSizeFromStat = param->io_buf->contentLen;


	if ( param->filePerProc == TRUE ) {
		if (param->verbose >= VERBOSE_2) {
			printf("\tall-reduce (1)\n");
		}
		MPI_CHECK(MPI_Allreduce(&aggFileSizeFromStat,
		                        &tmpSum,             /* sum */
		                        1,
		                        MPI_LONG_LONG_INT,
		                        MPI_SUM,
		                        testComm ),
		          "cannot total data moved" );

		aggFileSizeFromStat = tmpSum;
	}
	else {
		if (param->verbose >= VERBOSE_2) {
			printf("\tall-reduce (2a)\n");
		}
		MPI_CHECK(MPI_Allreduce(&aggFileSizeFromStat,
		                        &tmpMin,             /* min */
		                        1,
		                        MPI_LONG_LONG_INT,
		                        MPI_MIN,
		                        testComm ),
		          "cannot total data moved" );

		if (param->verbose >= VERBOSE_2) {
			printf("\tall-reduce (2b)\n");
		}
		MPI_CHECK(MPI_Allreduce(&aggFileSizeFromStat,
		                        &tmpMax,             /* max */
		                        1,
		                        MPI_LONG_LONG_INT,
		                        MPI_MAX,
		                        testComm ),
		          "cannot total data moved" );

		if ( tmpMin != tmpMax ) {
			if ( rank == 0 ) {
				WARN( "inconsistent file size by different tasks" );
			}

			/* incorrect, but now consistent across tasks */
			aggFileSizeFromStat = tmpMin;
		}
	}

	if (param->verbose >= VERBOSE_2) {
		printf("<- S3_GetFileSize [%llu]\n", aggFileSizeFromStat);
	}
	return ( aggFileSizeFromStat );
}
