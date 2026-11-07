#pragma once

#include <algorithm>

#include <QElapsedTimer>

namespace ncm {

class PlaybackClock
{
public:
    void report(qint64 position)
    {
        const auto current = now();
        if (playing_ && position < current && current - position < wobble)
            return;
        jump(position);
    }

    void jump(qint64 position)
    {
        position_ = position;
        since_.start();
    }

    void setPlaying(bool playing)
    {
        if (playing == playing_)
            return;
        jump(now());
        playing_ = playing;
    }

    bool playing() const { return playing_; }

    qint64 now() const
    {
        if (!playing_ || !since_.isValid())
            return position_;
        return position_ + std::min(since_.elapsed(), maxExtrapolation);
    }

private:
    static constexpr qint64 maxExtrapolation = 500;
    static constexpr qint64 wobble = 1000;

    qint64 position_ = 0;
    QElapsedTimer since_;
    bool playing_ = false;
};

}
