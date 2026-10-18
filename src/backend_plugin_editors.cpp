#include "backend.h"
#include "lv2editor.h"
#include "session/effect_racks.h"

#include <QTimer>

// Effects' own windows. A window embeds the live processor of one rack slot; it
// closes whenever that processor is replaced (a rebuilt chain, a removed or
// moved effect, a new session) and always before the processor is destroyed.

bool Backend::pluginHasEditor(int slot, int track)
{
    const ChainTarget target = chainTarget(track);
    if (!target.inserts || slot < 0 || std::size_t(slot) >= target.inserts->size()) return false;
    const auto live = m_chains.find(target.key);
    if (live == m_chains.end() || !live->second) return false;
    const auto* identity = live->second->insertIdentity(std::size_t(slot));
    if (!identity || *identity != (*target.inserts)[std::size_t(slot)].identity) return false;
    auto* processor = live->second->editorProcessor(std::size_t(slot));
    if (!processor) return false;
    if (processor->hasEditor()) return true;
    const auto& uri = (*target.inserts)[std::size_t(slot)].uri;
    const auto* info = m_scanning ? nullptr : m_lv2->find(uri);
    return info && info->format == host::Format::Lv2 && Lv2UiEditor::available(*m_lv2, uri);
}

bool Backend::pluginEditorOpen(int slot, int track) const
{
    const auto key = track == kMasterTarget ? kMasterChainKey
        : session::EffectOwner::track(track < 0 ? m_focused : session::TrackId(track));
    return std::any_of(m_editors.begin(), m_editors.end(), [&](const OpenEditor& editor) {
        return editor.key == key && editor.slot == slot && editor.window;
    });
}

bool Backend::openPluginEditor(int slot, int track)
{
    const auto reject = [this](const QString& message) { setError(message); return false; };
    pruneEditors();
    const ChainTarget target = chainTarget(track);
    if (!target.inserts || slot < 0 || std::size_t(slot) >= target.inserts->size())
        return reject(tr("Choose an effect in the rack first."));
    const auto& insert = (*target.inserts)[std::size_t(slot)];
    for (auto& editor : m_editors)
        if (editor.key == target.key && editor.slot == slot && editor.identity == insert.identity && editor.window) {
            editor.window->raise(); setError({}); return true;
        }
    if (insert.bypass) return reject(tr("Turn the effect on to open its window."));
    const auto live = m_chains.find(target.key);
    if (live == m_chains.end() || !live->second) return reject(tr("The effect is not running yet. Try again in a moment."));
    const auto* identity = live->second->insertIdentity(std::size_t(slot));
    if (!identity || *identity != insert.identity) return reject(tr("The effects changed. Try again in a moment."));
    auto* processor = live->second->editorProcessor(std::size_t(slot));
    if (!processor) return reject(tr("This effect is not installed, so it has no window."));
    std::unique_ptr<host::EmbeddedEditor> adapter;
    host::EmbeddedEditor* editor = processor;
    if (!processor->hasEditor()) {
        // An LV2 effect's own interface is a separate library; its controls go
        // through the same path as the rack's, so the project records them.
        const auto* plugin = m_scanning ? nullptr : m_lv2->find(insert.uri);
        if (!plugin || plugin->format != host::Format::Lv2 || !Lv2UiEditor::available(*m_lv2, insert.uri))
            return reject(tr("This effect has no window of its own. Use its controls in the rack."));
        const auto key = target.key;
        const auto identity = insert.identity;
        const auto slotOf = [this, key, identity]() -> int {
            const auto* inserts = session::findEffectRack(m_project, key);
            if (!inserts) return -1;
            for (std::size_t i = 0; i < inserts->size(); ++i) if ((*inserts)[i].identity == identity) return int(i);
            return -1;
        };
        const int owner = target.target;
        std::string reason;
        adapter = Lv2UiEditor::create(*m_lv2, m_urids, insert.uri,
            [this, slotOf, owner](std::uint32_t port, float value) {
                if (const int at = slotOf(); at >= 0) setPluginControl(at, qint64(port), double(value), owner);
            },
            [this, slotOf, key](std::uint32_t port) -> std::optional<float> {
                const int at = slotOf();
                const auto live = m_chains.find(key);
                if (at < 0 || live == m_chains.end() || !live->second) return std::nullopt;
                return float(live->second->controlValue(std::size_t(at), port));
            }, reason);
        if (!adapter) return reject(QString::fromStdString(reason));
        editor = adapter.get();
    }
    const auto* info = m_lv2->find(insert.uri);
    const QString effect = info ? QString::fromStdString(info->name) : QString::fromStdString(insert.uri);
    const QString owner = target.key.isMaster() ? tr("Master")
        : m_project.track(target.track) ? QString::fromStdString(m_project.track(target.track)->name) : QString();
    QString error;
    auto window = PluginEditorWindow::open(*editor, owner.isEmpty() ? effect : tr("%1 — %2").arg(effect, owner), error);
    if (!window) return reject(error);
    const auto key = target.key;
    connect(window.get(), &PluginEditorWindow::closed, this, [this, key, slot] {
        for (auto it = m_editors.begin(); it != m_editors.end(); ++it) {
            if (it->key != key || it->slot != slot) continue;
            // Emitted from the window's own event: let the effect go now and
            // delete the window once that event has returned.
            it->window->releaseProcessor();
            it->window.release()->deleteLater();
            // An LV2 interface library outlives the deferred window by one turn.
            if (auto* adapter = it->adapter.release()) QTimer::singleShot(0, this, [adapter] { delete adapter; });
            m_editors.erase(it);
            emit pluginEditorsChanged();
            return;
        }
    });
    m_editors.push_back({target.key, slot, insert.identity, live->second, processor, std::move(adapter), std::move(window)});
    setError({});
    emit pluginEditorsChanged();
    return true;
}

void Backend::closePluginEditor(int slot, int track)
{
    const auto key = track == kMasterTarget ? kMasterChainKey
        : session::EffectOwner::track(track < 0 ? m_focused : session::TrackId(track));
    const auto before = m_editors.size();
    std::erase_if(m_editors, [&](OpenEditor& editor) {
        if (editor.key != key || editor.slot != slot) return false;
        if (editor.window) editor.window->releaseProcessor();
        return true;
    });
    if (m_editors.size() != before) emit pluginEditorsChanged();
}

void Backend::pruneEditors(bool all)
{
    const auto before = m_editors.size();
    std::erase_if(m_editors, [&](OpenEditor& editor) {
        bool live = false;
        if (!all && editor.window) {
            const auto chain = editor.chain.lock();
            const auto current = m_chains.find(editor.key);
            if (chain && current != m_chains.end() && current->second == chain) {
                const auto* identity = chain->insertIdentity(std::size_t(editor.slot));
                live = identity && *identity == editor.identity
                    && chain->editorProcessor(std::size_t(editor.slot)) == editor.processor;
            }
        }
        if (!live && editor.window) editor.window->releaseProcessor();
        return !live;
    });
    if (m_editors.size() != before) emit pluginEditorsChanged();
}
