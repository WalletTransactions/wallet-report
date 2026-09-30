// Урезанная копия util/crypto.hpp из google-auth: этот сервис только
// ПРОВЕРЯЕТ подпись access-токена (RS256, по публичному ключу с JWKS
// auth-сервиса — см. util/jwks.hpp) — не подписывает свои и не хранит
// никакого приватного ключа/секрета вообще.
#pragma once

#include <string>
#include <vector>

namespace crypto {

// SHA-256 содержимого — используется для адресации blob по хешу (см.
// wallet-api/schema/table/blob.sql, org/docs/TODO_mobile_sync.md, "Иконки"):
// сервер сам считает хеш загруженных байт, не доверяя тому, что мог бы
// прислать клиент. Копия google-auth/src/util/crypto.hpp::sha256 (та нужна
// там для PKCE) — не общий заголовок между сервисами: каждый С++-сервис в
// этом проекте самодостаточен, без общей библиотеки-прослойки.
std::vector<unsigned char> sha256(const std::string& data);

// Проверяет подпись RSASSA-PKCS1-v1_5 SHA-256 по сырым модулю/экспоненте
// RSA-ключа — ровно в том формате, в котором их отдаёт JWKS (base64url,
// уже декодированный в байты вызывающим кодом). Возвращает false при любой
// ошибке (битый ключ, неверная подпись и т.п.), исключений не бросает.
bool rsa_sha256_verify(const std::vector<unsigned char>& n,
                        const std::vector<unsigned char>& e,
                        const std::string& signed_data,
                        const std::vector<unsigned char>& signature);

} // namespace crypto
