#pragma once

// `Style`, as the ported editor QML sees it.
//
// The editor's older QML came from an omarchy-shell plugin, where this name was a
// singleton the shell provides. The app supplies an object of the same shape under
// the same name so the ported drawing code keeps working. It is a BRIDGE: every
// value here is the same token `src/ui/Theme.qml` defines, and new QML imports
// `"ui"` and says `Theme.*` directly. When the last old site is gone, this file
// goes with it.
//
// ⚠️ `Color` used to live here too, restating the palette as C++ literals. It went
// when the accent started following the Omarchy theme (A93): the last three sites
// were Waveform.qml's wave, selection and background, and a second copy of the
// palette that could not follow would have drawn a cyan selection inside a chrome
// that had gone orange.

#include "theme.h"

#include <QObject>
#include <QVariantMap>

class Styles : public QObject {
    Q_OBJECT

    // Maps rather than nested objects: QML reads `Style.font.body` off a QVariantMap
    // exactly as it would off an object, and this keeps the whole thing to one file.
    Q_PROPERTY(QVariantMap font READ font NOTIFY changed)

    // The brief's corner language: controls 3, menus 4, panels square.
    Q_PROPERTY(int rCard READ rCard CONSTANT)   // menus, dialogs
    Q_PROPERTY(int rClip READ rClip CONSTANT)   // controls, lanes

public:
    explicit Styles(OmarchyTheme *theme, QObject *parent = nullptr);

    [[nodiscard]] QVariantMap font() const;
    [[nodiscard]] int rCard() const { return 4; }
    [[nodiscard]] int rClip() const { return 3; }

    // Controls: always the control radius now. Kept as a call so old sites compile.
    Q_INVOKABLE int rPill(qreal height) const;

    // The desktop text scale applies to every size, so `omarchy display text size`
    // moves the editor's type with everything else.
    Q_INVOKABLE qreal space(qreal pixels) const;

signals:
    void changed();

private:
    OmarchyTheme *m_theme;
};
