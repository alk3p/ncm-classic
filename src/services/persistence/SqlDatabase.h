#pragma once

#include <QHash>
#include <QJsonArray>
#include <QString>
#include <QThread>

struct sqlite3;

namespace ncm {

struct SqlResult
{
    int error = 0;
    QString message;
    QJsonArray rows;
    QJsonArray positionalRows;
    bool modified = false;

    explicit operator bool() const { return error == 0; }
};

class SqlDatabase
{
public:
    explicit SqlDatabase(const QString &path, const QString &pinyinDictionary = {});
    ~SqlDatabase();
    SqlDatabase(const SqlDatabase &) = delete;
    SqlDatabase &operator=(const SqlDatabase &) = delete;

    SqlResult execute(const QString &sql, bool atomic = false);
    SqlResult execute(const QStringList &sql, bool atomic = false);
    SqlResult initialize(bool offline);
    void interrupt() noexcept;
    bool isOpen() const { return db_ != nullptr; }
    static QString quote(const QString &s);

private:
    sqlite3 *db_ = nullptr;
    QString openError_;
    QThread *owner_;
    QHash<char32_t, QString> pinyin_;

    SqlResult exec(const QStringList &sql);
    QString sortKey(const QString &s) const;
    static int ascending(void *, int, const void *, int, const void *);
    static int descending(void *, int, const void *, int, const void *);
};

}
