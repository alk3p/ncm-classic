#include "Crypto.h"

#include <algorithm>
#include <memory>

#include <QCryptographicHash>
#include <QRandomGenerator>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

namespace ncm::crypto {

namespace {

std::expected<QByteArray, QString> aesEcb(const QByteArray &plain, const QByteArray &key)
{
    if (key.size() != 16)
        return std::unexpected(QStringLiteral("AES128 requires 16-byte key"));
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>
        ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    QByteArray out(plain.size() + 16, Qt::Uninitialized);
    int n = 0, tail = 0;
    if (!ctx
        || EVP_EncryptInit_ex(
               ctx.get(),
               EVP_aes_128_ecb(),
               nullptr,
               reinterpret_cast<const unsigned char *>(key.constData()),
               nullptr)
               != 1
        || EVP_EncryptUpdate(
               ctx.get(),
               reinterpret_cast<unsigned char *>(out.data()),
               &n,
               reinterpret_cast<const unsigned char *>(plain.constData()),
               plain.size())
               != 1
        || EVP_EncryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char *>(out.data() + n), &tail)
               != 1)
        return std::unexpected(QStringLiteral("AES encryption failed"));
    out.resize(n + tail);
    return out;
}

std::expected<QString, QString> linuxForward(const QString &s)
{
    return aesEcb(s.toUtf8(), "rFgB&h#%2?^eDg:Q").transform([](const QByteArray &bytes) {
        return QString::fromLatin1(bytes.toHex().toUpper());
    });
}

QString encryptId(const QString &s)
{
    auto a = s.toUtf8();
    const QByteArray key = "3go8&$8*3*3h0k(2)2";
    for (qsizetype i = 0; i < a.size(); ++i)
        a[i] = a[i] ^ key[i % key.size()];
    auto r = QCryptographicHash::hash(a, QCryptographicHash::Md5).toBase64();
    r.replace('/', '_');
    r.replace('+', '-');
    return QString::fromLatin1(r);
}

QString encodeAnonymousId(const QString &deviceId)
{
    if (deviceId.isEmpty())
        return {};
    auto result = (deviceId.toUtf8() + " " + encryptId(deviceId).toUtf8()).toBase64();
    result.replace('/', '_');
    result.replace('+', '-');
    return QString::fromLatin1(result);
}

}

std::expected<QString, QString> eapi(const QString &path, const QString &json)
{
    const auto p = path.toUtf8(), j = json.toUtf8();
    const auto hash = QCryptographicHash::hash(
                          "nobody" + p + "use" + j + "md5forencrypt", QCryptographicHash::Md5)
                          .toHex();
    return aesEcb(p + "-36cd479b6b5-" + j + "-36cd479b6b5-" + hash, "e82ckenh8dichen8")
        .transform(
            [](const QByteArray &bytes) { return QString::fromLatin1(bytes.toHex().toUpper()); });
}

std::expected<QJsonObject, QString> weapi(const QByteArray &json)
{
    QByteArray secret;
    const QByteArray chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int i = 0; i < 16; ++i)
        secret.append(chars[QRandomGenerator::system()->bounded(chars.size())]);
    auto cbc = [](const QByteArray &input,
                  const QByteArray &key) -> std::expected<QByteArray, QString> {
        std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>
            c(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
        QByteArray out(input.size() + 16, Qt::Uninitialized);
        int n = 0, m = 0;
        if (!c
            || EVP_EncryptInit_ex(
                   c.get(),
                   EVP_aes_128_cbc(),
                   nullptr,
                   reinterpret_cast<const unsigned char *>(key.constData()),
                   reinterpret_cast<const unsigned char *>("0102030405060708"))
                   != 1
            || EVP_EncryptUpdate(
                   c.get(),
                   reinterpret_cast<unsigned char *>(out.data()),
                   &n,
                   reinterpret_cast<const unsigned char *>(input.constData()),
                   input.size())
                   != 1
            || EVP_EncryptFinal_ex(c.get(), reinterpret_cast<unsigned char *>(out.data() + n), &m)
                   != 1)
            return std::unexpected(QStringLiteral("WEAPI AES failed"));
        out.resize(n + m);
        return out.toBase64();
    };
    static constexpr auto pem
        = "-----BEGIN PUBLIC "
          "KEY-----"
          "\nMIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDgtQn2JZ34ZC28NWYpAUd98iZ37BUrX/"
          "aKzmFbt7clFSs6sXqHauqKWqdtLkF2KexO40H1YTX8z2lSgBBOAxLsvaklV8k4cBFK9snQXE"
          "9/"
          "DDaFt6Rr7iVZMldczhC0JNgTz+"
          "SHXT6CBHuX3e9SdB1Ua44oncaTWz7OBGLbCiK45wIDAQAB\n-----END PUBLIC "
          "KEY-----\n";
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(pem, -1), BIO_free);
    if (!bio)
        return std::unexpected(QStringLiteral("WEAPI RSA key failed"));
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>
        key(PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    if (!key)
        return std::unexpected(QStringLiteral("WEAPI RSA key failed"));
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>
        ctx(EVP_PKEY_CTX_new(key.get(), nullptr), EVP_PKEY_CTX_free);
    QByteArray input(EVP_PKEY_get_size(key.get()), char(0));
    auto reverse = secret;
    std::reverse(reverse.begin(), reverse.end());
    input.replace(input.size() - reverse.size(), reverse.size(), reverse);
    size_t size = input.size();
    QByteArray out(size, Qt::Uninitialized);
    if (!ctx || EVP_PKEY_encrypt_init(ctx.get()) <= 0
        || EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_NO_PADDING) <= 0
        || EVP_PKEY_encrypt(
               ctx.get(),
               reinterpret_cast<unsigned char *>(out.data()),
               &size,
               reinterpret_cast<const unsigned char *>(input.constData()),
               input.size())
               <= 0)
        return std::unexpected(QStringLiteral("WEAPI RSA failed"));
    out.resize(size);
    auto params = cbc(json, "0CoJUm6Qyw8W8jud").and_then([&](const QByteArray &bytes) {
        return cbc(bytes, secret);
    });
    if (!params)
        return std::unexpected(params.error());
    return QJsonObject{
        {"params", QString::fromLatin1(*params)}, {"encSecKey", QString::fromLatin1(out.toHex())}};
}

std::expected<QJsonValue, QString> pure(const QString &name, const QJsonArray &args)
{
    if (args.size() != 1)
        return std::unexpected(QStringLiteral("Pure helper expects exactly one argument"));
    if (!args.at(0).isString()
        && !((name == "serialData" || name == "serialData2") && args.at(0).isArray()))
        return std::unexpected(
            QStringLiteral("Pure helper requires a string (EAPI serialData accepts [path,json])"));
    if (name == "encodeAnonymousId" || name == "encodeAnonymousId2")
        return encodeAnonymousId(args.at(0).toString());
    if (name == "encryptId")
        return encryptId(args.at(0).toVariant().toString());
    if (name == "serialData" || name == "serialData2") {
        const auto v = args.at(0);
        const auto a = v.toArray();
        auto result = v.isArray() ? eapi(a.at(0).toString(), a.at(1).toString())
                                  : linuxForward(v.toString());
        return result.transform([](const QString &encrypted) { return QJsonValue(encrypted); });
    }
    return std::unexpected("Unsupported pure helper: " + name);
}

}
