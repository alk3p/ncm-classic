#pragma once

#include <QWidget>
#include <QWindow>

namespace ncm {

inline void setWindowFlagInPlace(QWidget &widget, Qt::WindowType flag, bool on)
{
    auto flags = widget.windowFlags();
    if (flags.testFlag(flag) == on)
        return;
    flags.setFlag(flag, on);
    widget.overrideWindowFlags(flags);
    if (auto *window = widget.windowHandle())
        window->setFlags(flags);
}

}
