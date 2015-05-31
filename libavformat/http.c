/*
 * HTTP protocol for ffmpeg client
 * Copyright (c) 2009 Elan Feingold
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <curl/curl.h>

#include "libavutil/base64.h"
#include "libavutil/avstring.h"
#include "avformat.h"
#include "network.h"
#include "os_support.h"
#include "ring_buffer.c"
#include "http.h"

/* used for protocol handling */
#define BUFFER_SIZE   1024*1024
#define URL_SIZE      4096
#define HTTP_FALSE    0
#define HTTP_TRUE     1

//#define DEBUG 1

char* HTTP_USER_AGENT = 0;
char* HTTP_COOKIES = 0;

typedef struct 
{
    URLContext*    url_context;
    CURL*          curl_handle;
    CURLM*         curl_multi_handle;
    int64_t        file_size;
    OutRingBuffer* buffer;
    int64_t        off;
    
} HTTPContext;

static size_t write_simple_callback(char *buffer, size_t size, size_t nitems, void *userp)
{
	size_t realsize = size * nitems;
	
  OutRingBuffer *ringBuffer = (OutRingBuffer *)userp;
  rb_write(ringBuffer, buffer, realsize);

  return realsize;
}

char* PMS_IssueHttpRequest(const char* url, const char* verb, int timeout, int* httpCode)
{
  char* reply = 0;

  CURL* curl = curl_easy_init();
  if(curl) 
  {
    /* Set up a ring buffer */
    OutRingBuffer* buffer = 0;
    rb_init(&buffer, 1025);
    
    /* Build the URL */
    char curlError[CURL_ERROR_SIZE];

	  /* Verb */
	  if (strcmp(verb, "GET") != 0 && strcmp(verb, "POST") != 0)
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, verb);

		/* Set URL */
    curl_easy_setopt(curl, CURLOPT_URL, url);

    /* set timeout to 1 second, maybe we should even have less than this? */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);

    /* since FFMPEG can be multithreaded we don't want curl to send signals */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

		/* Don't write output to stdout */
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_simple_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buffer);

    /* PMS doesn't support IPv6, so let's only talk IPv4 */
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);

#ifdef CURLOPT_NOPROXY
    /* If the user have defined a proxy, let's disable it for this call */
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
#endif    

    /* store the Error */
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, &curlError);
    if (curl_easy_perform(curl) != 0) 
        fprintf(stderr, ">>>CURL ERROR: %s\n", curlError);

		/* Get return code */
		if (httpCode)
		  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, httpCode);

    curl_easy_cleanup(curl);
    
    int numBytes = rb_data_size(buffer);
    reply = av_malloc(numBytes+1);
    rb_read(buffer, reply, numBytes);
    reply[numBytes] = 0;
    
    rb_destroy(buffer);
	}

	return reply;
}

void PMS_Log(int level, const char* format, ...);
void PMS_Log(int level, const char* format, ...)
{
  // Format the mesage.
  char msg[2048];
  va_list va;
  va_start(va, format);
  vsnprintf(msg, 2048, format, va);
  va_end(va);

  CURL* curl = curl_easy_init();
  if(curl)
  {
    // Build the URL.
    char url[2096];
    char* encodedMsg = curl_easy_escape(curl, msg, 0);
    
    snprintf(url, 2096, "http://127.0.0.1:32400/log?level=%d&source=Transcoder&message=", level);
    av_strlcat(url, encodedMsg, 2096);
    
		// Issue the request.
		char* reply = PMS_IssueHttpRequest(url, "GET", 1, NULL);
    av_free(reply);
    
    // In addition, print to standard error.
    fprintf(stderr, msg);
    
    if (msg[strlen(msg)-1] != '\n')
      fprintf(stderr, "\n");
    fflush(stderr);

    curl_free(encodedMsg);
    curl_easy_cleanup(curl);
  }
}

void ff_http_set_headers(URLContext *h, const char *headers)
{
	av_log(NULL, AV_LOG_ERROR, "Can not handle headers correct (in http.c)...");
/*    HTTPContext *s = h->priv_data;
    int len = strlen(headers);
	
    if (len && strcmp("\r\n", headers + len - 2))
        av_log(NULL, AV_LOG_ERROR, "No trailing CRLF found in HTTP header.\n");
	
    av_strlcpy(s->headers, headers, sizeof(s->headers));*/
}

void ff_http_set_chunked_transfer_encoding(URLContext *h, int is_chunked)
{
	av_log(NULL, AV_LOG_ERROR, "Can not handle chunked size  (in http.c)...");
//    ((HTTPContext*)h->priv_data)->chunksize = is_chunked ? 0 : -1;
}

void ff_http_init_auth_state(URLContext *dest, const URLContext *src)
{
	av_log(NULL, AV_LOG_ERROR, "Can not handle auth state correct (in http.c)...");
/*    memcpy(&((HTTPContext*)dest->priv_data)->auth_state,
           &((HTTPContext*)src->priv_data)->auth_state, sizeof(HTTPAuthState));*/
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static int debug_callback(CURL* handle, curl_infotype info, char *output, size_t size, void *data)
{
#ifdef DEBUG
	if (info == CURLINFO_DATA_IN || info == CURLINFO_DATA_OUT)
		return 0;
	
	char *pOut = (char* )av_malloc(size + 1);
	strncpy(pOut, output, size);
	pOut[size] = '\0';
	fprintf(stderr, "[CURL] %s", pOut);
	av_free(pOut);
#endif
	
	return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static size_t write_callback(char *buffer, size_t size, size_t nitems, void *userp)
{
	HTTPContext* ctx = (HTTPContext* )userp;
	size_t bytes = size*nitems;
	
#ifdef DEBUG
	fprintf(stderr, "HTTP: write_callback called with %ld bytes (data available = %d, free space = %d)\n", 
			bytes, rb_data_size(ctx->buffer), rb_free(ctx->buffer));
#endif
	
	size_t bytes_written = rb_write(ctx->buffer, buffer, bytes);
	
#ifdef DEBUG
	fprintf(stderr, "HTTP: write_callback called with %ld bytes (wrote %ld bytes, data available = %d)\n", 
			bytes, bytes_written, rb_data_size(ctx->buffer));
#endif
	
	return bytes_written;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static size_t header_callback(void *buffer, size_t size, size_t nmemb, void *userp)
{
	HTTPContext* ctx = (HTTPContext* )userp;
	
	char* pOut = (char* )av_malloc(size * nmemb + 1);
	av_strlcpy(pOut, buffer, size*nmemb);
	pOut[size*nmemb] = '\0';
	
	if (strncmp(pOut, "Content-Length:", 15) == 0)
		ctx->file_size = atoll(pOut+15);
	
	if (strncmp(pOut, "Location:", 9) == 0)
	{
		char* location = pOut+10;
		if (strstr(location, "/:/webkit") != 0)
      //THIS NEEDS TO BE WRITTEN TO stdout
       // av_log(NULL, AV_LOG_INFO, "[PLEX-WKR] %s\n", location);
			fprintf(stdout, "[PLEX-WKR] %s\n", location);
      fflush(stdout);
	}
	
	av_free(pOut);
	
	return size*nmemb;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static int http_run(HTTPContext* ctx)
{
	struct timeval timeout;
	int rc, maxfd, num_running, ret;
	fd_set fdread, fdwrite, fdexcep;
	
	FD_ZERO(&fdread);
	FD_ZERO(&fdwrite);
	FD_ZERO(&fdexcep);
	
	timeout.tv_sec = 0;
	timeout.tv_usec = 0;
	
  long recTimeout = 0;
	if (curl_multi_timeout(ctx->curl_multi_handle, &recTimeout) != CURLM_OK)
	  return HTTP_FALSE;
	
	if (recTimeout >= 0)
	{
    timeout.tv_sec = recTimeout / 1000;
    timeout.tv_usec = (recTimeout % 1000) * 1000;
  }
  else
  {
    timeout.tv_usec = 1;
  }

	// Collect the set of file descriptors.
	if (curl_multi_fdset(ctx->curl_multi_handle, &fdread, &fdwrite, &fdexcep, &maxfd) != CURLM_OK)
		return HTTP_FALSE;
	
	// See if we need to wait.
	if (maxfd > 0)
	{
		rc = select(maxfd+1, &fdread, &fdwrite, &fdexcep, &timeout);
		if (rc == -1)
			return HTTP_FALSE;
	}
	
	// Perform the transfer.
	while ((ret=curl_multi_perform(ctx->curl_multi_handle, &num_running)) == CURLM_CALL_MULTI_PERFORM) 
		; // Twiddle thumbs.
	
	if (num_running == 0)
		return HTTP_FALSE;
	
	if (ret == CURLM_OK)
		return HTTP_TRUE;
	
	return HTTP_FALSE;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static void http_destroy_context(HTTPContext* ctx)
{
	curl_multi_remove_handle(ctx->curl_multi_handle, ctx->curl_handle);
	curl_easy_cleanup(ctx->curl_handle);
	curl_multi_cleanup(ctx->curl_multi_handle);
	
	rb_destroy(ctx->buffer);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static HTTPContext* http_create_context(URLContext* url_context, uint64_t offset)
{
	long code = 0;
	
	// Allocate the new context.
	HTTPContext* ctx = av_malloc(sizeof(HTTPContext));
	if (!ctx) return 0;
	memset(ctx, 0, sizeof(HTTPContext));
	ctx->file_size = -1;
	ctx->off = offset;
	ctx->curl_handle = curl_easy_init();
	
	rb_init(&ctx->buffer, BUFFER_SIZE);
	
	curl_easy_setopt(ctx->curl_handle, CURLOPT_VERBOSE, 1);
	curl_easy_setopt(ctx->curl_handle, CURLOPT_URL, url_context->filename);
	curl_easy_setopt(ctx->curl_handle, CURLOPT_RESUME_FROM_LARGE, offset);
	curl_easy_setopt(ctx->curl_handle, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    
	curl_easy_setopt(ctx->curl_handle, CURLOPT_DEBUGFUNCTION, debug_callback);
	curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEDATA, ctx);
	curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEHEADER, ctx);
	curl_easy_setopt(ctx->curl_handle, CURLOPT_HEADERFUNCTION, header_callback);
	curl_easy_setopt(ctx->curl_handle, CURLOPT_TRANSFERTEXT, 0);
	curl_easy_setopt(ctx->curl_handle, CURLOPT_HEADER, 0);
	
	curl_easy_setopt(ctx->curl_handle, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt(ctx->curl_handle, CURLOPT_MAXREDIRS, 8);
	
	curl_easy_setopt(ctx->curl_handle, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(ctx->curl_handle, CURLOPT_FAILONERROR, 1);
	
	curl_easy_setopt(ctx->curl_handle, CURLOPT_SSL_VERIFYPEER, 0);
	curl_easy_setopt(ctx->curl_handle, CURLOPT_SSL_VERIFYHOST, 0);
	
	curl_easy_setopt(ctx->curl_handle, CURLOPT_DNS_CACHE_TIMEOUT, 0);
	curl_easy_setopt(ctx->curl_handle, CURLOPT_CONNECTTIMEOUT, 20);
	
	//fprintf(stderr, "The user agent is: %s\n", HTTP_USER_AGENT);
	
	if (HTTP_USER_AGENT == 0)
		curl_easy_setopt(ctx->curl_handle, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows; U; Windows NT 5.1; fr; rv:1.9.2b4) Gecko/20091124 Firefox/3.6b4 (.NET CLR 3.5.30729)");
	else
		curl_easy_setopt(ctx->curl_handle, CURLOPT_USERAGENT, HTTP_USER_AGENT);
    
	if (HTTP_COOKIES != 0)
		curl_easy_setopt(ctx->curl_handle, CURLOPT_COOKIE, HTTP_COOKIES);
	
	ctx->curl_multi_handle = curl_multi_init();
	curl_multi_add_handle(ctx->curl_multi_handle, ctx->curl_handle);

        /* since FFMPEG can be multithreaded we don't want curl to send signals */
        curl_multi_setopt(ctx->curl_multi_handle, CURLOPT_NOSIGNAL, 1L);

        /* PMS doesn't support IPv6, so let's only talk IPv4 */
        curl_multi_setopt(ctx->curl_multi_handle, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);

#ifdef CURLOPT_NOPROXY
        /* If the user have defined a proxy, let's disable it for this call */
        curl_easy_setopt(ctx->curl_multi_handle, CURLOPT_NOPROXY, "*");
#endif

	// We start some action by calling perform right away.
	while (http_run(ctx) == HTTP_TRUE)
	{
		curl_easy_getinfo(ctx->curl_handle, CURLINFO_RESPONSE_CODE, &code);
		if (code != 0 && (code < 300 || code >= 400))
			break;
	}
	
	// If we still haven't gotten a code back, try again.
	if (code == 0)
	  curl_easy_getinfo(ctx->curl_handle, CURLINFO_RESPONSE_CODE, &code);
	
	if (code < 200 || code >= 400)
	{
		http_destroy_context(ctx);
		return 0;
	}
	
	return ctx;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static int http_open(URLContext *h, const char *uri, int flags)
{
	if (strstr(uri, "/video/amt"))
		h->is_streamed = 1;
	else 
		h->is_streamed = 0;
	
  //Frank: priv_data could contain information added while the 
  //       original URLContext was created. Overwriting it may 
  //       not be what we want here. 
  if (h->prot->priv_data_size){
    av_free(h->priv_data);
  }
  
  h->priv_data = http_create_context(h, 0);
	if (h->priv_data == 0)
		return -1;
	
	return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static int http_read(URLContext *h, uint8_t *buf, int size)
{
	HTTPContext *ctx = h->priv_data;
	
#ifdef DEBUG
	fprintf(stderr, "HTTP Read[%p]: Asked for %d bytes.\n", h, size);
#endif
	
	if (rb_data_size(ctx->buffer) >= size)
	{
#ifdef DEBUG
		fprintf(stderr, "HTTP Read[%p]: Returning bytes from buffer.\n", h);
#endif
		
		// We could satisfy out of the buffer.
		return rb_read(ctx->buffer, buf, size);
	}
	else
	{
		int ret = 0;
		int success = HTTP_TRUE;
		
		// Read some more.
		while (success == HTTP_TRUE && rb_data_size(ctx->buffer) < size)
		{
			success = http_run(ctx);
			
			// If we're running out of room, just return what we have.
			if (rb_free(ctx->buffer) < 16384)
        break;
		}
		
		ret = rb_read(ctx->buffer, buf, size);
		ctx->off += ret;
		
#ifdef DEBUG
		fprintf(stderr, "HTTP Read[%p]: Returning %d.\n", h, ret);
#endif
		
		return ret;
	}
	
	return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static int http_write(URLContext *h, uint8_t *buf, int size)
{
	return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static int http_close(URLContext *h)
{
	HTTPContext* ctx = h->priv_data;
	
#ifdef DEBUG
	fprintf(stderr, "HTTP closing connection.\n");
#endif
	
	http_destroy_context(ctx);
	av_free(ctx);
	
  h->priv_data = NULL;
  
	return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static int64_t http_seek(URLContext *h, int64_t off, int whence)
{
	HTTPContext* ctx = h->priv_data;
	HTTPContext* newCtx = 0;
	
#ifdef DEBUG
	fprintf(stderr, "HTTP Seek: current=%lld, offset = %lld bytes, whence = %d.\n", ctx->off, off, whence);
#endif
	
	// See if this is a special case for file size determination.
	if (whence == AVSEEK_SIZE && ctx->file_size > 0)
	{
#ifdef DEBUG
		fprintf(stderr, "HTTP Seek returning size of file: %lld.\n", ctx->file_size);
#endif
		
		return ctx->file_size;
	}
	
	int64_t the_offset = 0;
	if (whence == SEEK_END)
		the_offset = ctx->file_size - off;
	else if (whence == SEEK_CUR)
		the_offset = ctx->off + off;
	else if (whence == SEEK_SET)
		the_offset = off;
	
#ifdef DEBUG
    fprintf(stderr, "HTTP Seek computed absolute offset of: %lld out of %lld.\n", the_offset, ctx->file_size);
#endif
	
	// Special case for seeking right to the end.
	if (the_offset == ctx->file_size)
	{
#ifdef DEBUG
		fprintf(stderr, "HTTP Seek was just past end of file, treating it as a skip.\n");
#endif
		
		// Let it happen, and treat it as a no-op.
		return 0;
	}
	
	// See if it's valid.
	if (the_offset > ctx->file_size && ctx->file_size > 0)
	{
#ifdef DEBUG
		fprintf(stderr, "HTTP Seek to %lld is invalid.\n", the_offset);
#endif
		return -1;
	}
	
	// Try the seek.
	newCtx = http_create_context(h, the_offset);
	if (newCtx)
	{
		// Save the old file size.
		newCtx->file_size = ctx->file_size;
		
		// It worked! Keep the new context.
		http_destroy_context(h->priv_data);
		h->priv_data = newCtx;
		return off;
	}
	else
	{
#ifdef DEBUG
		fprintf(stderr, "HTTP Seek to %lld failed.\n", the_offset);
#endif
		
		// Didn't work.
		return -1;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static int http_get_file_handle(URLContext *h)
{
    return -1;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
URLProtocol ff_http_protocol = {
    "http",
    http_open,
    http_read,
    http_write,
    http_seek,
    http_close,
    .url_get_file_handle = http_get_file_handle,
    .priv_data_size = sizeof(HTTPContext)
};
