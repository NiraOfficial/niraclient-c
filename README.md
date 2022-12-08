## niraclient-c

niraclient-c is a library written in C that handles many of the details of uploading an asset to Nira.
It is primarily useful when integrating Nira uploading capability into an existing product or service.
If you have any questions, please reach out to contact@nira.app

### To get started:

To build the example executable on Windows:
1. Run build.bat. It will print instructions on how to include libcurl, which is the only external dependency.
2. Once libcurl is sorted out, run build.bat again, which should produce example\_usage.exe.
3. Running example\_usage.exe will upload the test assets to our test organization (uploadtest.nira.app).
4. For API usage documention, see the source code of example\_usage.c


### License and Legal

niraclient-c uses the MIT license  
Everything in the niralcient-c repo is in source code form. No static or dynamic libraries are included in this repository.  
niraclient-c has a single external dependency called libcurl which is also permissvely licensed.  
libcurl is not included in this repository, but it is available in source code form from https://curl.se/  
niraclient-c includes some other permissively licensed source code. See the NOTICE file for a full list.  
