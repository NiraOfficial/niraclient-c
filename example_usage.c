#include "niraclient.h"
#include <stdlib.h> // Used for getenv()

int printUploadProgress(void *_nc);
int startPrintProgressThread(NiraClient *_niraClient);
char *getRandomAssetName(char *_assetNameBuf, size_t _assetNameBufSize, const char *_assetNamePrefix);

int32_t main()
{
    // In your implementation, you can allow for specification of an asset name, and/or default
    // to using the "project name", if your application already has a concept of that.
    // For testing and easy iteration during development, we just generate a random name, here.
    char assetName[128];
    getRandomAssetName(assetName, sizeof(assetName), "test-asset-");

    NiraClient *niraClient = niraInit(
              "uploadtest.nira.app"     // Pass the actual org name that the user provides
            , "Your application v1.2.3" // User agent string included in all server requests. It's important to use the name of your application here, and a version number is also perferred.
            );

    // This is an important call - Reach out to Nira for the correct string value to use here.
    //niraSetAppName(niraClient, "ask_nira_support_about_this_value!");

    // This is a very important call if you're uploading a georeferenced dataset. When calling this function, you must pass the epsg string that corresponds with
    // how your geometry and camera data were saved.
    // "epsg:4978" is what we recommend, but we support just about any cartesian coordinate system. For example, UTM zones are also supported (e.g. "epsg:32632").
    //niraSetCoordsys(niraClient, "epsg:4978");

    char *appKeyId     = getenv("APP_KEY_ID_UPLOADTEST");
    char *appKeySecret = getenv("APP_KEY_SECRET_UPLOADTEST");

    // Calling setCredentials is required. Any NiraClient call that makes network requests will fail without a valid app key id and secret.
    niraSetCredentials(niraClient
      , appKeyId     // app key id: From the "App keys" area of the Nira UI.
      , appKeySecret // app key secret: From the "App keys" area of the Nira UI.
    );

    // Calling niraAuthorize is not strictly required because
    // NiraClient will call it internally whenever necessary.
    //
    // However, the internal calls to niraAuthorize will retry for up to
    // NIRA_INTERNAL_REQUEST_RETRY_TIME_TOTAL seconds (see niraclient.h),
    // which is 1000 seconds by default.
    //
    // Therefore, it is STRONGLY recommended to call this function just after a user provides
    // their key id and secret within your UI, so you can provide an error message immediately
    // if they've made a typo or entry mistake.
    //
    // It is also strongly recommended to call this function just prior to any call to niraUploadAsset
    // if you're using a stored app key id and secret. If it fails, this allows you to inform the user
    // that their key id and secret was not accepted and that they may need to generate a new one.
    //
    // The second parameter controls the number of seconds to retry the authorization request in case of failure.
    // 10 is generally a good number.
    NiraStatus authStatus = niraAuthorize(niraClient, 10);
    if (NIRA_SUCCESS != authStatus)
    {
        fprintf(stderr, "Error Message:\n%s\nError Detail:\n%s\n", niraGetErrorMessage(niraClient), niraGetErrorDetail(niraClient));
        return -1;
    }

    // Provide an array of NiraAssetFile struct instances that will be passed to niraUploadAsset() below.
    // This is a very basic example consisting of an obj, mtl, and png.
    // If you allocate from the heap, it's best to zero initialize this prior to populating it.
    NiraAssetFile niraAssetFiles[] = {
        {
            "./assets/tpot.obj", // path: A path to the file. Note, it's probably best to use absolute paths, but if using relative directory, be sure the cwd of the process is correct.
            NIRA_FILETYPE_SCENE, // type: One of NIRA_FILETYPE_TEXTURE (for texture files), NIRA_FILETYPE_PHOTO (for source photos), NIRA_FILETYPE_SCENE (for .obj files), NIRA_FILETYPE_MATERIAL (for .mtl files), or NIRA_FILETYPE_AUTO (everything else).
            NULL,                // name: The name of the file, including its extension. Optional. If NULL, basename(path) is automatically used ("tpot.obj", in this case)
        },
        { "./assets/tpot.mtl", NIRA_FILETYPE_MATERIAL },
        { "./assets/blue.png", NIRA_FILETYPE_TEXTURE },
    };

    // startPrintProgressThread(): This is an example of implement an upload progress bar in your application:
    startPrintProgressThread(niraClient);
    // See the startPrintProgressThread() and printUploadProgress() definitions near the end of this file for details.

    const size_t assetFileCount = sizeof(niraAssetFiles) / sizeof(niraAssetFiles[0]);

    NiraStatus st = niraUploadAsset(niraClient
        , niraAssetFiles
        , assetFileCount
        , assetName
        , "photogrammetry" // Asset type. There are some circumstances where this should be changed based on user input (e.g. if the asset files have vertex colors and no textures, "sculpt" is more appropriate)
    );

    if (NIRA_SUCCESS != st)
    {
        fprintf(stderr, "Error Message:\n%s\nError Detail:\n%s\n", niraGetErrorMessage(niraClient), niraGetErrorDetail(niraClient));
        niraDeinit(niraClient);
        return -1;
    }

    fprintf(stderr, "Asset URL: %s\n", niraClient->assetUrl);

    niraDeinit(niraClient);

    return 0;
}

char *getRandomAssetName(char *_assetNameBuf, size_t _assetNameBufSize, const char *_assetNamePrefix)
{
    char uuidStr[40];
    uuidGenV4(uuidStr);
    snprintf(_assetNameBuf, _assetNameBufSize, "%s%.*s", _assetNamePrefix, 8, uuidStr);
    return 0;
}

// Showing an upload progress bar:
//
// If you'd like to provide a progress bar and/or other upload status information in your UI,
// you can start querying niraClient in a background thread any time after niraInit is called on it.
// For this example, we just fire up a background thread function printUploadProgress()
// that repeadly reads and prints progress information to the terminal.
// See the printUploadProgress() definition near the end of this file for more details.
//
// Note, tinycthread is only used to fire off a background thread function printUploadProgress()
// as an example of how one might implement an upload progress bar. In your own application,
// you will likely already have a place in your UI/threading code where it makes sense to query
// and update your UI based on the upload's progress, similar to how printUploadProgress() does it.
#include "tinycthread/tinycthread.h"
#include "tinycthread/tinycthread.c"

int startPrintProgressThread(NiraClient *_niraClient)
{
    thrd_t threadId;
    int err = thrd_create(&threadId, &printUploadProgress, (void*)_niraClient);
    return 0;
}

int printUploadProgress(void *_nc)
{
    NiraClient *niraClient = (NiraClient *)_nc;

    while (1)
    {
        fprintf(stderr, "Progress:\n");

        if (0 == niraClient->totalFileSize)
        {
            fprintf(stderr, "Gathering filesize information...\n");
        }
        else
        {
            if (niraClient->assetUrl[0])
            {
                // The asset URL will become available fairly early in the upload process.
                // From a UX standpoint, it's usually best to provide the url to the user after
                // niraUploadAsset returns successfully.
                // That said, it is a valid URL as soon as niraClient->assetUrl[0] is non-zero,
                // and viewing the URL prior to completion will display a page informing the user
                // that the asset is still being uploaded or processed.
                fprintf(stderr, "\tAsset URL: %s\n", niraClient->assetUrl);
            }

            size_t totalFileSize = (niraClient->totalFileSize ? niraClient->totalFileSize : 1); // Prevent div0
            float progress = (float) niraClient->totalBytesCompleted / totalFileSize;

            // In fairly rare edge cases involving the bookkeeping after many HTTP request
            // failure/retry cycles, it cannot be guaranteed to be < 1.0, so if using this
            // value for a progress bar, be sure to clamp it:
            if (progress > 1.0) { progress = 1.0; };

            // A very simplistic progress bar :)
            for (size_t pp = 0, pn = (size_t)(progress*30); pp < pn; pp++) { fprintf(stderr, "="); }
            fprintf(stderr, "\n");

            fprintf(stderr, "\t%% complete: %f\n", progress * 100);

            // In fairly rare edge cases involving the bookkeeping after many HTTP request
            // failure/retry cycles, this cannot be guaranteed to be > 0, so be sure to clamp it
            // unless you wish to display a negative value:
            size_t bytesRemaining = (niraClient->totalFileSize - niraClient->totalBytesCompleted) > 0 ? (niraClient->totalFileSize - niraClient->totalBytesCompleted) : 0;

            fprintf(stderr, "\tBytes complete: %zd\n", (size_t)niraClient->totalBytesCompleted);
            fprintf(stderr, "\tBytes remaining: %zd\n", bytesRemaining);
            fprintf(stderr, "\tTotal file size: %zd\n", niraClient->totalFileSize);

            fprintf(stderr, "\tFiles comlete: %d\n", niraClient->totalFilesCompleted);
            fprintf(stderr, "\tTotal file count: %zd\n", niraClient->totalFileCount);
        }

        ///////////////////////////////////////////
        // niraClient->failingRequestCount:
        //
        // This will be true if NiraClient had one or more HTTP requests fail and is currently retrying the request.
        //
        // Some examples of why requests would be retrying:
        // 1. The user's internet connection is currently down (Wifi network disconnected, etc).
        //    NiraClient will keep retrying up to `NIRA_INTERNAL_REQUEST_RETRY_TIME_TOTAL` seconds for their connection to return (see niraclient.h).
        // 2. There was a temporary blip on our server or on a router on the way to our server. This does happen from time to time, which is why we retry.
        //
        // Somewhere underneath the progress bar area of your UI, it can be nice to include a message similar to the one shown below to let the user know something is unusual.
        // For example, if they accidentally disconnected their wifi, this warns them and gives them a chance to recover it.
        if (niraClient->failingRequestCount)
        {
            fprintf(stderr, "\tA networking issue was encountered while uploading. Automatically retrying...\n");
        }

        struct timespec sleepTs = {0, 1e8}; // 100ms
        thrd_sleep(&sleepTs, NULL);
    }

    return 0;
}
