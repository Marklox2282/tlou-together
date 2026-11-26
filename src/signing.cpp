#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include "signing.hpp"

namespace signing {

namespace {

struct Alg {
	BCRYPT_ALG_HANDLE h = nullptr;
	explicit Alg(LPCWSTR id) { BCryptOpenAlgorithmProvider(&h, id, nullptr, 0); }
	~Alg() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
};

struct Key {
	BCRYPT_KEY_HANDLE h = nullptr;
	~Key() { if (h) BCryptDestroyKey(h); }
};

}  // namespace

bool Sha256(const Bytes& data, uint32_t version, uint8_t out[32]) {
	const std::string suffix = "\n" + std::to_string(version);
	BCRYPT_HASH_HANDLE hash = nullptr;
	const bool ok = BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0)) &&
		BCRYPT_SUCCESS(BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0)) &&
		BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(suffix.data())),
			static_cast<ULONG>(suffix.size()), 0)) &&
		BCRYPT_SUCCESS(BCryptFinishHash(hash, out, 32, 0));
	if (hash) BCryptDestroyHash(hash);
	return ok;
}

bool Verify(const Bytes& publicBlob, const Bytes& data, uint32_t version, const Bytes& signature) {
	uint8_t digest[32];
	Alg alg(BCRYPT_ECDSA_P256_ALGORITHM);
	Key key;
	return signature.size() == 64 && alg.h && Sha256(data, version, digest) &&
		BCRYPT_SUCCESS(BCryptImportKeyPair(alg.h, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key.h,
			const_cast<PUCHAR>(publicBlob.data()), static_cast<ULONG>(publicBlob.size()), 0)) &&
		BCRYPT_SUCCESS(BCryptVerifySignature(key.h, nullptr, digest, sizeof(digest),
			const_cast<PUCHAR>(signature.data()), static_cast<ULONG>(signature.size()), 0));
}

bool GenerateKey(Bytes& privateBlob, Bytes& publicBlob) {
	Alg alg(BCRYPT_ECDSA_P256_ALGORITHM);
	Key key;
	ULONG n = 0;
	if (!alg.h || !BCRYPT_SUCCESS(BCryptGenerateKeyPair(alg.h, &key.h, 256, 0)) ||
		!BCRYPT_SUCCESS(BCryptFinalizeKeyPair(key.h, 0)))
		return false;
	for (auto [type, out] : { std::pair{ BCRYPT_ECCPRIVATE_BLOB, &privateBlob }, std::pair{ BCRYPT_ECCPUBLIC_BLOB, &publicBlob } }) {
		if (!BCRYPT_SUCCESS(BCryptExportKey(key.h, nullptr, type, nullptr, 0, &n, 0))) return false;
		out->resize(n);
		if (!BCRYPT_SUCCESS(BCryptExportKey(key.h, nullptr, type, out->data(), n, &n, 0))) return false;
	}
	return true;
}

bool Sign(const Bytes& privateBlob, const Bytes& data, uint32_t version, Bytes& signature) {
	uint8_t digest[32];
	Alg alg(BCRYPT_ECDSA_P256_ALGORITHM);
	Key key;
	ULONG n = 0;
	if (!alg.h || !Sha256(data, version, digest) ||
		!BCRYPT_SUCCESS(BCryptImportKeyPair(alg.h, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key.h,
			const_cast<PUCHAR>(privateBlob.data()), static_cast<ULONG>(privateBlob.size()), 0)) ||
		!BCRYPT_SUCCESS(BCryptSignHash(key.h, nullptr, digest, sizeof(digest), nullptr, 0, &n, 0)))
		return false;
	signature.resize(n);
	return BCRYPT_SUCCESS(BCryptSignHash(key.h, nullptr, digest, sizeof(digest), signature.data(), n, &n, 0));
}

}  // namespace signing
