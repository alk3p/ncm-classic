#include "Crypto.h"

#include <algorithm>

#include <QCryptographicHash>
#include <QRandomGenerator>

#include <bearssl.h>

namespace ncm::crypto {

namespace {

constexpr qsizetype aesBlockSize = 16;

QByteArray pkcs7Pad(QByteArray bytes)
{
    const auto padding = aesBlockSize - bytes.size() % aesBlockSize;
    bytes.append(padding, char(padding));
    return bytes;
}

std::expected<QString, QString> linuxForward(const QString &s)
{
    return aesEcbEncrypt(s.toUtf8(), "rFgB&h#%2?^eDg:Q").transform([](const QByteArray &bytes) {
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

std::expected<QByteArray, QString> aesEcbEncrypt(const QByteArray &plain, const QByteArray &key)
{
    if (key.size() != aesBlockSize)
        return std::unexpected(QStringLiteral("AES128 requires 16-byte key"));
    auto out = pkcs7Pad(plain);
    br_aes_ct_cbcenc_keys ctx;
    br_aes_ct_cbcenc_init(&ctx, key.constData(), key.size());
    // A fresh zero IV for each block makes CBC equivalent to ECB.
    for (qsizetype offset = 0; offset < out.size(); offset += aesBlockSize) {
        char iv[aesBlockSize]{};
        br_aes_ct_cbcenc_run(&ctx, iv, out.data() + offset, aesBlockSize);
    }
    return out;
}

std::expected<QByteArray, QString> aesEcbDecrypt(const QByteArray &ciphertext, const QByteArray &key)
{
    if (key.size() != aesBlockSize)
        return std::unexpected(QStringLiteral("AES128 requires 16-byte key"));
    if (ciphertext.isEmpty() || ciphertext.size() % aesBlockSize != 0)
        return std::unexpected(QStringLiteral("Invalid AES ciphertext length"));
    auto out = ciphertext;
    br_aes_ct_cbcdec_keys ctx;
    br_aes_ct_cbcdec_init(&ctx, key.constData(), key.size());
    char iv[aesBlockSize]{};
    auto *plain = reinterpret_cast<unsigned char *>(out.data());
    br_aes_ct_cbcdec_run(&ctx, iv, plain, out.size());

    const auto *orig = reinterpret_cast<const unsigned char *>(ciphertext.constData());
    for (qsizetype offset = aesBlockSize; offset < out.size(); ++offset)
        plain[offset] ^= orig[offset - aesBlockSize];

    const auto padding = static_cast<unsigned char>(out.back());
    if (padding == 0 || padding > aesBlockSize
        || !std::all_of(out.cend() - padding, out.cend(), [padding](char byte) {
               return byte == padding;
           }))
        return std::unexpected(QStringLiteral("Invalid AES padding"));
    out.chop(padding);
    return out;
}

std::expected<QString, QString> eapi(const QString &path, const QString &json)
{
    const auto p = path.toUtf8(), j = json.toUtf8();
    const auto hash = QCryptographicHash::hash(
                          "nobody" + p + "use" + j + "md5forencrypt", QCryptographicHash::Md5)
                          .toHex();
    return aesEcbEncrypt(p + "-36cd479b6b5-" + j + "-36cd479b6b5-" + hash, "e82ckenh8dichen8")
        .transform(
            [](const QByteArray &bytes) { return QString::fromLatin1(bytes.toHex().toUpper()); });
}

std::expected<QJsonObject, QString> weapi(const QByteArray &json)
{
    QByteArray secret;
    const QByteArray chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (qsizetype i = 0; i < aesBlockSize; ++i)
        secret.append(chars[QRandomGenerator::system()->bounded(chars.size())]);
    auto cbc = [](const QByteArray &input, const QByteArray &key) {
        auto out = pkcs7Pad(input);
        char iv[] = "0102030405060708";
        br_aes_ct_cbcenc_keys ctx;
        br_aes_ct_cbcenc_init(&ctx, key.constData(), key.size());
        br_aes_ct_cbcenc_run(&ctx, iv, out.data(), out.size());
        return out.toBase64();
    };
    // openssl rsa -pubin -in weapi.pem -noout -modulus
    //
    // -----BEGIN PUBLIC KEY-----
    // MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDgtQn2JZ34ZC28NWYpAUd98iZ3
    // 7BUrX/aKzmFbt7clFSs6sXqHauqKWqdtLkF2KexO40H1YTX8z2lSgBBOAxLsvakl
    // V8k4cBFK9snQXE9/DDaFt6Rr7iVZMldczhC0JNgTz+SHXT6CBHuX3e9SdB1Ua44o
    // ncaTWz7OBGLbCiK45wIDAQAB
    // -----END PUBLIC KEY-----
    auto modulus = QByteArray::fromHex(
        "e0b509f6259df8642dbc35662901477df22677ec152b5ff68ace615bb7b72515"
        "2b3ab17a876aea8a5aa76d2e417629ec4ee341f56135fccf695280104e0312ec"
        "bda92557c93870114af6c9d05c4f7f0c3685b7a46bee255932575cce10b424d8"
        "13cfe4875d3e82047b97ddef52741d546b8e289dc6935b3ece0462db0a22b8e7");
    unsigned char exponent[] = {0x01, 0x00, 0x01}; // 65537
    const br_rsa_public_key key{
        .n = reinterpret_cast<unsigned char *>(modulus.data()),
        .nlen = static_cast<size_t>(modulus.size()),
        .e = exponent,
        .elen = sizeof(exponent)};
    QByteArray encSecKey(modulus.size(), '\0');
    // WEAPI places the reversed secret at the end of the raw RSA operand.
    std::reverse_copy(secret.cbegin(), secret.cend(), encSecKey.end() - secret.size());
    if (!br_rsa_i31_public(
            reinterpret_cast<unsigned char *>(encSecKey.data()), encSecKey.size(), &key))
        return std::unexpected(QStringLiteral("WEAPI RSA failed"));
    const auto params = cbc(cbc(json, "0CoJUm6Qyw8W8jud"), secret);
    return QJsonObject{
        {"params", QString::fromLatin1(params)},
        {"encSecKey", QString::fromLatin1(encSecKey.toHex())}};
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
