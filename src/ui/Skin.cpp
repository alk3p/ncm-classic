#include "Skin.h"

#include <utility>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QXmlStreamReader>

namespace ncm {

namespace {

const QColor officialRed = QColor::fromHslF(0, .62f, .48f);

constexpr std::pair<const char *, Skin::Color> placeholders[] = {
    {"MENU_ITEM_TEXT_COLOR", Skin::Color::MenuItemText},
    {"NO_LRC_TITLE_COLOR", Skin::Color::NoLyricsTitle},
    {"NO_LRC_ARTIST_COLOR", Skin::Color::NoLyricsArtist},
    {"INFO_PANEL_TITLE_COLOR", Skin::Color::InfoPanelTitle},
    {"INFO_PANEL_ARTIST_COLOR", Skin::Color::InfoPanelArtist},
    {"PLAYLIST_ITEM_TEXT_COLOR", Skin::Color::PlaylistItemText},
    {"PLAYLIST_ITEM_SELECTED_COLOR", Skin::Color::PlaylistItemSelected},
};

QString cssColor(const QColor &color)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

template<typename Visitor>
void forEachElement(const QString &path, Visitor visit)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    QXmlStreamReader xml(&file);
    while (!xml.atEnd())
        if (xml.readNext() == QXmlStreamReader::StartElement)
            visit(xml.name(), xml.attributes());
}

}

Skin::Skin(QObject *parent)
    : QObject(parent)
{
    readColors();
}

bool Skin::load(const QString &name, const QColor &buttonColor)
{
    if (!QDir(":/ncm/assets/webapp/style/skin/" + name).exists())
        return false;
    name_ = name;
    const bool coloured = QFileInfo::exists(file("btn/color_btns.png"));
    buttonColor_ = !coloured ? QColor() : buttonColor.isValid() ? buttonColor : officialRed;
    readColors();
    emit changed();
    return true;
}

QString Skin::file(const QString &relative) const
{
    return ":/ncm/assets/webapp/style/skin/" + name_ + '/' + relative;
}

QPixmap Skin::pixmap(const QString &relative) const
{
    return QPixmap(file(relative));
}

QColor Skin::color(Color which) const
{
    return colors_[std::to_underlying(which)];
}

QString Skin::styleSheet(const QString &name) const
{
    QFile source(":/ncm/assets/styles/" + name);
    if (!source.open(QIODevice::ReadOnly))
        return {};
    auto sheet = QString::fromUtf8(source.readAll());
    sheet.replace(QLatin1StringView("SKIN_ROOT/"), file({}));
    for (const auto &[placeholder, which] : placeholders)
        sheet.replace(QLatin1StringView(placeholder), cssColor(color(which)));
    return sheet;
}

void Skin::readColors()
{
    const auto set = [this](Color which, QStringView argb) {
        if (const auto parsed = QColor::fromString(argb); parsed.isValid())
            colors_[std::to_underlying(which)] = parsed;
    };
    colors_.fill(Qt::white);

    forEachElement(file("mini/skin.xml"), [&](QStringView element, const QXmlStreamAttributes &a) {
        const auto name = a.value("name");
        if (name == u"wordbar_info") {
            set(Color::InfoPanelTitle, a.value("textcolor"));
            set(Color::InfoPanelArtist, a.value("textcolor1"));
        } else if (name == u"no_lrc_title") {
            set(Color::NoLyricsTitle, a.value("textcolor"));
        } else if (name == u"no_lrc_artist") {
            set(Color::NoLyricsArtist, a.value("textcolor"));
        } else if (name == u"new_lyrc") {
            set(Color::MiniLyrics, a.value("textcolor"));
        } else if (element == u"PlayList") {
            set(Color::PlaylistItemText, a.value("itemtextcolor"));
            set(Color::PlaylistItemSelected, a.value("itemselectedtextcolor"));
        }
    });
    forEachElement(file("menu/element.xml"), [&](QStringView element, const QXmlStreamAttributes &a) {
        if (element == u"MenuLabel")
            set(Color::MenuItemText, a.value("textcolor"));
    });
    bool outlined = false;
    forEachElement(file("lrc/skin.xml"), [&](QStringView element, const QXmlStreamAttributes &a) {
        if (element == u"TextOutline" && !std::exchange(outlined, true))
            set(Color::LyricsOutline, a.value("shadowColor"));
    });
}

}
