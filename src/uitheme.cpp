#include "uitheme.h"

#include <QFontDatabase>
#include <QGuiApplication>
#include <QFont>

Styles::Styles(OmarchyTheme *theme, QObject *parent) : QObject(parent), m_theme(theme)
{
    if (m_theme != nullptr)
        connect(m_theme, &OmarchyTheme::textScaleChanged, this, &Styles::changed);
}

qreal Styles::space(qreal pixels) const
{
    const qreal scale = m_theme != nullptr ? m_theme->textScale() : 1.0;
    return pixels * (scale > 0.0 ? scale : 1.0);
}

int Styles::rPill(qreal) const
{
    return rClip();
}

QVariantMap Styles::font() const
{
    // Inter when installed, Noto Sans otherwise -- the same choice Theme.qml makes,
    // so old and new sites set the same face. Inter's digits are tabular, so the
    // numbers use the same family too.
    const QStringList families = QFontDatabase::families();
    const QString family = families.contains(QStringLiteral("Inter"))
                         ? QStringLiteral("Inter")
                         : families.contains(QStringLiteral("Noto Sans"))
                         ? QStringLiteral("Noto Sans")
                         : QGuiApplication::font().family();
    QVariantMap out;
    out["family"] = family;
    out["mono"] = family;
    out["body"] = space(12);
    out["bodySmall"] = space(12);
    out["caption"] = space(11);
    return out;
}
