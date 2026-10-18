# OmaAudio — the application.
#
# Two build systems on purpose, and bin/build hides both. meson keeps building
# engine/ as a static library and keeps running its seventeen suites; qmake6 builds
# the app and links that library. Porting two thousand checks to QtTest to satisfy a
# convention would be the most expensive way imaginable to gain nothing.
#
# See APP-PORT-PLAN.md, amendment A12.

QT += core gui qml quick quickcontrols2 dbus

# CPU views retain the preceding image while a replacement is prepared (Qt 6.8).
lessThan(QT_MAJOR_VERSION, 6): error("OmaAudio requires Qt 6.8 or later")
equals(QT_MAJOR_VERSION, 6):lessThan(QT_MINOR_VERSION, 8): error("OmaAudio requires Qt 6.8 or later")

# C++20 rather than the house 17: the engine library it links is C++20, and the
# headers it exposes use designated initialisers and concepts.
CONFIG += c++20 release
TARGET = omaaudio
TEMPLATE = app

INCLUDEPATH += src engine/src

HEADERS += \
    src/commandline.h src/agentcli.h src/agentskill.h \
    src/storagepaths.h \
    src/resourcebudget.h \
    src/spectralstore.h \
    src/timeformat.h \
    src/manual.h \
    src/audiosetup.h \
    src/pluginlibrary.h src/pluginmetadata.h src/plugincategory.h src/shortname.h src/effectmenu.h src/pluginpresets.h src/builtinpresets.h src/recentdocuments.h \
    src/cpuaudioview.h \
    src/backend.h \
    src/trackidmodel.h src/audiocommand.h \
    src/plugineditor.h src/lv2editor.h \
    src/videoframeitem.h \
    src/keywatcher.h \
    src/filepicker.h \
    src/theme.h \
    src/uitheme.h

SOURCES += \
    src/commandline.cpp src/agentcli.cpp src/agentskill.cpp \
    src/storagepaths.cpp \
    src/manual.cpp \
    src/audiosetup.cpp \
    src/pluginlibrary.cpp src/pluginmetadata.cpp src/plugincategory.cpp src/effectmenu.cpp src/pluginpresets.cpp src/builtinpresets.cpp src/recentdocuments.cpp \
    src/main.cpp \
    src/cpuaudioview.cpp \
    src/backend.cpp \
    src/plugineditor.cpp \
    src/lv2editor.cpp \
    src/backend_clip_properties.cpp \
    src/backend_clip_envelope.cpp \
    src/backend_clip_effects.cpp \
    src/backend_tools.cpp \
    src/backend_speech.cpp \
    src/backend_performance.cpp \
    src/trackidmodel.cpp \
    src/backend_plugin_editors.cpp \
    src/backend_track_loudness.cpp \
    src/backend_ducking.cpp \
    src/backend_essential.cpp src/backend_sound.cpp src/backend_recipes.cpp src/backend_pauses.cpp src/backend_eq_curve.cpp src/backend_effect_apply.cpp \
    src/backend_plugin_presets.cpp \
    src/backend_recent_documents.cpp \
    src/backend_recording.cpp \
    src/backend_routing.cpp \
    src/backend_channel_edit.cpp \
    src/backend_multitrack.cpp \
    src/backend_file_batch.cpp \
    src/backend_collect.cpp \
    src/backend_delivery.cpp \
    src/backend_plugin_history.cpp \
    src/filebatch.cpp \
    src/filebatch_store.cpp \
    src/backend_storage.cpp \
    src/backend_resources.cpp \
    src/spectralstore.cpp \
    src/timeformat.cpp \
    src/backend_automation.cpp \
    src/backend_video.cpp \
    src/videoframeitem.cpp \
    src/filepicker.cpp \
    src/theme.cpp \
    src/uitheme.cpp

RESOURCES += src/resources.qrc

# The engine, built by meson into engine/build-release. A release build because
# imports are roughly ten times slower at -O0 and it is easy to forget which one is
# there. bin/build makes it before qmake6 runs.
isEmpty(ENGINE_LIB): ENGINE_LIB = $$PWD/engine/build-release/libomawave-session.a
LIBS += $$ENGINE_LIB
PRE_TARGETDEPS += $$ENGINE_LIB

# What the engine itself links. Mirrored in the PKGBUILD's depends().
CONFIG += link_pkgconfig
PKGCONFIG += libpipewire-0.3 libspa-0.2 sndfile lilv-0 lv2 tomlplusplus rnnoise xcb xcb-icccm
LIBS += -lpthread

HEADERS += src/displaycache.h
SOURCES += src/displaycache.cpp

HEADERS += src/graphicsstartup.h
SOURCES += src/graphicsstartup.cpp

HEADERS += src/aafimport.h
SOURCES += src/aafimport.cpp

HEADERS += src/filebatch.h src/filebatch_store.h
