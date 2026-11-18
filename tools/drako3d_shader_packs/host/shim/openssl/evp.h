// The part of OpenSSL's EVP digest API that Cemu's FetchShader.cpp uses (SHA-1 of a fetch shader's attribute
// offsets), implemented in ../../src/openssl_shim.cpp with Windows' CNG.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct evp_md_ctx_st EVP_MD_CTX;
typedef struct evp_md_st EVP_MD;

EVP_MD_CTX* EVP_MD_CTX_new(void);
void EVP_MD_CTX_free(EVP_MD_CTX* ctx);
const EVP_MD* EVP_sha1(void);
int EVP_DigestInit(EVP_MD_CTX* ctx, const EVP_MD* type);
int EVP_DigestUpdate(EVP_MD_CTX* ctx, const void* data, size_t count);
int EVP_DigestFinal_ex(EVP_MD_CTX* ctx, unsigned char* md, unsigned int* size);

#ifdef __cplusplus
}
#endif
