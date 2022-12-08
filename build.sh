# Build everything as one translation unit:
#gcc -msse4 -maes -I. -Icurl/build/linux64/include -Imbedtls/include -o example_usage example_usage.c curl/build/linux64/lib/libcurl.a mbedtls/library/libmbedtls.a  mbedtls/library/libmbedx509.a mbedtls/library/libmbedcrypto.a -pthread

# Or build niraclient.c as a separate translation unit and link:
clang -fopenmp -O2 -msse4 -maes -I. -Icurl/build/linux64/include -Imbedtls/include -o niraclient.o -c niraclient.c
clang -fopenmp -O2 -msse4 -maes -I. -Icurl/build/linux64/include -Imbedtls/include -o example_usage example_usage.c niraclient.o curl/build/linux64/lib/libcurl.a mbedtls/library/libmbedtls.a  mbedtls/library/libmbedx509.a mbedtls/library/libmbedcrypto.a -pthread
