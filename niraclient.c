#include "niraclient.h"

#include "meowhash/meow_hash_x64_aesni.h"

#include <curl/curl.h>

#if LIBCURL_VERSION_NUM != NIRA_CURL_EXPECTED_VERSION
#define NIRA_CURL_VERSION_MESSAGE "This version of niraclient was tested with libcurl version " NIRA_CURL_EXPECTED_VERSION_STR " (statically linked), and you have version " LIBCURL_VERSION ". Use this version at your own risk!"
#pragma message(NIRA_CURL_VERSION_MESSAGE)
#elif !defined(CURL_STATICLIB)
#pragma message("Warning: niraclient-c has been tested with a static build of libcurl. Use a dynamically loaded libcurl at your own risk.")
#endif

#include "cjson/cJSON.h"
#include "cjson/cJSON.c"

#include "miniz/miniz.h"
#include "miniz/miniz.c"

#include "mappedfile/mappedfile.c"

#include <errno.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#define closesocket close
#endif

#if defined(_MSC_VER)
#include <windows.h>
#include <intrin.h>

#define atomic_fetch_add(p_a, inc) InterlockedExchangeAdd(p_a, inc)
#define atomic_fetch_add_64(p_a, inc) InterlockedExchangeAdd64(p_a, inc)
#define atomic_compare_exchange_strong(p_a, expected, desired) atomic_compare_exchange_strong_int((LONG *)p_a, (LONG *)expected, (LONG)desired)

static inline int atomic_compare_exchange_strong_int(int *obj, int *expected, int desired)
{
    int orig = *expected;
    *expected = InterlockedCompareExchange(obj, desired, orig);
    return *expected == orig;
}

static time_t impl_timespec2msec(const struct timespec *ts)
{
        return (ts->tv_sec * 1000U) + (ts->tv_nsec / 1000000L);
}

static int thrd_sleep(const struct timespec *time_point, struct timespec *remaining)
{
    (void)remaining;
    assert(time_point);
    assert(!remaining); /* not implemented */
    Sleep((DWORD)impl_timespec2msec(time_point));
    return 0;
}

#else // !defined(_MSC_VER) ...
#define atomic_fetch_add(p_a, inc) __atomic_fetch_add(p_a, inc, __ATOMIC_SEQ_CST)
#define atomic_fetch_add_64(p_a, inc) __atomic_fetch_add(p_a, inc, __ATOMIC_SEQ_CST)
#define atomic_compare_exchange_strong(p_a, expected, desired) __atomic_compare_exchange_n(p_a, expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

#include <x86intrin.h>

static int thrd_sleep(const struct timespec *time_point, struct timespec *remaining)
{
    assert(time_point != NULL);
    return nanosleep(time_point, remaining);
}
#endif

// Not for optimization, it's just nicer than bringing in libmath
inline float fsqrt(const float f)
{
    __m128 temp = _mm_set_ss(f);
    temp = _mm_sqrt_ss(temp);
    return _mm_cvtss_f32(temp);
}

const uint64_t NiraClientInitSentinel = 0xDEADBEEF;

#define NIRA_SET_ERR_MSG(niraClient, ...) snprintf(niraClient->errorMessage, sizeof(niraClient->errorMessage), ##__VA_ARGS__);
#define NIRA_SET_ERR_DETAIL_X(niraClient, ...) snprintf(niraClient->errorDetail, sizeof(niraClient->errorDetail), ##__VA_ARGS__);
#define NIRA_SET_ERR_DETAIL(niraClient, detail, ...) NIRA_SET_ERR_DETAIL_X(niraClient, "%s() [%s:%d] "detail, __FUNCTION__, __FILE__, __LINE__, ##__VA_ARGS__)

#define NIRA_UNSET_ERR_DETAIL(niraClient) niraClient->errorDetail[0] = 0

static uint32_t NiraSetError(NiraClient *_niraClient, NiraStatus newStatus)
{
    int32_t expectedStatus = 0;

    return atomic_compare_exchange_strong(&_niraClient->statusCode, &expectedStatus, newStatus);
}

#define NiraHasError(niraClient) (NIRA_SUCCESS != niraClient->statusCode)
#define NiraGetError(niraClient) niraClient->statusCode

#if defined(_MSC_VER) && !defined(__thread)
#define __thread  __declspec( thread )
#endif

#ifdef _OPENMP
extern int omp_get_thread_num();
#endif

int32_t g_niraPerformCurlGlobalCleanup = 0;
atomic_int g_niraClientCount;

typedef struct _NiraFilePartTask
{
    uint32_t fileIdx;
    uint32_t partIdx;
} NiraFilePartTask;

#if 1
const size_t UPLOAD_CHUNK_SIZE = 1024 * 1024 * 20;
#else
const size_t UPLOAD_CHUNK_SIZE = 1024 * 1024 * 1;
#endif

const size_t NIRL_CURL_UPLOAD_MAX_READ_SIZE = 512 * 1024;
static size_t niraRequestedUploadWriteBufferSize;

#if defined(_MSC_VER)
// On Windows, RtlGenRandom[1] is supposed to output cryptographically-secure random
// bytes[2]. Despite the warning in the docs, it will probably not be removed[3].
//
// [1]: https://docs.microsoft.com/en-us/windows/win32/api/ntsecapi/nf-ntsecapi-rtlgenrandom
// [2]: https://www.microsoft.com/security/blog/2019/11/25/going-in-depth-on-the-windows-10-random-number-generation-infrastructure/
// [3]: https://github.com/rust-random/rand/issues/111#issuecomment-316140155.
#define RtlGenRandom SystemFunction036
BOOLEAN NTAPI RtlGenRandom(PVOID RandomBuffer, ULONG RandomBufferLength);

int getentropy(void* buf, uint32_t len)
{
    if (len > 256) {
        errno = EIO;
        return -1;
    }

    if (!RtlGenRandom((PVOID)buf, (ULONG)len)) {
        errno = EIO;
        return -1;
    }

    return 0;
}
#else
#include <unistd.h> // getentropy()
#endif

const char* filenameExt(const char* _filePath)
{
  const char* bs = strrchr(_filePath, '\\');
  const char* fs = strrchr(_filePath, '/');
  const char* basename = (bs > fs ? bs : fs);

#if _WIN32
  const char* colon = strrchr(_filePath, ':');
  basename = basename > colon ? basename : colon;
#endif

  if (NULL != basename)
  {
    return basename+1;
  }

  return _filePath;
}

typedef unsigned char uuid_t2[16];

static const char *fmt_lower = "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x";

struct uuid {
    uint32_t time_low;
    uint16_t time_mid;
    uint16_t time_hi_and_version;
    uint16_t clock_seq;
    uint8_t  node[6];
};

void my_uuid_unpack(const uuid_t2 in, struct uuid *uu)
{
    const uint8_t* ptr = in;
    uint32_t tmp;

    tmp = *ptr++;
    tmp = (tmp << 8) | *ptr++;
    tmp = (tmp << 8) | *ptr++;
    tmp = (tmp << 8) | *ptr++;
    uu->time_low = tmp;

    tmp = *ptr++;
    tmp = (tmp << 8) | *ptr++;
    uu->time_mid = tmp;

    tmp = *ptr++;
    tmp = (tmp << 8) | *ptr++;
    uu->time_hi_and_version = tmp;

    tmp = *ptr++;
    tmp = (tmp << 8) | *ptr++;
    uu->clock_seq = tmp;

    memcpy(uu->node, ptr, 6);
}

void my_uuid_pack(const struct uuid *uu, uuid_t2 ptr)
{
    uint32_t tmp;
    unsigned char *out = ptr;

    tmp = uu->time_low;
    out[3] = (unsigned char) tmp;
    tmp >>= 8;
    out[2] = (unsigned char) tmp;
    tmp >>= 8;
    out[1] = (unsigned char) tmp;
    tmp >>= 8;
    out[0] = (unsigned char) tmp;

    tmp = uu->time_mid;
    out[5] = (unsigned char) tmp;
    tmp >>= 8;
    out[4] = (unsigned char) tmp;

    tmp = uu->time_hi_and_version;
    out[7] = (unsigned char) tmp;
    tmp >>= 8;
    out[6] = (unsigned char) tmp;

    tmp = uu->clock_seq;
    out[9] = (unsigned char) tmp;
    tmp >>= 8;
    out[8] = (unsigned char) tmp;

    memcpy(out+10, uu->node, 6);
}

static void my_uuid_unparse(const uuid_t2 uu, char *out)
{
    struct uuid uuid;

    my_uuid_unpack(uu, &uuid);

    sprintf(out, fmt_lower,
            uuid.time_low, uuid.time_mid, uuid.time_hi_and_version,
            uuid.clock_seq >> 8, uuid.clock_seq & 0xFF,
            uuid.node[0], uuid.node[1], uuid.node[2],
            uuid.node[3], uuid.node[4], uuid.node[5]);
}

int32_t uuidGenV4(char *_uuidStr)
{
    uuid_t2 buf;
    struct uuid uu;

    if (0 != getentropy(buf, sizeof(buf)))
    {
        return -1;
    }

    my_uuid_unpack(buf, &uu);

    uu.clock_seq = (uu.clock_seq & 0x3FFF) | 0x8000;
    uu.time_hi_and_version = (uu.time_hi_and_version & 0x0FFF)
        | 0x4000;

    uint32_t out[4];
    my_uuid_pack(&uu, (uint8_t *)out);

    my_uuid_unparse((unsigned char *)out, _uuidStr);

    return 0;
}

static size_t niraCurlWriteCallback(void *_srcBuffer, size_t _membsize, size_t _nmemb, void *userp)
{
    NiraService *niraService = (NiraService *) userp;

    size_t numBytesToWrite = _membsize * _nmemb;

    const char *dstBufEnd = niraService->responseBuf + sizeof(niraService->responseBuf);

    char *dst = niraService->responseBuf + niraService->responseLength;

    if ((dst + numBytesToWrite + 1) > dstBufEnd)
    {
        return CURLE_WRITE_ERROR;
    }

    memcpy(dst, _srcBuffer, numBytesToWrite);

    niraService->responseLength += numBytesToWrite;
    niraService->responseBuf[niraService->responseLength] = 0;

    return numBytesToWrite;
}

uint32_t isValidNiraClient(NiraClient *_niraClient)
{
    return (NULL != _niraClient && NiraClientInitSentinel == _niraClient->isInitialized);
}

NiraStatus niraServiceReset(NiraClient *_niraClient, NiraService *_niraService, struct curl_slist *_requestHeaders)
{
    curl_easy_reset(_niraService->curl);

    curl_easy_setopt(_niraService->curl, CURLOPT_ERRORBUFFER, _niraService->curlErrorDetail);
    //curl_easy_setopt(_niraService->curl, CURLOPT_FAILONERROR, 1);
    curl_easy_setopt(_niraService->curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);

    if (NULL != _requestHeaders)
    {
        curl_easy_setopt(_niraService->curl, CURLOPT_HTTPHEADER, _requestHeaders);
    }

    _niraService->responseLength = 0;

    return NIRA_SUCCESS;
}

struct nirl_curl_mimestream {
    NiraClient *niraClient;
    const char *filename;
    const char *buffer;
    curl_off_t size;
    curl_off_t position;
    z_stream zstream;
    size_t compressedSize;
    uint32_t useCompression;
};

size_t nirl_mime_read_callback(char *writeBuffer, size_t size, size_t nitems, void *arg)
{
    struct nirl_curl_mimestream *p = (struct nirl_curl_mimestream *) arg;

    size_t bytesRemainingInFile = p->size - p->position;

    if (bytesRemainingInFile == 0)
    {
        //fprintf(stderr, "finishing file %s\n", p->filename);
        return 0;
    }

    size_t maxBytesToWrite = nitems * size;

    if (0 == p->useCompression)
    {
        if (maxBytesToWrite > bytesRemainingInFile)
        {
            maxBytesToWrite = bytesRemainingInFile;
        }

        const char *readBuffer = p->buffer + p->position;

        memcpy(writeBuffer, readBuffer, maxBytesToWrite);

        p->position += maxBytesToWrite;

        return maxBytesToWrite;
    }

    //fprintf(stderr, "read called %s %zd read buffer:%zd\n", p->filename, size * nitems, NIRL_CURL_UPLOAD_MAX_READ_SIZE);

    size_t maxBytesToRead = NIRL_CURL_UPLOAD_MAX_READ_SIZE;

    //if (maxBytesToWrite > niraRequestedUploadWriteBufferSize)
    //{
    //    fprintf(stderr, "Curl write buffer (%zd) smaller than requested (requested size: %zd) [This is not an error]\n", maxBytesToWrite, niraRequestedUploadWriteBufferSize);
    //}

    if (maxBytesToRead > bytesRemainingInFile)
    {
        maxBytesToRead = bytesRemainingInFile;
    }

    p->zstream.next_in = (const unsigned char *)(p->buffer + p->position);
    p->zstream.avail_in = maxBytesToRead;

    p->zstream.next_out = (unsigned char *)writeBuffer;
    p->zstream.avail_out = maxBytesToWrite;

    int32_t zstatus = deflate(&p->zstream, (bytesRemainingInFile == maxBytesToRead) ? Z_FINISH : Z_SYNC_FLUSH);

    if (Z_OK != zstatus && Z_STREAM_END != zstatus)
    {
        if (NiraSetError(p->niraClient, NIRA_ERROR_COMPRESSION_FAILURE))
        {
            NIRA_SET_ERR_DETAIL(p->niraClient, "deflate() error: %s %d", mz_error(zstatus), zstatus);
            NIRA_SET_ERR_MSG(p->niraClient, "Compression error while uploading %s", p->filename);
        }

        return CURL_READFUNC_ABORT;
    }

    p->position += (maxBytesToRead - p->zstream.avail_in);

    atomic_fetch_add_64(&p->niraClient->totalBytesCompleted, (maxBytesToRead - p->zstream.avail_in));

    //if (0 == p->zstream.avail_out)
    //{
    //    fprintf(stderr, "Ran out of space in output buffer (This is not an error)\n");
    //}

    size_t bytesWritten = maxBytesToWrite - p->zstream.avail_out;

    p->compressedSize += bytesWritten;

    //fprintf(stderr, "writing %s %zd bytes\n", p->filename, bytesWritten);

    return bytesWritten;
}

int nirl_mime_seek_callback(void *arg, curl_off_t offset, int origin)
{
    //fprintf(stderr, "seek called %zd %d\n", offset, origin);

    struct nirl_curl_mimestream *p = (struct nirl_curl_mimestream *) arg;

    // Because we're doing streaming compression,
    // we support seek back to 0, but nothing else.
    if (SEEK_SET == origin && 0 == offset)
    {
        p->position = 0;

        p->zstream.next_in = NULL;
        p->zstream.avail_in = 0;
        p->zstream.next_out = NULL;
        p->zstream.avail_out = 0;

        deflateReset(&p->zstream);

        return CURL_SEEKFUNC_OK;
    }

    return CURL_SEEKFUNC_FAIL;
}

NiraStatus performRequestWithRetries(NiraClient *_niraClient, NiraService *_niraService, uint8_t _doAutoAuthTokenRefresh, int64_t _retryTimeSeconds)
{
    const int64_t retries = (int64_t)(fsqrt((float)_retryTimeSeconds * NIRA_REQUEST_RETRY_BACKOFF_FACTOR * 2) / NIRA_REQUEST_RETRY_BACKOFF_FACTOR) + 1;
    //fprintf(stderr, "total retry time target: %d, backoff:%d, num retries: %zd\n", _retryTimeSeconds, NIRA_REQUEST_RETRY_BACKOFF_FACTOR, retries);

    char *url;
    curl_easy_getinfo(_niraService->curl, CURLINFO_EFFECTIVE_URL, &url);

    int64_t httpRespCode = 0;

    for (size_t tt = 0; tt <= retries; tt++)
    {
        if (_niraClient->abortAll)
        {
            break;
        }

        httpRespCode = 0;
        _niraService->responseLength = 0;
        _niraService->responseBuf[0] = 0;

        if (tt == 1)
        {
            atomic_fetch_add(&_niraClient->failingRequestCount, 1);
        }

        //fprintf(stderr, "performing request (%d / %d)\n", tt, retries);

        atomic_fetch_add(&_niraClient->activeRequestCount, 1);
        CURLcode curlCode = curl_easy_perform(_niraService->curl);
        atomic_fetch_add(&_niraClient->activeRequestCount, -1);

        if (CURLE_OK == curlCode)
        {
            curl_easy_getinfo(_niraService->curl, CURLINFO_RESPONSE_CODE, &httpRespCode);

            // 556 is a Nira-specific HTTP server status signifying that we should abort all operations.
            if (httpRespCode == 556)
            {
                break;
            }

            if (_doAutoAuthTokenRefresh && httpRespCode == 401)
            {
                // HTTP 401 means our token has expired.
                // If we successfully obtain a new one, retry the original request.
                if (NIRA_SUCCESS == niraAuthorize(_niraClient, 30))
                {
                    //fprintf(stderr, "got 401, retrying...\n");
                    struct timespec sleepTs = {1, 0};
                    thrd_sleep(&sleepTs, NULL);
                    continue;
                }

                // Failure to obtain a new token after retrying for 30 seconds is considered a hard error and we fail immediately.
                return NiraGetError(_niraClient);
            }

            if (httpRespCode >= 200 && httpRespCode <= 299)
            {
                if (tt > 0)
                {
                    atomic_fetch_add(&_niraClient->failingRequestCount, -1);
                }

                //fprintf(stderr, "got successful HTTP response code: %zd (%s)\n", httpRespCode, url);
                return NIRA_SUCCESS;
            }

            // Fall through to the sleep/retry
        }

        if (_niraClient->abortAll)
        {
            break;
        }

        int64_t waitTimeSec = tt * NIRA_REQUEST_RETRY_BACKOFF_FACTOR;
        struct timespec sleepTs = {waitTimeSec, 1e8};
        fprintf(stderr, "DEBUG: HTTP Failure: %zd (%.*s), retrying (%zd) after %zd.5 s...\n", httpRespCode, 256, _niraService->responseBuf, tt, waitTimeSec);
        thrd_sleep(&sleepTs, NULL);
    }

    if (_niraClient->abortAll)
    {
        if (NiraSetError(_niraClient, NIRA_ERROR_ABORTED_BY_USER))
        {
            NIRA_UNSET_ERR_DETAIL(_niraClient);
            NIRA_SET_ERR_MSG(_niraClient, "Asset upload aborted by user");
            return NiraGetError(_niraClient);
        }
    }

    if (NiraSetError(_niraClient, NIRA_ERROR_HTTP))
    {
        cJSON *errResp = cJSON_Parse(_niraService->responseBuf);
        cJSON *errMsgItem = errResp ? cJSON_GetObjectItem(errResp, "message") : NULL;

        if (errMsgItem)
        {
            NIRA_SET_ERR_DETAIL(_niraClient, "URL:%s Curl Error:%s", url, _niraService->curlErrorDetail);
            NIRA_SET_ERR_MSG(_niraClient, "HTTP error: %.*s [%zd], URL:%s", 256, errMsgItem->valuestring, httpRespCode, url);
        }
        else
        {
            NIRA_SET_ERR_DETAIL(_niraClient, "URL:%s Curl Error:%s", url, _niraService->curlErrorDetail);
            NIRA_SET_ERR_MSG(_niraClient, "HTTP error: %.*s [%zd], URL:%s", 256, _niraService->responseBuf, httpRespCode, url);
        }

        if (errResp)
        {
            cJSON_Delete(errResp);
        }
    }

    return NiraGetError(_niraClient);
}

NiraStatus makeFileUploadPartRequest(NiraClient *_niraClient, NiraService *_niraService, const char *_uploadServiceHost, const char *_filename, NiraAssetFile *_assetFile, const cJSON *_jsonPartBody, const char *_filePartBody, size_t _filePartSize)
{
    struct curl_slist uploadRequestHeaders[] = {
      { _niraClient->orgHeader       , &uploadRequestHeaders[1] },
      { _niraClient->userAgentHeader , &uploadRequestHeaders[2] },
      { "Transfer-Encoding: chunked" , &uploadRequestHeaders[3] }, // We don't know the size due to streaming compression, so this is required
      { "Content-Length:"            , &uploadRequestHeaders[4] }, // We don't know the size due to streaming compression, so this is required
      { _niraClient->apiTokenHeader  , NULL                     },
    };

    struct curl_slist jsonPartHeaders[] = {
      {"Content-Type: application/json", NULL}
    };

    struct curl_slist filePartHeaders[] = {
      {"Content-Type: application/octet-stream", NULL}
    };

    niraServiceReset(_niraClient, _niraService, uploadRequestHeaders);

    if (0 == niraRequestedUploadWriteBufferSize)
    {
        niraRequestedUploadWriteBufferSize = deflateBound(NULL, NIRL_CURL_UPLOAD_MAX_READ_SIZE) + (10*1024);
    }

    //fprintf(stderr, "NIRL_CURL_UPLOAD_MAX_READ_SIZE: %zd niraRequestedUploadWriteBufferSize: %zd\n", NIRL_CURL_UPLOAD_MAX_READ_SIZE, niraRequestedUploadWriteBufferSize);
    curl_easy_setopt(_niraService->curl, CURLOPT_UPLOAD_BUFFERSIZE, niraRequestedUploadWriteBufferSize);

    char uri[1024];
    snprintf(uri, sizeof(uri), "https://%s/file-upload-part", _uploadServiceHost);
    curl_easy_setopt(_niraService->curl, CURLOPT_URL, uri);

    curl_mime *mime = curl_mime_init(_niraService->curl);

    {
        curl_mimepart *jsonPart = curl_mime_addpart(mime);
        curl_mime_headers(jsonPart, jsonPartHeaders, /*takeOwnership*/0);

        char *jsonPartStr = cJSON_Print(_jsonPartBody);
        curl_mime_data(jsonPart, jsonPartStr, CURL_ZERO_TERMINATED);
        curl_mime_name(jsonPart, "params");
        free(jsonPartStr);
    }

    struct nirl_curl_mimestream curlMimeStream = {0};

    {
        curl_mimepart *filePart = curl_mime_addpart(mime);
        curl_mime_headers(filePart, filePartHeaders, /*takeOwnership*/0);

        curlMimeStream.filename = _filename;
        curlMimeStream.niraClient = _niraClient;
        curlMimeStream.buffer = _filePartBody;
        curlMimeStream.size = _filePartSize;
        curlMimeStream.position = 0;
        curlMimeStream.useCompression = _niraClient->useCompression;
        deflateInit(&curlMimeStream.zstream, MZ_BEST_SPEED);

        // Since deflate can sometimes produce data larger than the source,
        // we actually want to provide a size larger than the file part, here.
        // This ensures that libcurl keeps calling our curl_mime_data_cb even
        // if we've written out more bytes than the size of the file part.
        // In other words, we want to disable libcurl's behavior of
        // ceasing its calls to our curl_mime_data_cb function when bytesWritten == partsize.
        //
        // Note, curl_mime_data_cb can indicate that it's finished by returning 0.
        size_t sizeThatIsPuproselyLargerThanFilePart = deflateBound(NULL, _filePartSize) * 2;

        curl_mime_data_cb(filePart, sizeThatIsPuproselyLargerThanFilePart, nirl_mime_read_callback, nirl_mime_seek_callback, NULL, &curlMimeStream);
        curl_mime_name(filePart, "data");
        curl_mime_filename(filePart, _filename);
    }

    curl_easy_setopt(_niraService->curl, CURLOPT_MIMEPOST, mime);

    curl_easy_setopt(_niraService->curl, CURLOPT_WRITEFUNCTION, niraCurlWriteCallback);
    curl_easy_setopt(_niraService->curl, CURLOPT_WRITEDATA, (void *)_niraService);

    NiraStatus reqStatus = performRequestWithRetries(_niraClient, _niraService, true, NIRA_INTERNAL_REQUEST_RETRY_TIME_TOTAL);

    deflateEnd(&curlMimeStream.zstream);

    atomic_fetch_add_64(&_assetFile->compressedBytesUploaded, curlMimeStream.compressedSize);

    curl_mime_free(mime);

    if (NIRA_SUCCESS != reqStatus)
    {
        return NiraGetError(_niraClient);
    }

    //fprintf(stderr, "Got the http response: %s", responseBuffer);

    return NIRA_SUCCESS;
}

NiraStatus makeGetRequest(NiraClient *_niraClient, NiraService *_niraService, const char *uri, int64_t _retryTimeSeconds)
{
    struct curl_slist requestHeaders[] = {
      { _niraClient->orgHeader           , &requestHeaders[1] },
      { _niraClient->userAgentHeader     , &requestHeaders[2] },
      { _niraClient->apiTokenHeader      , NULL               },
    };

    niraServiceReset(_niraClient, _niraService, requestHeaders);

    curl_easy_setopt(_niraService->curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(_niraService->curl, CURLOPT_URL, uri);

    curl_easy_setopt(_niraService->curl, CURLOPT_WRITEFUNCTION, niraCurlWriteCallback);
    curl_easy_setopt(_niraService->curl, CURLOPT_WRITEDATA, (void *)_niraService);

    NiraStatus reqStatus = performRequestWithRetries(_niraClient, _niraService, /* _doAutoAuthTokenRefresh */ true, _retryTimeSeconds);
    if (NIRA_SUCCESS != reqStatus)
    {
        return NiraGetError(_niraClient);
    }

    //fprintf(stderr, "Got the http response: %d", reqStatus);

    return NIRA_SUCCESS;
}

NiraStatus makePostRequestJson(NiraClient *_niraClient, NiraService *_niraService, const char *uri, const cJSON *requestBody)
{
    struct curl_slist requestHeaders[] = {
      { _niraClient->orgHeader           , &requestHeaders[1] },
      { _niraClient->userAgentHeader     , &requestHeaders[2] },
      { _niraClient->apiTokenHeader      , &requestHeaders[3] },
      { "Content-Type: application/json" , NULL               },
    };

    niraServiceReset(_niraClient, _niraService, requestHeaders);

    char *requestBodyStr = cJSON_Print(requestBody);

    curl_easy_setopt(_niraService->curl, CURLOPT_POST, 1L);
    curl_easy_setopt(_niraService->curl, CURLOPT_POSTFIELDS, requestBodyStr);
    curl_easy_setopt(_niraService->curl, CURLOPT_URL, uri);

    curl_easy_setopt(_niraService->curl, CURLOPT_WRITEFUNCTION, niraCurlWriteCallback);
    curl_easy_setopt(_niraService->curl, CURLOPT_WRITEDATA, (void *)_niraService);

    NiraStatus reqStatus = performRequestWithRetries(_niraClient, _niraService, true, NIRA_INTERNAL_REQUEST_RETRY_TIME_TOTAL);
    free(requestBodyStr);

    if (NIRA_SUCCESS != reqStatus)
    {
        return NiraGetError(_niraClient);
    }

    //fprintf(stderr, "Got the http response: %s", responseBuffer);

    return NIRA_SUCCESS;
}

NiraStatus makePatchRequestJson(NiraClient *_niraClient, NiraService *_niraService, const char *uri, const cJSON *requestBody)
{
    struct curl_slist requestHeaders[] = {
      { _niraClient->orgHeader              , &requestHeaders[1] },
      { _niraClient->userAgentHeader        , &requestHeaders[2] },
      { _niraClient->apiTokenHeader         , &requestHeaders[3] },
      { "Content-Type: application/json"    , NULL               },
    };

    niraServiceReset(_niraClient, _niraService, requestHeaders);

    char *requestBodyStr = cJSON_Print(requestBody);

    curl_easy_setopt(_niraService->curl, CURLOPT_POST, 1L);
    curl_easy_setopt(_niraService->curl, CURLOPT_CUSTOMREQUEST, "PATCH");
    curl_easy_setopt(_niraService->curl, CURLOPT_POSTFIELDS, requestBodyStr);
    curl_easy_setopt(_niraService->curl, CURLOPT_URL, uri);

    curl_easy_setopt(_niraService->curl, CURLOPT_WRITEFUNCTION, niraCurlWriteCallback);
    curl_easy_setopt(_niraService->curl, CURLOPT_WRITEDATA, (void *)_niraService);

    NiraStatus reqStatus = performRequestWithRetries(_niraClient, _niraService, true, NIRA_INTERNAL_REQUEST_RETRY_TIME_TOTAL);
    free(requestBodyStr);

    if (NIRA_SUCCESS != reqStatus)
    {
        return NiraGetError(_niraClient);
    }

    //fprintf(stderr, "Got the http response: %s", responseBuffer);

    return NIRA_SUCCESS;
}


int32_t getFileHashAndSize(NiraClient *_niraClient, NiraAssetFile *_assetFile)
{
    #if _NIRACLIENT_WIN32_WCHAR_PATHS_AND_NAMES
    FILE *fh;
    _wfopen_s(&fh, _assetFile->pathW, L"rb");
    #else
    FILE *fh = fopen(_assetFile->path, "rb");
    #endif
    if (NULL == fh)
    {
        if (NiraSetError(_niraClient, NIRA_ERROR_FILE_IO))
        {
            NIRA_SET_ERR_DETAIL(_niraClient, "fopen: %s (%d)", strerror(errno), errno);
            NIRA_SET_ERR_MSG(_niraClient, "File does not exist or it's not readable: %s", _assetFile->path);
        }
        return NiraGetError(_niraClient);
    }
    fclose(fh);

    #if _NIRACLIENT_WIN32_WCHAR_PATHS_AND_NAMES
    char *mappedFile = map_file(_assetFile->pathW, /*out*/ &_assetFile->size);
    #else
    char *mappedFile = map_file(_assetFile->path, /*out*/ &_assetFile->size);
    #endif

    if (NULL == mappedFile)
    {
        // Windows probably doesn't set errno for its memory mapping related calls (CreateFileMappingA, etc)
        // If we want more specific error info on Windows, we'll need handling for it. The fopen above should
        // handle most error situations, though.
        if (NiraSetError(_niraClient, NIRA_ERROR_FILE_IO))
        {
            NIRA_SET_ERR_DETAIL(_niraClient, "map_file: %s (%d)", strerror(errno), errno);
            NIRA_SET_ERR_MSG(_niraClient, "Could not open or read file %s", _assetFile->path);
        }
        return NiraGetError(_niraClient);
    }

    meow_u128 meow = MeowHash(MeowDefaultSeed, _assetFile->size, mappedFile);

    uint64_t meow64[2];
    meow64[0] = MeowU64From(meow, 0);
    meow64[1] = MeowU64From(meow, 1);

    my_uuid_unparse((unsigned char*)meow64, _assetFile->hashStr);

    unmap_file(mappedFile, _assetFile->size);

    //fprintf(stdout, "GOT HASH for %s: %s\n", _filepath, _uuidStr);

    return 0;
}

NiraStatus niraInitCurlHandles(NiraClient *_niraClient)
{
    if (NULL == _niraClient->authservice)
    {
        _niraClient->authservice = calloc(1, sizeof(NiraService));

        _niraClient->authservice->curl = curl_easy_init();

        if (NULL == _niraClient->authservice->curl)
        {
            goto nira_curl_init_failure;
        }

#ifdef _WIN32
        struct curl_tlssessioninfo *curlSession;
        curl_easy_getinfo(_niraClient->authservice->curl, CURLINFO_TLS_SSL_PTR, &curlSession);

        if (CURLSSLBACKEND_SCHANNEL != curlSession->backend)
        {
            fprintf(stderr, "WARNING: On Windows, niraclient was tested using the Schannel SSL backend. Instead, you're using backend id: %d\n", curlSession->backend);
        }
#endif
    }

    if (NULL == _niraClient->webservice)
    {
        _niraClient->webservice = calloc(1, sizeof(NiraService));

        _niraClient->webservice->curl = curl_easy_init();

        if (NULL == _niraClient->webservice->curl)
        {
            goto nira_curl_init_failure;
        }
    }

    for (size_t tt = 0; tt < NIRA_MAX_UPLOAD_THREADS; tt++)
    {
        if (NULL == _niraClient->uploaders[tt])
        {
            _niraClient->uploaders[tt] = calloc(1, sizeof(NiraService));

            _niraClient->uploaders[tt]->curl = curl_easy_init();

            if (NULL == _niraClient->uploaders[tt]->curl)
            {
                goto nira_curl_init_failure;
            }
        }
    }

    return NIRA_SUCCESS;

    nira_curl_init_failure:
        if (NiraSetError(_niraClient, NIRA_ERROR_CURL_INIT_FAILURE))
        {
            NIRA_SET_ERR_DETAIL(_niraClient, "curl_easy_init() failure%s", "");
            NIRA_SET_ERR_MSG(_niraClient, "Error initializing HTTP request context");
        }

        return NiraGetError(_niraClient);
}

NiraStatus niraSetCredentials(NiraClient *_niraClient, const char *_apiKeyId, const char *_apiKeySecret)
{
    snprintf(_niraClient->apiKeyIdHeader, sizeof(_niraClient->apiKeyIdHeader), "x-api-key-id: %s", _apiKeyId);
    snprintf(_niraClient->apiKeySecretHeader, sizeof(_niraClient->apiKeySecretHeader), "x-api-key-secret: %s", _apiKeySecret);
    snprintf(_niraClient->apiTokenHeader, sizeof(_niraClient->apiTokenHeader), "x-api-token: invalid");

    return NIRA_SUCCESS;
}

NiraStatus _niraAuthorize(NiraClient *_niraClient, int64_t _retryTimeSeconds)
{
    static uint8_t once;

    if (!_niraClient->apiKeyIdHeader[0] || !_niraClient->apiKeySecretHeader[0])
    {
        if (NiraSetError(_niraClient, NIRA_ERROR_MISSING_CREDENTIALS))
        {
            NIRA_UNSET_ERR_DETAIL(_niraClient);
            NIRA_SET_ERR_MSG(_niraClient, "A key id and secret is required!");
        }

        return NiraGetError(_niraClient);
    }

    if (!isValidNiraClient(_niraClient))
    {
        return NIRA_ERROR_INVALID_NIRACLIENT;
    }

    if (NIRA_SUCCESS != niraInitCurlHandles(_niraClient))
    {
        return NiraGetError(_niraClient);
    }

    if (0 == _niraClient->apiKeyEndpoint[0])
    {
        niraServiceReset(_niraClient, _niraClient->authservice, NULL);
        curl_easy_setopt(_niraClient->authservice->curl, CURLOPT_URL, _niraClient->niraConfigEndpoint);

        curl_easy_setopt(_niraClient->authservice->curl, CURLOPT_WRITEFUNCTION, niraCurlWriteCallback);
        curl_easy_setopt(_niraClient->authservice->curl, CURLOPT_WRITEDATA, (void *)_niraClient->authservice);

        NiraStatus reqStatus = performRequestWithRetries(_niraClient, _niraClient->authservice, false, _retryTimeSeconds);

        if (NIRA_SUCCESS != reqStatus)
        {
            return NiraGetError(_niraClient);
        }

        cJSON *configResp = cJSON_Parse(_niraClient->authservice->responseBuf);

        if (!configResp)
        {
            if (NiraSetError(_niraClient, NIRA_ERROR_JSON_PARSE))
            {
                NIRA_SET_ERR_DETAIL(_niraClient, "Response: %.*s", 128, _niraClient->authservice->responseBuf);
                NIRA_SET_ERR_MSG(_niraClient, "Could not parse configuration response");
            }
            return NiraGetError(_niraClient);
        }

        cJSON *authServerUrlItem = cJSON_GetObjectItem(configResp, "authorizationUrl");
        if (!authServerUrlItem)
        {
            if (NiraSetError(_niraClient, NIRA_ERROR_JSON_MISSING_ATTRIBUTE))
            {
                NIRA_SET_ERR_DETAIL(_niraClient, "Response: %.*s", 128, _niraClient->authservice->responseBuf);
                NIRA_SET_ERR_MSG(_niraClient, "Missing authorizationUrl in configuration response");
            }

            cJSON_Delete(configResp);
            return NiraGetError(_niraClient);
        }

        const char *authServerUrl = authServerUrlItem->valuestring;

        snprintf(_niraClient->apiKeyEndpoint, sizeof(_niraClient->apiKeyEndpoint), "%s/%s", authServerUrl, "api-key-auth");

        cJSON_Delete(configResp);
    }

    struct curl_slist authRequestHeaders[] = {
      { _niraClient->orgHeader          , &authRequestHeaders[1] },
      { _niraClient->userAgentHeader    , &authRequestHeaders[2] },
      { _niraClient->apiKeyIdHeader     , &authRequestHeaders[3] },
      { _niraClient->apiKeySecretHeader , NULL                   },
    };

    niraServiceReset(_niraClient, _niraClient->authservice, authRequestHeaders);

    curl_easy_setopt(_niraClient->authservice->curl, CURLOPT_CUSTOMREQUEST, "POST");
    curl_easy_setopt(_niraClient->authservice->curl, CURLOPT_URL, _niraClient->apiKeyEndpoint);

    curl_easy_setopt(_niraClient->authservice->curl, CURLOPT_WRITEFUNCTION, niraCurlWriteCallback);
    curl_easy_setopt(_niraClient->authservice->curl, CURLOPT_WRITEDATA, (void *)_niraClient->authservice);

    NiraStatus reqStatus = performRequestWithRetries(_niraClient, _niraClient->authservice, false, _retryTimeSeconds);

    if (NIRA_SUCCESS != reqStatus)
    {
        return NiraGetError(_niraClient);
    }

    cJSON *authResp = cJSON_Parse(_niraClient->authservice->responseBuf);
    if (!authResp)
    {
        if (NiraSetError(_niraClient, NIRA_ERROR_JSON_PARSE))
        {
            NIRA_SET_ERR_DETAIL(_niraClient, "Response: %.*s", 128, _niraClient->authservice->responseBuf);
            NIRA_SET_ERR_MSG(_niraClient, "Could not parse authorization response");
        }
        return NiraGetError(_niraClient);
    }

    cJSON *tokenItem = cJSON_GetObjectItem(authResp, "token");
    if (!tokenItem)
    {
        if (NiraSetError(_niraClient, NIRA_ERROR_JSON_MISSING_ATTRIBUTE))
        {
            NIRA_SET_ERR_DETAIL(_niraClient, "Response: %.*s", 128, _niraClient->authservice->responseBuf);
            NIRA_SET_ERR_MSG(_niraClient, "Missing token in authorization response");
        }
        cJSON_Delete(authResp);
        return NiraGetError(_niraClient);
    }

    const char *token = tokenItem->valuestring;

    //cJSON *expiresItem = cJSON_GetObjectItem(authResp, "expires");
    //int64_t expires = tokenItem->valueint;

    snprintf(_niraClient->apiTokenHeader, sizeof(_niraClient->apiTokenHeader), "x-api-token: %s", token);

    cJSON_Delete(authResp);

    return NIRA_SUCCESS;
}

NiraStatus niraAuthorize(NiraClient *_niraClient, int64_t _retryTimeSeconds)
{
    {
        int32_t expectedValue = 0;
        while (!atomic_compare_exchange_strong(&_niraClient->isAuthorizing, &expectedValue, 1))
        {
            struct timespec sleepTs = {1, 0};
            thrd_sleep(&sleepTs, NULL);
        }
    }

    NiraStatus authStatus;

    // If a successful authorization occurred in the last 6 seconds,
    // we just assume another thread won a race against us to reauthorize
    // and we return success. This prevents having multiple threads
    // perform reauthorization around the same time as each other.
    if (_niraClient->lastSuccessfulAuthTime > (time(NULL) - 6))
    {
        authStatus = NIRA_SUCCESS;
    }
    else
    {
        authStatus = _niraAuthorize(_niraClient, _retryTimeSeconds);
        _niraClient->lastSuccessfulAuthTime = (NIRA_SUCCESS == authStatus) ? time(NULL) : 0;
    }

    {
        int32_t expectedValue = 1;
        if (!atomic_compare_exchange_strong(&_niraClient->isAuthorizing, &expectedValue, 0))
        {
            fprintf(stderr, "error condition!\n");
            _niraClient->isAuthorizing = 0;
        }
    }

    return authStatus;
}

const char *conflictResToStr(NiraAssetNameConflictResolution _conflictRes)
{
    switch (_conflictRes)
    {
        case NIRA_CONFLICTRES_CREATE_NEW_ASSET_WITH_NAME_POSTFIX:
            return "create_new_asset_with_name_postfix";

        case NIRA_CONFLICTRES_ADD_VERSION_AND_UPDATE_FILESET:
            return "add_version_and_update_fileset";

        case NIRA_CONFLICTRES_ADD_VERSION_AND_REPLACE_FILESET:
            return "add_version_and_replace_fileset";

        case NIRA_CONFLICTRES_RETURN_ERROR:
            return "return_error";

        case NIRA_CONFLICTRES_SERVER_DEFAULT:
        default:
            return NULL;
    }

    return NULL;
}

#if _NIRACLIENT_WIN32_WCHAR_PATHS_AND_NAMES
NiraStatus convertWideStringsToUtf8(NiraClient *_niraClient, NiraAssetFile *_files, size_t _fileCount, const wchar_t *_assetName, char **_assetNameOut)
{
    size_t strPoolBytesAvailable = 0;

    for (size_t ff = 0; ff < _fileCount; ff++)
    {
        NiraAssetFile *assetFile = &_files[ff];

        strPoolBytesAvailable += WideCharToMultiByte(CP_UTF8, 0, assetFile->pathW, -1, NULL, 0, NULL, NULL);
        //fprintf(stderr, "For string for FILE %zd, pool is now %zd\n", ff, strPoolBytesAvailable);

        if (assetFile->nameW)
        {
            strPoolBytesAvailable += WideCharToMultiByte(CP_UTF8, 0, assetFile->nameW, -1, NULL, 0, NULL, NULL);
            //fprintf(stderr, "For string for NAME %zd, pool is now %zd\n", ff, strPoolBytesAvailable);
        }
    }

    strPoolBytesAvailable += WideCharToMultiByte(CP_UTF8, 0, _assetName, -1, NULL, 0, NULL, NULL);
    //fprintf(stderr, "For string for ASSET NAME, pool is now %zd\n", strPoolBytesAvailable);

    _niraClient->stringPool = calloc(strPoolBytesAvailable, sizeof(char));

    char *strPoolPtr = _niraClient->stringPool;

    for (size_t ff = 0; ff < _fileCount; ff++)
    {
        NiraAssetFile *assetFile = &_files[ff];

        {
            size_t pathStrLen = WideCharToMultiByte(CP_UTF8, 0, assetFile->pathW, -1, strPoolPtr, strPoolBytesAvailable, NULL, NULL);
            assetFile->path = strPoolPtr;
            strPoolBytesAvailable -= pathStrLen;
            strPoolPtr += pathStrLen;
            //fprintf(stderr, "Converted string for FILE %zd, pool is now %zd, ptr is now %p, strPoolBytesAvailable is %zd, path string is %s\n", ff, strPoolBytesAvailable, strPoolPtr, strPoolBytesAvailable, assetFile->path);
        }

        if (assetFile->nameW)
        {
            size_t nameStrLen = WideCharToMultiByte(CP_UTF8, 0, assetFile->nameW, -1, strPoolPtr, strPoolBytesAvailable, NULL, NULL);
            assetFile->name = strPoolPtr;
            strPoolBytesAvailable -= nameStrLen;
            strPoolPtr += nameStrLen;
            //fprintf(stderr, "Converted string for NAME %zd, pool is now %zd, ptr is now %p, strPoolBytesAvailable is %zd, name string is %s\n", ff, strPoolBytesAvailable, strPoolPtr, strPoolBytesAvailable, assetFile->name);
        }
    }

    {
        size_t assetnameStrLen = WideCharToMultiByte(CP_UTF8, 0, _assetName, -1, strPoolPtr, strPoolBytesAvailable, NULL, NULL);
        *_assetNameOut = strPoolPtr;
        strPoolBytesAvailable -= assetnameStrLen;
        strPoolPtr += assetnameStrLen;

        //fprintf(stderr, "Converted string for ASSET NAME, pool is now %zd, ptr is now %p, strPoolBytesAvailable is %zd, asset name is %s\n", strPoolBytesAvailable, strPoolPtr, strPoolBytesAvailable, *_assetNameOut);
    }

    return NIRA_SUCCESS;
}
#endif

#if _NIRACLIENT_WIN32_WCHAR_PATHS_AND_NAMES
NiraStatus niraUploadAsset(NiraClient *_niraClient, NiraAssetFile *_files, size_t _fileCount, const wchar_t *_assetName, NiraAssetType _assetType)
#else
NiraStatus niraUploadAsset(NiraClient *_niraClient, NiraAssetFile *_files, size_t _fileCount, const char *_assetName, NiraAssetType _assetType)
#endif
{
    if (!isValidNiraClient(_niraClient))
    {
        return NIRA_ERROR_INVALID_NIRACLIENT;
    }

#if _NIRACLIENT_WIN32_WCHAR_PATHS_AND_NAMES
    char *assetName;
    convertWideStringsToUtf8(_niraClient, _files, _fileCount, _assetName, /*out*/ &assetName);
#else
    const char *assetName = _assetName;
#endif

    if (NIRA_SUCCESS != niraInitCurlHandles(_niraClient))
    {
        return NiraGetError(_niraClient);
    }

    _niraClient->totalFileSize = 0;
    _niraClient->totalFileCount = _fileCount;

    const char *assetType;
    switch (_assetType)
    {
        case NIRA_ASSET_TYPE_PBR:
            assetType = "default";
            break;

        case NIRA_ASSET_TYPE_SCULPT:
            assetType = "sculpt";
            break;

        case NIRA_ASSET_TYPE_VOLUMETRIC_VIDEO:
            assetType = "volumetric_video";
            break;

        case NIRA_ASSET_TYPE_PHOTOGRAMMETRY:
        default:
            assetType = "photogrammetry";
            break;
    }

    char jobUuidStr[40];

    if (0 != uuidGenV4(jobUuidStr))
    {
        if (NiraSetError(_niraClient, NIRA_ERROR_ENTROPY_FAILURE))
        {
            NIRA_UNSET_ERR_DETAIL(_niraClient);
            NIRA_SET_ERR_MSG(_niraClient, "Could not generate uuid");
        }

        return NiraGetError(_niraClient);
    }

    // First, inform the server that we're preparing to send it some files.
    uint32_t jobId;
    {
        cJSON *jobCreationRequest = cJSON_CreateObject();

        cJSON_AddStringToObject(jobCreationRequest, "status", "validating");

        cJSON_AddStringToObject(jobCreationRequest, "assettype", assetType);
        cJSON_AddStringToObject(jobCreationRequest, "batchId", jobUuidStr);
        cJSON_AddStringToObject(jobCreationRequest, "assetname", assetName);

        const char *nameConflictResolution = conflictResToStr(_niraClient->assetNameConflictResolution);

        if (nameConflictResolution)
        {
            cJSON_AddStringToObject(jobCreationRequest, "nameConflictResolution", nameConflictResolution);
        }

        if (0 != _niraClient->appName[0])
        {
            cJSON_AddStringToObject(jobCreationRequest, "dccname", _niraClient->appName);
        }

        if (0 != _niraClient->coordsys[0])
        {
            cJSON_AddStringToObject(jobCreationRequest, "coordsys", _niraClient->coordsys);
        }

        NiraStatus reqStatus = makePostRequestJson(_niraClient, _niraClient->webservice, _niraClient->jobsEndpoint, jobCreationRequest);
        cJSON_Delete(jobCreationRequest);

        if (NIRA_SUCCESS != reqStatus)
        {
            return NiraGetError(_niraClient);
        }

        cJSON *jobResp = cJSON_Parse(_niraClient->webservice->responseBuf);

        if (NULL == jobResp)
        {
            if (NiraSetError(_niraClient, NIRA_ERROR_JSON_PARSE))
            {
                NIRA_SET_ERR_DETAIL(_niraClient, "Response: %.*s", 128, _niraClient->webservice->responseBuf);
                NIRA_SET_ERR_MSG(_niraClient, "Could not parse upload job creation response");
            }
            return NiraGetError(_niraClient);
        }

        cJSON *jobIdItem = cJSON_GetObjectItem(jobResp, "id");
        if (!jobIdItem)
        {
            if (NiraSetError(_niraClient, NIRA_ERROR_JSON_MISSING_ATTRIBUTE))
            {
                NIRA_SET_ERR_DETAIL(_niraClient, "Response: %.*s", 128, _niraClient->webservice->responseBuf);
                NIRA_SET_ERR_MSG(_niraClient, "Missing id in upload job creation response");
            }
            cJSON_Delete(jobResp);
            return NiraGetError(_niraClient);
        }

        jobId = jobIdItem->valueint;

        cJSON *assetShortUuidItem = cJSON_GetObjectItem(jobResp, "assetShortUuid");
        if (!assetShortUuidItem)
        {
            if (NiraSetError(_niraClient, NIRA_ERROR_JSON_MISSING_ATTRIBUTE))
            {
                NIRA_SET_ERR_DETAIL(_niraClient, "Response: %.*s", 128, _niraClient->webservice->responseBuf);
                NIRA_SET_ERR_MSG(_niraClient, "Missing assetShortUuid in upload job creation response");
            }
            cJSON_Delete(jobResp);
            return NiraGetError(_niraClient);
        }

        snprintf(_niraClient->assetUrl, sizeof(_niraClient->assetUrl), "https://%s/a/%s", _niraClient->orgName, assetShortUuidItem->valuestring);

        cJSON_Delete(jobResp);
    }

    size_t totalFileSize = 0;

    // Get hashes and sizes of all files
    for (size_t ff = 0; ff < _fileCount; ff++)
    {
        NiraAssetFile *assetFile = &_files[ff];

        NiraStatus status = getFileHashAndSize(_niraClient, assetFile);
        if (NIRA_SUCCESS != status)
        {
            return status;
        }

        if (_niraClient->abortAll)
        {
            if (NiraSetError(_niraClient, NIRA_ERROR_ABORTED_BY_USER))
            {
                NIRA_UNSET_ERR_DETAIL(_niraClient);
                NIRA_SET_ERR_MSG(_niraClient, "Asset upload aborted by user");
                return NiraGetError(_niraClient);
            }
        }

        totalFileSize += assetFile->size;
    }

    // By setting _niraClient->totalFileSize to non-zero here, we signify to the caller
    // that it can start using the progress related variables (totalFileSize, totalBytesCompleted,
    // totalFilesCompleted, etc.) for a progress bar or progress display of some kind.
    _niraClient->totalFileSize = totalFileSize;

    const uint8_t forceFileUploads = NULL != getenv("NIRA_FORCE_FILE_UPLOADS");

    int32_t ff = 0;

    // Create file records for all files
    #pragma omp parallel num_threads(_niraClient->numUploadThreads)
    #pragma omp for schedule(static, 1)
    for (ff = 0; ff < _fileCount; ff++)
    {
        NiraAssetFile *assetFile = &_files[ff];

        const char *filename = assetFile->name ? assetFile->name : filenameExt(assetFile->path);

        if (0 != uuidGenV4(assetFile->uuidStr))
        {
            if (NiraSetError(_niraClient, NIRA_ERROR_ENTROPY_FAILURE))
            {
                NIRA_UNSET_ERR_DETAIL(_niraClient);
                NIRA_SET_ERR_MSG(_niraClient, "Could not generate uuid");
            }

            continue;
        }

        cJSON *fileRequestBody = cJSON_CreateObject();

        cJSON_AddStringToObject(fileRequestBody, "fileName", filename);

        // For image files (.jpg, .png, etc), it's best to provide either "photogrammetry_image"
        // (for source photos) or "image" (for textures).
        // If NULL, server will try to figure it out, but cannot always do so.
        switch (assetFile->type)
        {
            case NIRA_FILETYPE_TEXTURE:
            {
                cJSON_AddStringToObject(fileRequestBody, "type", "image");
            } break;

            case NIRA_FILETYPE_PHOTO:
            {
                cJSON_AddStringToObject(fileRequestBody, "type", "photogrammetry_image");
            } break;

            case NIRA_FILETYPE_MATERIAL:
            {
                cJSON_AddStringToObject(fileRequestBody, "type", "material");
            } break;

            default:
            {
                // Allow the server to figure out the others
            } break;
        }

        cJSON_AddStringToObject(fileRequestBody, "uuid", assetFile->uuidStr);
        cJSON_AddStringToObject(fileRequestBody, "meowhash", assetFile->hashStr);
        cJSON_AddNumberToObject(fileRequestBody, "filesize", (double)assetFile->size);

        cJSON_AddNumberToObject(fileRequestBody, "jobId", jobId);

        size_t threadIdx = 0;

#ifdef _OPENMP
        threadIdx = omp_get_thread_num();
#endif

        NiraStatus reqStatus = makePostRequestJson(_niraClient, _niraClient->uploaders[threadIdx], _niraClient->filesEndpoint, fileRequestBody);
        cJSON_Delete(fileRequestBody);

        if (NIRA_SUCCESS != reqStatus)
        {
            continue;
        }

        cJSON *fileResp = cJSON_Parse(_niraClient->uploaders[threadIdx]->responseBuf);
        if (!fileResp)
        {
            if (NiraSetError(_niraClient, NIRA_ERROR_JSON_PARSE))
            {
                NIRA_SET_ERR_DETAIL(_niraClient, "Response: %.*s", 128, _niraClient->uploaders[threadIdx]->responseBuf);
                NIRA_SET_ERR_MSG(_niraClient, "Could not parse file creation response");
            }
            continue;
        }

        cJSON *fileStatusItem = cJSON_GetObjectItem(fileResp, "status");
        assetFile->isAlreadyOnServer = fileStatusItem && (0 == strcmp(fileStatusItem->valuestring, "ready_for_processing"));
        if (forceFileUploads) { assetFile->isAlreadyOnServer = 0; }

        if (assetFile->isAlreadyOnServer)
        {
            atomic_fetch_add_64(&_niraClient->totalBytesCompleted, assetFile->size);
            atomic_fetch_add(&_niraClient->totalFilesCompleted, 1);
        }

        cJSON *fileUuidItem = cJSON_GetObjectItem(fileResp, "uuid");
        uint8_t isDuplicateFilename = fileUuidItem && 0 != strcmp(fileUuidItem->valuestring, assetFile->uuidStr);
        cJSON_Delete(fileResp);

        // Nira does not allow files of the same name to be uploaded within the same job (batch).
        // If the returned file record has a different uuid than the one we just provided,
        // it means we've already created a file with this name on the job, and it has
        // simply returned that previously created file record rather than creating a new one.
        if (isDuplicateFilename)
        {
            if (NiraSetError(_niraClient, NIRA_ERROR_DUPLICATE_FILENAME))
            {
                NIRA_SET_ERR_DETAIL(_niraClient, "File path: %s", assetFile->path);
                NIRA_SET_ERR_MSG(_niraClient, "All filenames within an upload batch must be unique. Duplicate filename %s", filename);
            }
            continue;
        }
    }

    if (NiraHasError(_niraClient))
    {
        return NiraGetError(_niraClient);
    }

    char uploadServiceHost[256];

    // Inform the server that we're about to upload files by setting the job status to "uploading".
    {
        cJSON *updateJobRequest = cJSON_CreateObject();

        cJSON_AddStringToObject(updateJobRequest, "status", "uploading");
        cJSON_AddStringToObject(updateJobRequest, "batchId", jobUuidStr);

        char jobPatchEndpoint[1024];
        snprintf(jobPatchEndpoint, sizeof(jobPatchEndpoint), "%s/%d", _niraClient->jobsEndpoint, jobId);

        NiraStatus reqStatus = makePatchRequestJson(_niraClient, _niraClient->webservice, jobPatchEndpoint, updateJobRequest);
        cJSON_Delete(updateJobRequest);

        if (NIRA_SUCCESS != reqStatus)
        {
            return NiraGetError(_niraClient);
        }

        cJSON *updateJobResp = cJSON_Parse(_niraClient->webservice->responseBuf);

        if (NULL == updateJobResp)
        {
            if (NiraSetError(_niraClient, NIRA_ERROR_JSON_PARSE))
            {
                NIRA_SET_ERR_DETAIL(_niraClient, "Response: %.*s", 128, _niraClient->webservice->responseBuf);
                NIRA_SET_ERR_MSG(_niraClient, "Could not parse update job response");
            }

            return NiraGetError(_niraClient);
        }

        cJSON *uploadServiceHostItem = cJSON_GetObjectItem(updateJobResp, "uploadServiceHost");
        if (!uploadServiceHostItem || !uploadServiceHostItem->valuestring || 0 == uploadServiceHostItem->valuestring[0])
        {
            if (NiraSetError(_niraClient, NIRA_ERROR_JSON_MISSING_ATTRIBUTE))
            {
                NIRA_SET_ERR_DETAIL(_niraClient, "Response: %.*s", 128, _niraClient->webservice->responseBuf);
                NIRA_SET_ERR_MSG(_niraClient, "Missing uploadServiceHost in job update response");
            }
            cJSON_Delete(updateJobResp);
            return NiraGetError(_niraClient);
        }

        snprintf(uploadServiceHost, sizeof(uploadServiceHost), "%s", uploadServiceHostItem->valuestring);

        cJSON_Delete(updateJobResp);
    }

    // Get part counts of all files
    size_t totalPartCount = 0;

    for (size_t ff = 0; ff < _fileCount; ff++)
    {
        NiraAssetFile *assetFile = &_files[ff];

        //fprintf(stderr, "FOR FILE: %s HASH: %s SIZE:%zd\n", assetFile->path, assetFile->hashStr, assetFile->size);
        if (assetFile->isAlreadyOnServer) { continue; }

        //fprintf(stderr, "got file status: %s\n", fileStatus->valuestring);
        const size_t filePartCount = (uint32_t)((assetFile->size / UPLOAD_CHUNK_SIZE) + 1);
        totalPartCount += filePartCount;
        assetFile->partsUploadedCount = 0;
    }

    // File type priority
    NiraFileType fileTypePriorities[] = {
        NIRA_FILETYPE_TEXTURE,
        NIRA_FILETYPE_MATERIAL,
        NIRA_FILETYPE_SCENE,
        NIRA_FILETYPE_AUTO, // Catch all, includes everything not already queued.
    };

    NiraFilePartTask *filePartTasks = calloc(totalPartCount, sizeof(NiraFilePartTask));
    size_t filePartTaskIdx = 0;

    size_t numFiletypePriorities = sizeof(fileTypePriorities) / sizeof(fileTypePriorities[0]);
    for (size_t ftpIdx = 0; ftpIdx < numFiletypePriorities; ftpIdx++)
    {
        NiraFileType requiredFiletype = (NiraFileType) fileTypePriorities[ftpIdx];

        for (size_t ff = 0; ff < _fileCount; ff++)
        {
            NiraAssetFile *assetFile = &_files[ff];

            if (assetFile->isAlreadyOnServer) { continue; }
            if (assetFile->isQueued)          { continue; }

            if (requiredFiletype == assetFile->type || requiredFiletype == NIRA_FILETYPE_AUTO)
            {
                const size_t filePartCount = (uint32_t)((assetFile->size / UPLOAD_CHUNK_SIZE) + 1);

                fprintf(stderr, "FILE: %zd %s\n", ff, assetFile->path);

                for (size_t pp = 0; pp < filePartCount; pp++)
                {
                    filePartTasks[filePartTaskIdx].fileIdx = ff;
                    filePartTasks[filePartTaskIdx].partIdx = pp;

                    filePartTaskIdx++;
                }

                assetFile->isQueued = true;
            }
        }
    }

    int32_t taskIdx = 0;

    #pragma omp parallel num_threads(_niraClient->numUploadThreads)
    #pragma omp for schedule(static, 1)
    for (taskIdx = 0; taskIdx < totalPartCount; taskIdx++)
    {
        if (NiraHasError(_niraClient))
        {
            continue;
        }

        NiraFilePartTask *filePartTask = &filePartTasks[taskIdx];

        uint32_t fileIdx = filePartTask->fileIdx;
        uint32_t partIdx = filePartTask->partIdx;

#ifdef _OPENMP
        fprintf(stderr, "task %d uploading file %d chunk %d using thread: %d\n", taskIdx, fileIdx, partIdx, omp_get_thread_num());
#endif

        NiraAssetFile *assetFile = &_files[fileIdx];

        const size_t filePartCount = (uint32_t)((assetFile->size / UPLOAD_CHUNK_SIZE) + 1);

        const char *filename = assetFile->name ? assetFile->name : filenameExt(assetFile->path);

        if (NULL == assetFile->mappedBuf) // An optimization (assuming OpenMP doesn't do it already, but it may)
        {
            #pragma omp critical
            if (NULL == assetFile->mappedBuf)
            {
                // Sanity check -- did the file disappear?
                {
                    #if _NIRACLIENT_WIN32_WCHAR_PATHS_AND_NAMES
                    FILE *fh;
                    _wfopen_s(&fh, assetFile->pathW, L"rb");
                    #else
                    FILE *fh = fopen(assetFile->path, "rb");
                    #endif
                    if (NULL == fh)
                    {
                        if (NiraSetError(_niraClient, NIRA_ERROR_FILE_CHANGED_DURING_UPLOAD))
                        {
                            NIRA_SET_ERR_DETAIL(_niraClient, "fopen: %s (%d)", strerror(errno), errno);
                            NIRA_SET_ERR_MSG(_niraClient, "File disappeared or became unreadable during upload: %s", assetFile->path);
                        }
                    }

                    if (NULL != fh) { fclose(fh); }
                }

                if (!NiraHasError(_niraClient))
                {
                    size_t filesize;

                    #if _NIRACLIENT_WIN32_WCHAR_PATHS_AND_NAMES
                    assetFile->mappedBuf = map_file(assetFile->pathW, /*out*/ &filesize);
                    #else
                    assetFile->mappedBuf = map_file(assetFile->path, /*out*/ &filesize);
                    #endif

                    if (NULL == assetFile->mappedBuf)
                    {
                        if (NiraSetError(_niraClient, NIRA_ERROR_FILE_IO))
                        {
                            NIRA_SET_ERR_DETAIL(_niraClient, "map_file: %s (%d)", strerror(errno), errno);
                            NIRA_SET_ERR_MSG(_niraClient, "Could not open or read file %s", assetFile->path);
                        }
                    }
                    else if (assetFile->size != filesize)
                    {
                        unmap_file(assetFile->mappedBuf, filesize);
                        assetFile->mappedBuf = NULL;

                        if (NiraSetError(_niraClient, NIRA_ERROR_FILE_CHANGED_DURING_UPLOAD))
                        {
                            NIRA_SET_ERR_DETAIL(_niraClient, "Filesize changed: %zd -> %zd", assetFile->size, filesize);
                            NIRA_SET_ERR_MSG(_niraClient, "File was modified during upload: %s", assetFile->path);
                        }
                    }
                }
            }
        }

        if (NiraHasError(_niraClient))
        {
            continue;
        }

        if (NULL == assetFile->mappedBuf)
        {
            // This should only happen if the code is written improperly
            if (NiraSetError(_niraClient, NIRA_ERROR_FILE_IO))
            {
                NIRA_UNSET_ERR_DETAIL(_niraClient);
                NIRA_SET_ERR_MSG(_niraClient, "Unexpected memory mapping / multi-threading error on file: %s", assetFile->path);
            }
            continue;
        }

        //fprintf(stderr, "got file status: %s\n", fileStatus->valuestring);

        // Note, we could consider a check like this:
        //{
        //    // Sanity check -- did the file disappear?
        //    FILE *fh = fopen(assetFile->path, "rb");
        //    if (NULL == fh)
        //    {
        //        NIRA_SET_ERR_DETAIL(_niraClient, "fopen: %s (%d)", strerror(errno), errno);
        //        NIRA_SET_ERR_MSG(_niraClient, "File disappeared or became unreadable during upload: %s", assetFile->path);
        //        return NIRA_ERROR_FILE_CHANGED_DURING_UPLOAD;
        //    }
        //    fclose(fh);
        //}
        //
        // We could also consider check if filesize has changed here. It's likely an error condition if any have.

        if (NiraHasError(_niraClient))
        {
            continue;
        }

        size_t partoffset = (partIdx * UPLOAD_CHUNK_SIZE);
        char *filePartPtr = &assetFile->mappedBuf[partoffset];
        size_t filePartSize = (partIdx < (filePartCount - 1)) ? UPLOAD_CHUNK_SIZE : (assetFile->size % UPLOAD_CHUNK_SIZE);

        if ((filePartPtr + filePartSize) > (assetFile->mappedBuf + assetFile->size))
        {
            if (NiraSetError(_niraClient, NIRA_ERROR_FILE_IO))
            {
                NIRA_SET_ERR_DETAIL(_niraClient, "filePartPtr:%p filePartSize:%zd assetFile->mappedBuf:%p assetFile->size:%zd\n", filePartPtr, filePartSize, assetFile->mappedBuf, assetFile->size);
                NIRA_SET_ERR_MSG(_niraClient, "Unexpected result while reading file %s", assetFile->path);
            }
            continue;
        }

        cJSON *fileUploadPartRequestBody = cJSON_CreateObject();

        cJSON_AddStringToObject(fileUploadPartRequestBody, "uuid", assetFile->uuidStr);
        cJSON_AddStringToObject(fileUploadPartRequestBody, "filename", filename);
        cJSON_AddNumberToObject(fileUploadPartRequestBody, "chunksize", (double)filePartSize);
        cJSON_AddNumberToObject(fileUploadPartRequestBody, "partindex", (double)partIdx);
        cJSON_AddNumberToObject(fileUploadPartRequestBody, "filePartCount", (double)filePartCount);
        cJSON_AddNumberToObject(fileUploadPartRequestBody, "totalfilesize", (double)assetFile->size);
        cJSON_AddNumberToObject(fileUploadPartRequestBody, "partbyteoffset", (double)partoffset);
        if (_niraClient->useCompression)
        {
            cJSON_AddStringToObject(fileUploadPartRequestBody, "compression", "deflate");
        }

#ifdef _OPENMP
        const size_t threadIdx = omp_get_thread_num();
#else
        const size_t threadIdx = 0;
#endif

        NiraStatus reqStatus = makeFileUploadPartRequest(_niraClient, _niraClient->uploaders[threadIdx], uploadServiceHost, filename, assetFile, fileUploadPartRequestBody, filePartPtr, filePartSize);
        cJSON_Delete(fileUploadPartRequestBody);

        if (NIRA_SUCCESS != reqStatus)
        {
            continue;
        }

        cJSON *uploadPartResponse = cJSON_Parse(_niraClient->uploaders[threadIdx]->responseBuf);

        if (!uploadPartResponse)
        {
            if (NiraSetError(_niraClient, NIRA_ERROR_JSON_PARSE))
            {
                NIRA_SET_ERR_DETAIL(_niraClient, "Response: %.*s", 128, _niraClient->uploaders[threadIdx]->responseBuf);
                NIRA_SET_ERR_MSG(_niraClient, "Could not parse file upload part response");
            }
            continue;
        }

        cJSON_Delete(uploadPartResponse);

        uint32_t partsUploaded = atomic_fetch_add(&assetFile->partsUploadedCount, 1) + 1;

        if (partsUploaded == filePartCount)
        {
            assetFile->partsUploadedCount = filePartCount;

            //fprintf(stderr, "finished upload of %s\n", filename);

            cJSON *fileUploadDoneRequestBody = cJSON_CreateObject();

            cJSON_AddStringToObject(fileUploadDoneRequestBody, "filename", filename);
            cJSON_AddStringToObject(fileUploadDoneRequestBody, "uuid", assetFile->uuidStr);
            cJSON_AddNumberToObject(fileUploadDoneRequestBody, "totalfilesize", (double)assetFile->size);
            cJSON_AddNumberToObject(fileUploadDoneRequestBody, "filePartCount", filePartCount);
            cJSON_AddStringToObject(fileUploadDoneRequestBody, "meowhash", assetFile->hashStr);

            char fileUploadDoneEndpoint[512];
            snprintf(fileUploadDoneEndpoint, sizeof(fileUploadDoneEndpoint), "https://%s/file-upload-done", uploadServiceHost);

            //fprintf(stderr, "issuing file upload done request %s\n", fileUploadDoneEndpoint);
            NiraStatus reqStatus = makePostRequestJson(_niraClient, _niraClient->uploaders[threadIdx], fileUploadDoneEndpoint, fileUploadDoneRequestBody);
            cJSON_Delete(fileUploadDoneRequestBody);

            if (NIRA_SUCCESS != reqStatus)
            {
                continue;
            }

            cJSON *fileUploadDoneResp = cJSON_Parse(_niraClient->uploaders[threadIdx]->responseBuf);
            if (!fileUploadDoneResp)
            {
                if (NiraSetError(_niraClient, NIRA_ERROR_JSON_PARSE))
                {
                    NIRA_SET_ERR_DETAIL(_niraClient, "Response: %.*s", 128, _niraClient->uploaders[threadIdx]->responseBuf);
                    NIRA_SET_ERR_MSG(_niraClient, "Could not parse file upload done response");
                }
                continue;
            }

            cJSON_Delete(fileUploadDoneResp);
            //fprintf(stderr, "finished file upload done request %s\n", fileUploadDoneEndpoint);

            unmap_file(assetFile->mappedBuf, assetFile->size);
            assetFile->mappedBuf = NULL;

            atomic_fetch_add(&_niraClient->totalFilesCompleted, 1);
        }
    }

    free(filePartTasks);

    if (NiraHasError(_niraClient))
    {
        return NiraGetError(_niraClient);
    }

    for (size_t ff = 0; ff < _fileCount; ff++)
    {
        NiraAssetFile *assetFile = &_files[ff];

        if (assetFile->isAlreadyOnServer) { continue; }

        const size_t filePartCount = (uint32_t)((assetFile->size / UPLOAD_CHUNK_SIZE) + 1);

        if (assetFile->partsUploadedCount != filePartCount)
        {
            if (NiraSetError(_niraClient, NIRA_ERROR_GENERAL))
            {
                NIRA_SET_ERR_DETAIL(_niraClient, "assetFile->partsUploadedCount:%d filePartCount:%zd\n", assetFile->partsUploadedCount, filePartCount);
                NIRA_SET_ERR_MSG(_niraClient, "Failure uploading some parts of %s", assetFile->path);
            }

            return NiraGetError(_niraClient);
        }

        fprintf(stderr, "Compression stats: %s size:%zd compressedSize:%zd ratio:%f\n", assetFile->path, assetFile->size, (size_t)assetFile->compressedBytesUploaded, ((float)(assetFile->compressedBytesUploaded)) / assetFile->size);
    }

    {
        cJSON *updateJobRequest = cJSON_CreateObject();

        cJSON_AddStringToObject(updateJobRequest, "status", "uploaded");
        cJSON_AddStringToObject(updateJobRequest, "batchId", jobUuidStr);

        char jobPatchEndpoint[1024];
        snprintf(jobPatchEndpoint, sizeof(jobPatchEndpoint), "%s/%d", _niraClient->jobsEndpoint, jobId);

        NiraStatus reqStatus = makePatchRequestJson(_niraClient, _niraClient->webservice, jobPatchEndpoint, updateJobRequest);
        cJSON_Delete(updateJobRequest);

        if (NIRA_SUCCESS != reqStatus)
        {
            return NiraGetError(_niraClient);
        }

        cJSON *updateJobResp = cJSON_Parse(_niraClient->webservice->responseBuf);

        if (NULL == updateJobResp)
        {
            if (NiraSetError(_niraClient, NIRA_ERROR_JSON_PARSE))
            {
                NIRA_SET_ERR_DETAIL(_niraClient, "Response: %.*s", 128, _niraClient->webservice->responseBuf);
                NIRA_SET_ERR_MSG(_niraClient, "Could not parse update job response");
            }

            return NiraGetError(_niraClient);
        }

        cJSON_Delete(updateJobResp);
    }

    return 0;
}

NiraClient *niraInit(const char *_orgName, const char *_userAgent)
{
    NiraClient *niraClient = calloc(1, sizeof(NiraClient));

    niraClient->isInitialized = NiraClientInitSentinel;

    snprintf(niraClient->orgName, sizeof(niraClient->orgName), "%s", _orgName);
    snprintf(niraClient->userAgentHeader, sizeof(niraClient->userAgentHeader), "User-Agent: %s", _userAgent);
    snprintf(niraClient->jobsEndpoint, sizeof(niraClient->jobsEndpoint), "https://%s/api/jobs", _orgName);
    snprintf(niraClient->filesEndpoint, sizeof(niraClient->filesEndpoint), "https://%s/api/files", _orgName);
    snprintf(niraClient->assetsEndpoint, sizeof(niraClient->assetsEndpoint), "https://%s/api/assets", _orgName);
    snprintf(niraClient->orgHeader, sizeof(niraClient->orgHeader), "x-nira-org: %s", _orgName);
    snprintf(niraClient->coordsysEndpoint, sizeof(niraClient->coordsysEndpoint), "https://%s/api/coordsys", _orgName);

    snprintf(niraClient->apiTokenHeader, sizeof(niraClient->apiTokenHeader), "x-api-token: invalid");

    snprintf(niraClient->niraConfigEndpoint, sizeof(niraClient->niraConfigEndpoint), "https://%s/loginconfig", _orgName);

    niraClient->useCompression = 1;
    niraClient->numUploadThreads = 4;
    niraClient->assetNameConflictResolution = NIRA_CONFLICTRES_CREATE_NEW_ASSET_WITH_NAME_POSTFIX;

    int32_t niraClientCount = atomic_fetch_add(&g_niraClientCount, 1) + 1;

    if (1 == niraClientCount)
    {
        //fprintf(stderr, "running curl global init\n");
        curl_global_init(CURL_GLOBAL_DEFAULT);

        curl_version_info_data *curlVersionInfo = curl_version_info(CURLVERSION_NOW);

        if (NIRA_CURL_EXPECTED_VERSION != curlVersionInfo->version_num)
        {
            fprintf(stderr, "This version of niraclient was tested with libcurl version " NIRA_CURL_EXPECTED_VERSION_STR " (statically linked), and you have version %s. Use this version at your own risk!\n", curlVersionInfo->version);
        }
    }

    return niraClient;
}

NiraStatus niraDeinit(NiraClient *_niraClient)
{
    if (!isValidNiraClient(_niraClient))
    {
        return NIRA_ERROR_INVALID_NIRACLIENT;
    }

    _niraClient->isInitialized = 0;

    if (NULL != _niraClient->webservice)
    {
        //fprintf(stderr, "Cleaned up webservice.curl\n");
        if (NULL != _niraClient->webservice->curl)
        {
            curl_easy_cleanup(_niraClient->webservice->curl);
        }

        free(_niraClient->webservice);
    }

    if (NULL != _niraClient->authservice)
    {
        if (NULL != _niraClient->authservice->curl)
        {
            curl_easy_cleanup(_niraClient->authservice->curl);
        }

        free(_niraClient->authservice);
    }

    for (size_t tt = 0; tt < NIRA_MAX_UPLOAD_THREADS; tt++)
    {
        if (NULL != _niraClient->uploaders[tt])
        {
            //fprintf(stderr, "Cleaned up uploaders[%zd]\n", tt);
            if (NULL != _niraClient->uploaders[tt]->curl)
            {
                curl_easy_cleanup(_niraClient->uploaders[tt]->curl);
            }
            free(_niraClient->uploaders[tt]);
        }
    }

    if (NULL != _niraClient->stringPool)
    {
        free(_niraClient->stringPool);
    }

    free(_niraClient);

    int32_t niraClientCount = atomic_fetch_add(&g_niraClientCount, -1) - 1;

    if (niraClientCount == 0)
    {
        if (1 == g_niraPerformCurlGlobalCleanup)
        {
            //fprintf(stderr, "running curl global cleanup\n");
            curl_global_cleanup();
        }
    }

    return NIRA_SUCCESS;
}

NiraStatus niraSetNumUploadThreads(NiraClient *_niraClient, uint8_t _numUploadThreads)
{
    if (!isValidNiraClient(_niraClient))
    {
        return NIRA_ERROR_INVALID_NIRACLIENT;
    }

    _niraClient->numUploadThreads = _numUploadThreads;

    if (_niraClient->numUploadThreads > NIRA_MAX_UPLOAD_THREADS)
    {
        _niraClient->numUploadThreads = NIRA_MAX_UPLOAD_THREADS;
    }

    return NIRA_SUCCESS;
}

NiraStatus niraSetAppName(NiraClient *_niraClient, const char *_appName)
{
    if (!isValidNiraClient(_niraClient))
    {
        return NIRA_ERROR_INVALID_NIRACLIENT;
    }

    snprintf(_niraClient->appName, sizeof(_niraClient->appName), "%s", _appName);
    return NIRA_SUCCESS;
}

NiraStatus niraSetCoordsys(NiraClient *_niraClient, const char *_coordsys, int64_t _retryTimeSeconds)
{
    if (!isValidNiraClient(_niraClient))
    {
        return NIRA_ERROR_INVALID_NIRACLIENT;
    }

    char coordsysReqUrl[1024];
    snprintf(coordsysReqUrl, sizeof(coordsysReqUrl), "%s/%s", _niraClient->coordsysEndpoint, _coordsys);

    NiraStatus reqStatus = makeGetRequest(_niraClient, _niraClient->webservice, coordsysReqUrl, _retryTimeSeconds);
    if (NIRA_SUCCESS != reqStatus)
    {
        return NiraGetError(_niraClient);
    }

    cJSON *coordsysResp = cJSON_Parse(_niraClient->webservice->responseBuf);
    if (NULL == coordsysResp)
    {
        if (NiraSetError(_niraClient, NIRA_ERROR_JSON_PARSE))
        {
            NIRA_SET_ERR_DETAIL(_niraClient, "Response: %.*s", 128, _niraClient->webservice->responseBuf);
            NIRA_SET_ERR_MSG(_niraClient, "Could not parse coordsys response");
        }
        return NiraGetError(_niraClient);
    }

    cJSON *supportedItem = cJSON_GetObjectItem(coordsysResp, "isSupported");
    if (NULL != supportedItem && !supportedItem->valueint)
    {
        cJSON *supportErrMsg = cJSON_GetObjectItem(coordsysResp, "isSupportedMsg");

        if (NiraSetError(_niraClient, NIRA_ERROR_UNSUPPORTED_COORDINATE_SYSTEM))
        {
            NIRA_UNSET_ERR_DETAIL(_niraClient);
            NIRA_SET_ERR_MSG(_niraClient, "%s", supportErrMsg->valuestring);
        }

        cJSON_Delete(coordsysResp);

        return NiraGetError(_niraClient);
    }

    cJSON_Delete(coordsysResp);

    snprintf(_niraClient->coordsys, sizeof(_niraClient->coordsys), "%s", _coordsys);

    return NIRA_SUCCESS;
}

NiraStatus niraAbort(NiraClient *_niraClient)
{
    if (!isValidNiraClient(_niraClient))
    {
        return NIRA_ERROR_INVALID_NIRACLIENT;
    }

    atomic_fetch_add(&_niraClient->abortAll, 1);

    while (true)
    {
        if (NULL != _niraClient->webservice)
        {
            //fprintf(stderr, "Cleaned up webservice.curl\n");
            if (NULL != _niraClient->webservice->curl)
            {
                curl_socket_t sockfd;
                curl_easy_getinfo(_niraClient->webservice->curl, CURLINFO_ACTIVESOCKET, &sockfd);
                closesocket(sockfd);
            }
        }

        if (NULL != _niraClient->authservice)
        {
            if (NULL != _niraClient->authservice->curl)
            {
                curl_socket_t sockfd;
                curl_easy_getinfo(_niraClient->authservice->curl, CURLINFO_ACTIVESOCKET, &sockfd);
                closesocket(sockfd);
            }
        }

        for (size_t tt = 0; tt < NIRA_MAX_UPLOAD_THREADS; tt++)
        {
            if (NULL != _niraClient->uploaders[tt])
            {
                if (NULL != _niraClient->uploaders[tt]->curl)
                {
                    curl_socket_t sockfd;
                    curl_easy_getinfo(_niraClient->uploaders[tt]->curl, CURLINFO_ACTIVESOCKET, &sockfd);
                    closesocket(sockfd);
                }
            }
        }

        if (_niraClient->activeRequestCount == 0)
        {
            break;
        }

        struct timespec sleepTs = {0, 2e8}; // 200ms
        thrd_sleep(&sleepTs, NULL);
    }

    return NIRA_SUCCESS;
}

const char *niraGetErrorMessage(NiraClient *_niraClient)
{
    static __thread char errorMessage[sizeof(_niraClient->errorMessage)];
    snprintf(errorMessage, sizeof(errorMessage), "%s", _niraClient->errorMessage);
    return errorMessage;
}

const char *niraGetErrorDetail(NiraClient *_niraClient)
{
    static __thread char errorDetail[sizeof(_niraClient->errorDetail)];
    snprintf(errorDetail, sizeof(errorDetail), "%s", _niraClient->errorDetail);
    return errorDetail;
}

const char *niraGetClientVersion()
{
    return "e206a77c";
}

/* vim: set sw=4 ts=4 expandtab: */
