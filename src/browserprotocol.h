#ifndef QDM_BROWSERPROTOCOL_H
#define QDM_BROWSERPROTOCOL_H
/** @file browserprotocol.h
 * @brief 桌面与 Native Host 共用的协议常量、系统随机数及 DPAPI 配对凭据。
 * 不依赖下载引擎和界面。密钥不输出日志，不修改 Windows 证书或系统信任。
 */
#include <QtCore>
namespace BrowserProtocol {
const char ExtensionId[] = "nkblcdnjneadmggjpnjajlbebpbmhjpj";
const char HostName[] = "com.qdm.download_manager";
bool writeJson(const QString &path, const QJsonObject &value, QString *error);
bool loadToken(const QString &directory, QByteArray *token, bool create, QString *error);
bool connection(const QString &directory, quint16 *port, QByteArray *token, QString *error);
bool validExtension(const QByteArray &id);
QString cleanFileName(const QString &suggestion);
} // namespace BrowserProtocol
#endif
