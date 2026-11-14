#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif
/** @file browserprotocol.cpp @brief 文件仅供当前 Windows 用户使用，Token 经 DPAPI 保护。 */
#include "browserprotocol.h"
#include <windows.h>
#include <wincrypt.h>
namespace BrowserProtocol {
/** @brief 通过 QSaveFile 原子替换本机元信息，避免崩溃后读到半份 JSON。失败填充
 * error；不得将密钥写到日志。 */
bool writeJson(const QString &path, const QJsonObject &value, QString *error) {
    QSaveFile file(path);
    const QByteArray data = QJsonDocument(value).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}
/** @brief 读取当前用户 DPAPI 密钥；create=true 且尚不存在时生成 32 字节系统随机数。DPAPI 输出由
 * LocalFree 释放，不跨运行库直接 free。 */
bool loadToken(const QString &dir, QByteArray *token, bool create, QString *error) {
    QFile file(dir + "/browser-secret.bin");
    DATA_BLOB out = {0, 0};
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) {
            *error = file.errorString();
            return false;
        }
        QByteArray encoded = file.read(16384);
        DATA_BLOB in = {DWORD(encoded.size()), reinterpret_cast<BYTE *>(encoded.data())};
        if (!CryptUnprotectData(&in, 0, 0, 0, 0, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            *error = QString::fromUtf8("无法解密浏览器配对信息");
            return false;
        }
        *token = QByteArray(reinterpret_cast<const char *>(out.pbData), int(out.cbData));
        LocalFree(out.pbData);
        if (token->size() != 64) {
            *error = QString::fromUtf8("配对信息格式错误");
            return false;
        }
        return true;
    }
    if (!create) {
        *error = QString::fromUtf8("请先启动 QDM 桌面程序");
        return false;
    }
    HCRYPTPROV provider = 0;
    BYTE bytes[32];
    if (!CryptAcquireContextW(&provider, 0, 0, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        *error = "RNG initialization failed";
        return false;
    }
    bool randomOk = CryptGenRandom(provider, sizeof(bytes), bytes) != FALSE;
    CryptReleaseContext(provider, 0);
    if (!randomOk) {
        *error = "RNG failed";
        return false;
    }
    *token = QByteArray(reinterpret_cast<const char *>(bytes), sizeof(bytes)).toHex();
    DATA_BLOB in = {DWORD(token->size()), reinterpret_cast<BYTE *>(token->data())};
    if (!CryptProtectData(&in, L"QDM Browser Bridge", 0, 0, 0, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        *error = "DPAPI failed";
        return false;
    }
    const QByteArray encrypted(reinterpret_cast<const char *>(out.pbData), int(out.cbData));
    LocalFree(out.pbData);
    QSaveFile saved(file.fileName());
    if (!saved.open(QIODevice::WriteOnly) || saved.write(encrypted) != encrypted.size() ||
        !saved.commit()) {
        *error = saved.errorString();
        return false;
    }
    return true;
}
/** @brief 读取同一个数据目录中的端口发现文件，并解密配对密钥。这里只返回 loopback
 * 连接参数，不接受文件中的任意远程主机地址。 */
bool connection(const QString &dir, quint16 *port, QByteArray *token, QString *error) {
    QFile file(dir + "/browser-endpoint.json");
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QString::fromUtf8("请先启动支持扩展的 QDM 0.2.0 或更高版本");
        return false;
    }
    QJsonObject info = QJsonDocument::fromJson(file.read(16384)).object();
    int value = info.value("port").toInt();
    if (value < 1 || value > 65535) {
        *error = QString::fromUtf8("桌面连接信息无效");
        return false;
    }
    *port = quint16(value);
    return loadToken(dir, token, false, error);
}
/** @brief 严格匹配本项目固定扩展 ID；公开 key 用于保持 ID 稳定，真正请求授权仍依赖随机 Token。 */
bool validExtension(const QByteArray &id) {
    return id == QByteArray(ExtensionId);
}
/** @brief
 * 清理浏览器给出的不可信文件名。只保留最后一级名称，替换非法字符/设备名并限制长度；目录始终由桌面
 * profile 决定。 */
QString cleanFileName(const QString &input) {
    QString name = input;
    name.replace('\\', '/');
    name = name.section('/', -1);
    name.replace(QRegExp("[<>:\"/\\\\|?*\\x00-\\x1f]"), "_");
    name = name.trimmed();
    while (name.endsWith('.') || name.endsWith(' '))
        name.chop(1);
    if (name.isEmpty() || name == "." || name == "..")
        name = "download.bin";
    QRegExp reserved("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\\..*)?$", Qt::CaseInsensitive);
    if (reserved.exactMatch(name))
        name.prepend('_');
    if (name.size() > 140) {
        QString suffix = QFileInfo(name).suffix().left(16);
        name = name.left(120) + (suffix.isEmpty() ? QString() : "." + suffix);
    }
    return name;
}
} // namespace BrowserProtocol
