#include "SqlDatabase.h"

#include <utility>

#include <QFile>
#include <QJsonObject>
#include <QScopeGuard>
#include <QVariant>

#include <sqlite3.h>

namespace ncm {

SqlDatabase::SqlDatabase(const QString &path, const QString &dict)
    : owner_(QThread::currentThread())
{
    if (sqlite3_open_v2(
            path.toUtf8().constData(),
            &db_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
            nullptr)
        != SQLITE_OK) {
        openError_ = db_ ? QString::fromUtf8(sqlite3_errmsg(db_)) : "Cannot open database";
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }
    sqlite3_busy_timeout(db_, 1500);
    QFile f(dict);
    if (f.open(QIODevice::ReadOnly)) {
        while (!f.atEnd()) {
            auto line = f.readLine().trimmed();
            auto i = line.indexOf(':');
            bool ok = false;
            uint c = line.left(i).toUInt(&ok, 16);
            if (ok && i > 0)
                pinyin_.insert(c, QString::fromUtf8(line.mid(i + 1)));
        }
    }
    sqlite3_create_collation(db_, "pinyin_asc", SQLITE_UTF8, this, ascending);
    sqlite3_create_collation(db_, "pinyin_desc", SQLITE_UTF8, this, descending);
}

void SqlDatabase::interrupt() noexcept
{
    if (db_)
        sqlite3_interrupt(db_);
}

SqlDatabase::~SqlDatabase()
{
    if (db_)
        sqlite3_close_v2(db_);
}

QString SqlDatabase::sortKey(const QString &s) const
{
    QString k;
    for (char32_t c : s.toCaseFolded().toUcs4()) {
        auto it = pinyin_.constFind(c);
        k += it == pinyin_.cend() ? QString::fromUcs4(&c, 1) : *it;
    }
    return k;
}

int SqlDatabase::ascending(void *self, int al, const void *a, int bl, const void *b)
{
    auto d = static_cast<SqlDatabase *>(self);
    auto x = QString::fromUtf8(static_cast<const char *>(a), al),
         y = QString::fromUtf8(static_cast<const char *>(b), bl);
    int n = QString::compare(d->sortKey(x), d->sortKey(y), Qt::CaseInsensitive);
    return n ? n : QString::compare(x, y);
}

int SqlDatabase::descending(void *s, int al, const void *a, int bl, const void *b)
{
    return -ascending(s, al, a, bl, b);
}

QString SqlDatabase::quote(const QString &s)
{
    auto t = s;
    t.replace("'", "''");
    return "'" + t + "'";
}

SqlResult SqlDatabase::execute(const QString &sql, bool atomic)
{
    return execute(QStringList{sql}, atomic);
}

SqlResult SqlDatabase::execute(const QStringList &sql, bool atomic)
{
    if (QThread::currentThread() != owner_)
        return {SQLITE_MISUSE, "Database accessed outside owner thread"};
    if (!db_)
        return {SQLITE_CANTOPEN, openError_};
    if (!atomic)
        return exec(sql);

    if (auto begin = exec({"SAVEPOINT ncm_atomic"}); !begin)
        return begin;
    auto rollback = qScopeGuard([this] { exec({"ROLLBACK TO ncm_atomic; RELEASE ncm_atomic"}); });
    const int authorization = sqlite3_set_authorizer(
        db_,
        [](void *, int action, const char *, const char *, const char *, const char *) {
            return action == SQLITE_TRANSACTION || action == SQLITE_SAVEPOINT ? SQLITE_DENY
                                                                              : SQLITE_OK;
        },
        nullptr);
    if (authorization != SQLITE_OK)
        return {authorization, QString::fromUtf8(sqlite3_errstr(authorization))};

    auto result = exec(sql);
    sqlite3_set_authorizer(db_, nullptr, nullptr);
    if (!result) {
        result.rows = {};
        result.positionalRows = {};
        return result;
    }
    if (auto release = exec({"RELEASE ncm_atomic"}); !release)
        return release;
    rollback.dismiss();
    return result;
}

SqlResult SqlDatabase::exec(const QStringList &sql)
{
    SqlResult result;
    auto fail = [&](int code, QString message) {
        result.error = code;
        result.message = std::move(message);
        return std::move(result);
    };
    for (const auto &chunk : sql) {
        auto bytes = chunk.toUtf8();
        const char *tail = bytes.constData();
        const char *end = tail + bytes.size();
        while (tail < end) {
            sqlite3_stmt *stmt = nullptr;
            const char *next = nullptr;
            int code = sqlite3_prepare_v2(db_, tail, int(end - tail), &stmt, &next);
            auto finalize = qScopeGuard([stmt] { sqlite3_finalize(stmt); });
            if (code != SQLITE_OK)
                return fail(code, QString::fromUtf8(sqlite3_errmsg(db_)));
            if (!next || next <= tail)
                return fail(SQLITE_ERROR, QStringLiteral("SQL parser made no progress"));
            tail = next;
            if (!stmt)
                continue;
            result.modified = result.modified || !sqlite3_stmt_readonly(stmt);
            while ((code = sqlite3_step(stmt)) == SQLITE_ROW) {
                QJsonObject row;
                QJsonArray positional;
                for (int i = 0; i < sqlite3_column_count(stmt); ++i) {
                    QJsonValue v;
                    switch (sqlite3_column_type(stmt, i)) {
                    case SQLITE_INTEGER:
                        v = QJsonValue(qint64(sqlite3_column_int64(stmt, i)));
                        break;
                    case SQLITE_FLOAT:
                        v = sqlite3_column_double(stmt, i);
                        break;
                    case SQLITE_TEXT:
                        v = QString::fromUtf8(
                            reinterpret_cast<const char *>(sqlite3_column_text(stmt, i)),
                            sqlite3_column_bytes(stmt, i));
                        break;
                    case SQLITE_BLOB:
                        v = QString::fromLatin1(
                            QByteArray(
                                static_cast<const char *>(sqlite3_column_blob(stmt, i)),
                                sqlite3_column_bytes(stmt, i))
                                .toBase64());
                        break;
                    default:
                        v = QJsonValue::Null;
                    }
                    positional.append(v);
                    row.insert(QString::fromUtf8(sqlite3_column_name(stmt, i)), v);
                }
                result.rows.append(row);
                result.positionalRows.append(positional);
            }
            if (code != SQLITE_DONE)
                return fail(code, QString::fromUtf8(sqlite3_errmsg(db_)));
            finalize.dismiss();
            if (const int status = sqlite3_finalize(stmt); status != SQLITE_OK)
                return fail(status, QString::fromUtf8(sqlite3_errmsg(db_)));
        }
    }
    return result;
}

SqlResult SqlDatabase::initialize(bool offline)
{
    QFile f(
        offline ? ":/ncm/persistence/offline-schema.sql" : ":/ncm/persistence/online-schema.sql");
    if (!f.open(QIODevice::ReadOnly))
        return {SQLITE_CANTOPEN, "Missing bundled schema", {}};
    return execute(QString::fromUtf8(f.readAll()), true);
}

}
