#ifndef NIRACLIENT_H
#define NIRACLIENT_H
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#ifndef _OPENMP
#pragma message( "openmp is recommended for best upload performance, and required if you need thread safety. Compile using '-fopenmp' with clang/gcc and '/openmp' with MSVC")
#endif

// Note, we've tested niraclient with libcurl version 7.86.0 (statically linked), so that is what we recommend.
// Other versions may work fine.
#define NIRA_CURL_EXPECTED_VERSION 0x075600
#define NIRA_CURL_EXPECTED_VERSION_STR "7.86.0"

#if defined(_MSC_VER)
#ifndef _CRT_SECURE_NO_WARNINGS
    #define _CRT_SECURE_NO_WARNINGS
#endif
typedef volatile int atomic_int;
typedef volatile unsigned atomic_uint;
typedef volatile unsigned long long atomic_ullong;
#else
typedef int atomic_int;
typedef unsigned int atomic_uint;
typedef unsigned long long atomic_ullong;
#endif

// g_niraPerformCurlGlobalCleanup:
//
// Setting this global to 1 in your application prior to calling any Nira related functions
// will cause the niraDeinit() function to automatically call curl_global_cleanup()
// when niraDeinit() is called on the last NiraClient instance within your process.
//
// Note, curl_global_init() will always be called if needed, so it's safe to for your
// processes' NiraClient instance count to bounce between 0 and 1.
//
// Also note, setting this to 1 is only safe to use if you're not already using libcurl elsewhere within your process.
// If you don't use this option, you need to either call curl_global_cleanup() yourself at the
// appropriate time, or the bit of memory that curl allocates for its global initializion will remain allocated
// until your process exits, which is not a big deal, in most cases.
extern int32_t g_niraPerformCurlGlobalCleanup; // 0 by default

// g_niraClientCount:
//
// The total number of current NiraClient instances.
// This is incremented upon a successful call to niraInit() and decremented upon a successful call to niraDeinit().
extern atomic_int g_niraClientCount;

#ifndef NIRA_MAX_UPLOAD_THREADS
#   define NIRA_MAX_UPLOAD_THREADS 8
#endif

// Controls how long (in seconds) Nira should keep retrying failed
// requests. Requests are retried using a backoff amount equal to the retry
// count. Before the first retry, we wait 0 seconds, before the second retry
// we wait 1 second, etc. This means that the 5th retry of a request will
// occur after approximately 10 seconds (0+1+2+3+4) have elapsed, so using
// a value of 10 for NIRA_INTERNAL_REQUEST_RETRY_TIME_TOTAL allows for 5 retries.
//
// A default of 1000 seconds (16 minutes) is used here.
#ifndef NIRA_INTERNAL_REQUEST_RETRY_TIME_TOTAL
#   define NIRA_INTERNAL_REQUEST_RETRY_TIME_TOTAL 1000
#endif

// A multiplier for the request retry backoff.
#ifndef NIRA_REQUEST_RETRY_BACKOFF_FACTOR
#   define NIRA_REQUEST_RETRY_BACKOFF_FACTOR 1
#endif

#ifndef NIRA_MAX_HTTP_RESPONSE_SIZE
#   define NIRA_MAX_HTTP_RESPONSE_SIZE 128 * 1024
#endif

// How to handle when a specified asset name is already being used by an existing asset.
// Note, the default currently used within niraclient.c is NIRA_CONFLICTRES_CREATE_NEW_ASSET_WITH_NAME_POSTFIX.
typedef enum _NiraAssetNameConflictResolution
{
    // If an asset with the specified name already exists, the server will determine
    // the handling. Currently, the server default is NIRA_CONFLICTRES_ADD_VERSION_AND_UPDATE_FILESET,
    // but this may change in the future and/or become configurable by a Nira organization's administrator.
    NIRA_CONFLICTRES_SERVER_DEFAULT = 0,

    // If an asset with the specified name already exists, the server will automatically add
    // a postfix to the asset name. The postfix style used could be determined by the organization
    // administrator, but could be a numbered series like "(2)", "(3)", etc, or the current
    // date/time.
    NIRA_CONFLICTRES_CREATE_NEW_ASSET_WITH_NAME_POSTFIX,

    // If an asset with the specified name already exists, the server will increment the
    // version of the asset, and the new version will be comprised of all the existing
    // files from the asset plus any new files included in the upload. The uploaded files
    // will also replace any existing files that have the same filenames.
    NIRA_CONFLICTRES_ADD_VERSION_AND_UPDATE_FILESET,

    // If an asset with the specified name already exists, the server will increment the
    // version of the asset, and the new version will be comprised ONLY of the uploaded files.
    // Files from prior versions of the asset will not automatically be a part of the new version.
    NIRA_CONFLICTRES_ADD_VERSION_AND_REPLACE_FILESET,

    // If an asset with the specified name already exists, the server will return an error.
    NIRA_CONFLICTRES_RETURN_ERROR,

} NiraAssetNameConflictResolution;

typedef enum _NiraFileType
{
    NIRA_FILETYPE_AUTO = 0,
    NIRA_FILETYPE_TEXTURE,
    NIRA_FILETYPE_PHOTO,
    NIRA_FILETYPE_SCENE,
    NIRA_FILETYPE_MATERIAL,

    NIRA_FILETYPE_COUNT,
} NiraFileType;

typedef enum _NiraAssetType
{
    NIRA_ASSET_TYPE_PHOTOGRAMMETRY = 0,
    NIRA_ASSET_TYPE_SCULPT,
    NIRA_ASSET_TYPE_PBR,
    NIRA_ASSET_TYPE_VOLUMETRIC_VIDEO,

    NIRA_ASSET_TYPE_COUNT,
} NiraAssetType;

typedef struct _NiraAssetFile
{
    ////////////////////////
    // Public. The usage code can populate these:
    //
    #if defined(_WIN32) && defined(_NIRACLIENT_UTF16_PATHS_AND_NAMES)
    wchar_t *pathW; // A path to the file. It's probably best to use absolute paths, but if using relative directory, be sure the cwd of the process is correct and not altered during execution.
    NiraFileType type; // Specify NIRA_FILETYPE_TEXTURE, NIRA_FILETYPE_IMAGE, NIRA_FILETYPE_MATERIAL, etc.
    wchar_t *nameW; // The name of the file, including its extension. Optional. If NULL, `basename(path)` is used. The default is almost always the correct choice.
    #else
    char  *path; // A path to the file. It's probably best to use absolute paths, but if using relative directory, be sure the cwd of the process is correct and not altered during execution.
    NiraFileType type; // Specify NIRA_FILETYPE_TEXTURE, NIRA_FILETYPE_IMAGE, NIRA_FILETYPE_MATERIAL, etc.
    char  *name; // The name of the file, including its extension. Optional. If NULL, `basename(path)` is used. The default is almost always the correct choice.
    #endif

    ////////////////////////
    // Private:
    //
    #if defined(_WIN32) && defined(_NIRACLIENT_UTF16_PATHS_AND_NAMES)
    // When _NIRACLIENT_UTF16_PATHS_AND_NAMES is defined, niraclient-c takes care of populating the path and name strings below with utf-8 encodings of the pathW and nameW strings above.
    // If you're using _NIRACLIENT_UTF16_PATHS_AND_NAMES, please do not specify these path and name pointers from your application:
    char  *path; // A path to the file. It's probably best to use absolute paths, but if using relative directory, be sure the cwd of the process is correct and not altered during execution.
    char  *name; // The name of the file, including its extension. Optional. If NULL, `basename(path)` is used. The default is almost always the correct choice.
    #endif
    size_t size;
    char *mappedBuf;
    char uuidStr[38];
    char hashStr[38];
    uint32_t isAlreadyOnServer;
    uint32_t isQueued;
    atomic_uint partsUploadedCount;
    atomic_ullong bytesUploaded;
    atomic_ullong compressedBytesUploaded;
} NiraAssetFile;

typedef void CURL;

typedef struct _NiraService
{
    CURL *curl;
    char curlErrorDetail[512];
    char responseBuf[NIRA_MAX_HTTP_RESPONSE_SIZE];
    size_t responseLength;
} NiraService;

typedef struct _NiraClient
{
    // Public:
    uint32_t numUploadThreads;
    uint32_t useCompression;
    NiraAssetNameConflictResolution assetNameConflictResolution;

    // Private:
    uint64_t isInitialized;

    char orgName[256];
    char jobsEndpoint[512];
    char coordsysEndpoint[512];
    char niraConfigEndpoint[512];
    char filesEndpoint[512];
    char assetsEndpoint[512];
    char apiKeyEndpoint[512];
    char userAgentHeader[512];
    char orgHeader[512];
    char apiKeyIdHeader[512];
    char apiKeySecretHeader[512];
    char apiTokenHeader[4096];
    char appName[128];
    char coordsys[128];

    char assetUrl[512];

    atomic_int statusCode;
    char errorMessage[512];
    char errorDetail[2048];

    time_t lastSuccessfulAuthTime;

    atomic_uint abortAll;

    atomic_int isAuthorizing;
    atomic_int failingRequestCount;
    atomic_int activeRequestCount;

    atomic_uint totalFilesCompleted;
    atomic_ullong totalBytesCompleted;

    volatile size_t totalFileSize;
    size_t totalFileCount;

    char *stringPool;

    NiraService *uploaders[NIRA_MAX_UPLOAD_THREADS];
    NiraService *webservice;
    NiraService *authservice;
} NiraClient;

typedef enum _NiraStatus
{
    NIRA_SUCCESS = 0,
    NIRA_ERROR_GENERAL,
    NIRA_ERROR_JSON_PARSE,
    NIRA_ERROR_JSON_MISSING_ATTRIBUTE,
    NIRA_ERROR_NETWORK,
    NIRA_ERROR_HTTP,
    NIRA_ERROR_FILE_IO,
    NIRA_ERROR_DUPLICATE_FILENAME,
    NIRA_ERROR_FILE_CHANGED_DURING_UPLOAD,
    NIRA_ERROR_ENTROPY_FAILURE,
    NIRA_ERROR_INVALID_NIRACLIENT,
    NIRA_ERROR_CURL_INIT_FAILURE,
    NIRA_ERROR_COMPRESSION_FAILURE,
    NIRA_ERROR_MISSING_CREDENTIALS,
    NIRA_ERROR_ABORTED_BY_USER,
    NIRA_ERROR_UNSUPPORTED_COORDINATE_SYSTEM,

    NIRA_ERROR_COUNT,
} NiraStatus;

// Allocates a NiraClient instance and returns it. Does not make network requests.
// Returns NULL upon error, which would basically mean malloc failed.
// This function does not make network requests.
extern NiraClient *niraInit (const char *_orgName, const char *_userAgent);

// Deallocates the provided NiraClient instance and its associated resources.
// This function does not make network requests.
extern NiraStatus niraDeinit(NiraClient *_niraClient);

// Stores the apiKeyId and apiKeySecret onto the NiraClient. This is required.
// This function does not make network requests.
extern NiraStatus niraSetCredentials(NiraClient *_niraClient, const char *_apiKeyId, const char *_apiKeySecret);

// Exchanges the apiKeyId and apiKeySecret (specified via niraSetCredentials) for an apiToken,
// then persists some more information into the provided NiraClient.
// This may make network requests to the web service and auth service. You are not required to call this, but it is
// useful for verifying that a key id and secret is correct when a user provides it within your UI, or prior to
// performing a long-running operation like a call to niraUploadAsset.
// This function will make make network requests, and is blocking.
extern NiraStatus niraAuthorize(NiraClient *_niraClient, int64_t _retryTimeSeconds);

// Uploads the provided files (the array of `NiraAssetFile`s) to a new asset named `_assetName`.
// This will make network requests, and is blocking.
// On Windows, you can use the unicode variant of this function by defining the preprocessor flag _NIRACLIENT_UTF16_PATHS_AND_NAMES.
// When _NIRACLIENT_UTF16_PATHS_AND_NAMES is defined, the function expects the array of NiraAssetFile structs to contain wchar_t strings
// for the path and name members, rather than char strings.
// The _assetName parameter should also be a wchar_t *.
#if defined(_WIN32) && defined(_NIRACLIENT_UTF16_PATHS_AND_NAMES)
extern NiraStatus niraUploadAsset(NiraClient *_niraClient, NiraAssetFile *_files, size_t _fileCount, const wchar_t *_assetName, NiraAssetType _assetType);
#else
extern NiraStatus niraUploadAsset(NiraClient *_niraClient, NiraAssetFile *_files, size_t _fileCount, const char *_assetName, NiraAssetType _assetType);
#endif

extern NiraStatus niraSetAppName(NiraClient *_niraClient, const char *_appName);

extern NiraStatus niraSetNumUploadThreads(NiraClient *_niraClient, uint8_t _numUploadThreads);

extern NiraStatus niraSetCoordsys(NiraClient *_niraClient, const char *_coordsys, int64_t _retryTimeSeconds);

extern const char *niraGetClientVersion();

// niraAbort() attempts to immediately close all active sockets on the provided NiraClient,
// and causes the current niraUploadAsset() call to return as soon as they
// are closed. This will cause active file upload requests to be canceled midway through.
//
// A call to niraAbort will block until the any read/write related background threads
// are fully aborted, which makes it safe to call niraDeinit() immediately afterward.
extern NiraStatus niraAbort(NiraClient *_niraClient);

// niraGetErrorMessage(), niraGetErrorDetail(), and NiraClient.statusCode:
//
// The error information always represents the NiraClient instance's *first* error, if there
// is one. Later calls to the API will never reset an error message, error detail, or error statusCode.
// This means if you're not checking the return of every function call, the error information could
// be from some prior call.
//
// The strings returned by these functions are kept in static thread local storage,
// so they will remain safe to read by the calling thread even after niraDeinit()
// is called on the corresponding NiraClient instance.
//
// NiraClient.statusCode always stores the NiraStatus first encountered by that instance,
// and is equal to NIRA_SUCCESS (0) if an error has never been encountered.
extern const char *niraGetErrorMessage(NiraClient *_niraClient);
extern const char *niraGetErrorDetail(NiraClient *_niraClient);

extern int32_t uuidGenV4(char *_uuidStr);

#endif // NIRACLIENT_H
/* vim: set sw=4 ts=4 expandtab: */
