#pragma once

#include <QString>
#include <QVector>

// The user manual, installed beside the application and read from inside it.
//
// docs/user-manual.md is the user-facing guide. The package installs that same
// document as share/doc/omaaudio/manual.md. Build-tree runs read the source guide;
// README.md remains the installation and technical reference.
namespace omaaudio::manual {

// One heading and the text under it, down to the next heading of the same or
// higher level. Level 1 is the document's own title, 2 a part, 3 a section.
struct Section {
    QString title;
    QString body;       // markdown, without the heading line
    int level = 2;
};

// Where the manual is, first hit wins:
//   $OMAAUDIO_MANUAL                                 an explicit file
//   <binary>/../share/doc/omaaudio/manual.md         an installed prefix
//   <binary>/../docs/user-manual.md                  the app build tree
//   <binary>/../../docs/user-manual.md               a test build tree
//   /usr/share/doc/omaaudio/manual.md                the package
//   /usr/local/share/doc/omaaudio/manual.md          a local install
// Empty when there is none.
[[nodiscard]] QString findManual();

// The file's text, or an empty string with `error` set to something a person
// can act on.
[[nodiscard]] QString readManual(const QString &path, QString *error = nullptr);

// The document split at its headings, in order. A document with no headings
// comes back as one untitled section.
[[nodiscard]] QVector<Section> sections(const QString &markdown);

} // namespace omaaudio::manual
