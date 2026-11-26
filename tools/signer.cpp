// Ключ и подпись обновлений.
//   signer genkey <private.key> <src/update_key.hpp>   - новый ключ (закрытый - только у автора)
//   signer sign <private.key> <file> <version> <out.sig>
//   signer verify <src/update_key.hpp-blob.bin> ...    - не нужно: проверку делает тест обновления
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

#include "../src/signing.hpp"

using signing::Bytes;

static bool ReadFile(const char* path, Bytes& out) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	out.assign(std::istreambuf_iterator<char>(f), {});
	return true;
}

static bool WriteFile(const char* path, const void* data, size_t n) {
	std::ofstream f(path, std::ios::binary);
	f.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
	return static_cast<bool>(f);
}

int main(int argc, char** argv) {
	if (argc == 4 && !strcmp(argv[1], "genkey")) {
		if (std::ifstream(argv[2])) return fprintf(stderr, "%s already exists, not overwriting\n", argv[2]), 1;
		Bytes priv, pub;
		if (!signing::GenerateKey(priv, pub) || !WriteFile(argv[2], priv.data(), priv.size())) return fprintf(stderr, "genkey failed\n"), 1;
		std::string h = "#pragma once\n// Открытый ключ подписи обновлений (BCRYPT_ECCPUBLIC_BLOB, P-256). Сгенерирован tools/signer.\n"
			"#include <cstdint>\ninline constexpr uint8_t kUpdatePublicKey[] = {";
		char b[8];
		for (size_t i = 0; i < pub.size(); ++i) {
			snprintf(b, sizeof(b), "%s0x%02X", i ? ", " : "", pub[i]);
			h += b;
		}
		h += "};\n";
		if (!WriteFile(argv[3], h.data(), h.size())) return fprintf(stderr, "can't write %s\n", argv[3]), 1;
		printf("key written: %s (keep private), public key: %s\n", argv[2], argv[3]);
		return 0;
	}
	if (argc == 6 && !strcmp(argv[1], "sign")) {
		Bytes priv, data, sig;
		const unsigned long version = strtoul(argv[4], nullptr, 10);
		if (!ReadFile(argv[2], priv) || !ReadFile(argv[3], data) || !version) return fprintf(stderr, "bad input\n"), 1;
		if (!signing::Sign(priv, data, static_cast<uint32_t>(version), sig) || !WriteFile(argv[5], sig.data(), sig.size()))
			return fprintf(stderr, "sign failed\n"), 1;
		printf("signed %s as version %lu\n", argv[3], version);
		return 0;
	}
	fprintf(stderr, "usage: signer genkey <private.key> <update_key.hpp> | signer sign <private.key> <file> <version> <out.sig>\n");
	return 2;
}
