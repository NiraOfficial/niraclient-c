#include "niraclient.h"
#include <stdlib.h> // Used for getenv()

int printUploadProgress(void *_nc);
int startPrintProgressThread(NiraClient *_niraClient);
char *getRandomAssetName(char *_assetNameBuf, size_t _assetNameBufSize, const char *_assetNamePrefix);

int32_t main()
{
    // It's a good idea to display the Nira client version string in the UI.
    // This is useful for debugging and customer support purposes.
    // To retrieve the version string, call niraGetClientVersion().
    //
    // It's nice to display this somewhere prominent in the uploader UI,
    // both just before and during the upload process.
    // This way, it is visible in screenshots and screencaptures that users
    // may provide in support requests.
    fprintf(stderr, "Nira Uploader (%s)\n", niraGetClientVersion());

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

    char *appKeyId     = getenv("APP_KEY_ID_UPLOADTEST");
    char *appKeySecret = getenv("APP_KEY_SECRET_UPLOADTEST");

    // Calling setCredentials is required. Any NiraClient call that makes network requests will fail without a valid app key id and secret.
    niraSetCredentials(niraClient
      , appKeyId     // app key id: From the "App keys" area of the Nira UI.
      , appKeySecret // app key secret: From the "App keys" area of the Nira UI.
    );

    // niraAuthorize:
    //
    // The niraAuthorize function should be called just after a user provides their key id and secret
    // within your UI. If the function fails, you can provide an error message immediately,
    // letting the user know that they've most likely made a typo or entry mistake.
    //
    // It is also strongly recommended to call this function just prior to calling niraSetCoordsys and niraUploadAsset.
    // So the function call sequence prior to any upload should be like this:
    // niraAuthorize(...)
    // niraSetCoordsys(...) // If you want georeferencing
    // niraUploadAsset(...)
    //
    // This way, if the niraAuthorize call fails, this allows you to inform the user that their key id and secret was not
    // accepted and that they may need to generate a new one, instead of moving onto the niraSetCoordsys or
    // niraUploadAsset calls.
    //
    // The second parameter to niraAuthorize controls the number of seconds to retry the authorization
    // request in case of failure. 10 is generally a good number.
    NiraStatus authStatus = niraAuthorize(niraClient, /*request retry time in seconds:*/ 10);
    if (NIRA_SUCCESS != authStatus)
    {
        fprintf(stderr, "Error Message:\n%s\nError Detail:\n%s\n", niraGetErrorMessage(niraClient), niraGetErrorDetail(niraClient));
        return -1;
    }

    // niraSetCoordsys:
    //
    // This is an optional call, but is very important if you're uploading a georeferenced dataset.
    // When calling niraSetCoordsys, you must pass the epsg string that corresponds with how your geometry and camera data were saved.
    //
    // Using the epsg string for the UTM zone correspondong with the asset's geographic location works well here.
    // For example, "epsg:32632" is the UTM Zone covering Denmark, Netherlands, Germany, etc and "epsg:32617" is the UTM zone covering the east coast of the U.S.
    // Nira also supports just about any cartesian, ENU coordinate system, so if a use prefers to use a surveying coordinate system local to their country, this is fine.
    // For example, someone in Belgium may prefer "epsg:31370", which is not a UTM zone, but it is supported because it is cartesian and ENU.
    //
    // This function makes a network request to check whether the specified coordinate system is supported by Nira, and returns an error if it is not.
    //
    // If this function returns the status NIRA_ERROR_UNSUPPORTED_COORDINATE_SYSTEM, it means the provided coordinate system is not supported by Nira.
    // If this happens, it's best to display to the user the error message string returned by niraGetErrorMessage(niraClient), then
    // do not allow the user to upload the dataset.
    //
    // For reference, the error message string returned by niraGetErrorMessage(niraClient) will be something like:
    // "The coordinate system EPSG:4326 is not supported by Nira. Please upload data using a Cartesian coordinate system such as a UTM Zone".
    // In the error dialog presented to the user, underneath the error message, it's a good idea to give a hint how the user can change their coordinate system
    // within your application's UI in order to resolve the issue.
    //
    // As mentioned above, it's best to call niraSetCoordsys function just after niraAuthorize and just before calling niraUploadAsset, in a sequence like this:
    // niraAuthorize(...)
    // niraSetCoordsys(...) // If you want georeferencing
    // niraUploadAsset(...)
    //
    // The third parameter to niraSetCoordsys controls the number of seconds to retry the request in case of failure. 10 is generally a good number.
    //
    #if 0 // Use this code if your asset is georeferenced!
    NiraStatus coordsysStatus = niraSetCoordsys(niraClient, "epsg:32632", /*request retry time in seconds:*/ 10);
    // For reference, this will return an error because epsg:4326 is not cartesian and is thus unsupported by Nira:
    //NiraStatus coordsysStatus = niraSetCoordsys(niraClient, "epsg:4326", /*request retry time in seconds:*/ 10);
    if (NIRA_SUCCESS != coordsysStatus)
    {
        // In the case of NIRA_ERROR_UNSUPPORTED_COORDINATE_SYSTEM, you can provide the error message directly to the user.
        if (NIRA_ERROR_UNSUPPORTED_COORDINATE_SYSTEM == coordsysStatus)
        {
            fprintf(stderr, "%s\n", niraGetErrorMessage(niraClient));
        }
        else
        {
            fprintf(stderr, "Error Message:\n%s\nError Detail:\n%s\n", niraGetErrorMessage(niraClient), niraGetErrorDetail(niraClient));
        }
        return -1;
    }
    #endif

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
            // When niraClient->totalFileSize is 0, this means niraUploadAsset()
            // is still gathering filesizes, hashing files, and determining which
            // files the server already has, so the progress related variables
            // do not yet contain meaningful values. Therefore, you should not
            // display any progress bar updates until the totalFileSize becomes
            // non-zero.
            //
            // During this time, you can display something like "Preparing files for upload..."
            fprintf(stderr, "Preparing files for upload...\n");
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
