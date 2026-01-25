# This expects libcurl in your library path and include path. Please note, not all linux distributions will have
# libcurl installed by default. It is likely best to ship libcurl with your application.
# See build.bat for additional information about Zstandard and the NIRACLIENT_USE_INCLUDED_ZSTD_C define.
clang -fopenmp -O2 -msse4 -maes -I. -DNIRACLIENT_USE_INCLUDED_ZSTD_C -o example_usage example_usage.c niraclient.c -lcurl -pthread

# Build everything as one translation unit:
#gcc -msse4 -maes -I. -Icurl/build/linux64/include -Imbedtls/include -o example_usage example_usage.c curl/build/linux64/lib/libcurl.a mbedtls/library/libmbedtls.a  mbedtls/library/libmbedx509.a mbedtls/library/libmbedcrypto.a -pthread

# Or build niraclient.c as a separate translation unit and link:
#clang -fopenmp -O2 -msse4 -maes -I. -Icurl/build/linux64/include -Imbedtls/include -o niraclient.o -c niraclient.c
#clang -fopenmp -O2 -msse4 -maes -I. -Icurl/build/linux64/include -Imbedtls/include -o example_usage example_usage.c niraclient.o curl/build/linux64/lib/libcurl.a mbedtls/library/libmbedtls.a  mbedtls/library/libmbedx509.a mbedtls/library/libmbedcrypto.a -pthread


# For a locally compiled, statically linked copy of curl in the curl subdirectory:
#clang -fopenmp -O2 -msse4 -maes -I. -I./curl/build/linux64/include -o niraclient.o -c niraclient.c
#clang -fopenmp -O2 -msse4 -maes -I. -I./curl/build/linux64/include -o example_usage example_usage.c niraclient.o curl/build/linux64/lib/libcurl.a -pthread -lcrypto -lssl -lnghttp2 -lbrotlidec -lbrotlicommon
