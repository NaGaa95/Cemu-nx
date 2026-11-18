// SHA-1 for Cemu's FetchShader.cpp through the EVP calls it makes (../shim/openssl/evp.h), computed with Windows'
// CNG (bcrypt). The digest only names a fetch shader's Vulkan pipeline state; it never reaches the GLSL.

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <windows.h>
#include <bcrypt.h>

#include <cstring>
#include <vector>

struct evp_md_ctx_st
{
	std::vector<unsigned char> data;
};

struct evp_md_st
{
	int unused;
};

extern "C"
{
	EVP_MD_CTX* EVP_MD_CTX_new(void)
	{
		return new evp_md_ctx_st();
	}

	void EVP_MD_CTX_free(EVP_MD_CTX* ctx)
	{
		delete ctx;
	}

	const EVP_MD* EVP_sha1(void)
	{
		static const evp_md_st sha1{};
		return &sha1;
	}

	int EVP_DigestInit(EVP_MD_CTX* ctx, const EVP_MD*)
	{
		ctx->data.clear();
		return 1;
	}

	int EVP_DigestUpdate(EVP_MD_CTX* ctx, const void* data, size_t count)
	{
		const auto* bytes = static_cast<const unsigned char*>(data);
		ctx->data.insert(ctx->data.end(), bytes, bytes + count);
		return 1;
	}

	int EVP_DigestFinal_ex(EVP_MD_CTX* ctx, unsigned char* md, unsigned int* size)
	{
		unsigned char digest[SHA_DIGEST_LENGTH] = {};
		const NTSTATUS status = BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, ctx->data.data(),
			static_cast<ULONG>(ctx->data.size()), digest, sizeof(digest));
		std::memcpy(md, digest, sizeof(digest));
		if (size)
			*size = SHA_DIGEST_LENGTH;
		return status >= 0 ? 1 : 0;
	}
}
