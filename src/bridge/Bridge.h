#pragma once

#include <functional>
#include <utility>

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>

namespace ncm {

class Bridge final : public QObject
{
    Q_OBJECT

public:
    using Reply = std::function<void(QJsonArray)>;
    using Handler = std::function<void(const QJsonArray &, Reply)>;

    struct Caller
    {
        QString window;
        QString document;
    };

    explicit Bridge(QObject *parent = nullptr);

    void addHandler(const QString &method, Handler handler);
    void call(const Caller &, const QString &method, const QJsonArray &args, Reply reply);
    QString callingWindow() const { return caller_.window; }
    QString callingDocument() const { return caller_.document; }

    void publish(const QString &name, const QJsonArray &args = {});
    void publishTo(const QString &target, const QString &name, const QJsonArray &args = {});

    static QJsonArray failure(const QString &code, const QString &message);

signals:
    void event(const QString &target, const QString &name, const QJsonArray &args);
    void invoked(const QString &method, const QJsonArray &args);

private:
    QHash<QString, Handler> handlers_;
    Caller caller_;
};

inline QString stringArg(const QJsonArray &a, int i)
{
    const auto v = a.at(i);
    return v.isString() ? v.toString() : v.toVariant().toString();
}

inline double numberArg(const QJsonArray &a, int i, double fallback = 0)
{
    bool ok = false;
    const auto d = a.at(i).toVariant().toDouble(&ok);
    return ok ? d : fallback;
}

inline bool boolArg(const QJsonArray &a, int i)
{
    const auto v = a.at(i);
    if (v.isBool())
        return v.toBool();
    if (v.isString())
        return v.toString() == "true" || v.toString() == "1";
    return v.toDouble() != 0;
}

}
