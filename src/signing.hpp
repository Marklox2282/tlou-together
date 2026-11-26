#pragma once
// Подпись обновлений: ECDSA P-256 над SHA-256(файл + "\n" + номер версии) через bcrypt Windows.
// Номер версии входит в подпись, чтобы старую подписанную сборку нельзя было выдать за новую.

#include <cstdint>
#include <string>
#include <vector>

namespace signing {

using Bytes = std::vector<uint8_t>;

bool Sha256(const Bytes& data, uint32_t version, uint8_t out[32]);
// publicBlob - BCRYPT_ECCPUBLIC_BLOB (P-256), signature - 64 байта r||s.
bool Verify(const Bytes& publicBlob, const Bytes& data, uint32_t version, const Bytes& signature);
// Для утилиты выпуска.
bool GenerateKey(Bytes& privateBlob, Bytes& publicBlob);
bool Sign(const Bytes& privateBlob, const Bytes& data, uint32_t version, Bytes& signature);

}  // namespace signing
