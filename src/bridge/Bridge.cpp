#include "Bridge.h"

namespace ncm {

Bridge::Bridge(QObject *parent)
    : QObject(parent)
{}

void Bridge::addHandler(const QString &method, Handler handler)
{
    if (handlers_.contains(method))
        qFatal("Duplicate bridge handler: %s", qPrintable(method));
    handlers_.insert(method, std::move(handler));
}

void Bridge::call(const Caller &caller, const QString &method, const QJsonArray &args, Reply reply)
{
    emit invoked(method, args);
    const auto it = handlers_.constFind(method);
    if (it == handlers_.cend()) {
        reply(failure("unsupported_method", method));
        return;
    }
    const auto previous = std::exchange(caller_, caller);
    (*it)(args, std::move(reply));
    caller_ = previous;
}

void Bridge::publish(const QString &name, const QJsonArray &args)
{
    emit event(QString(), name, args);
}

void Bridge::publishTo(const QString &target, const QString &name, const QJsonArray &args)
{
    emit event(target, name, args);
}

QJsonArray Bridge::failure(const QString &code, const QString &message)
{
    return {QJsonObject{{"success", false}, {"error", code}, {"message", message}}};
}

}
