#pragma once

#include "automation.h"
#include "plugin_state.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace omawave::session {

// One plugin in a track's chain, as the model sees it.
//
// Deliberately free of lilv: `session/` is plain C++20 and unit-testable headless, and
// a project that has been loaded on a machine where the plugin is not installed still
// has to carry this. The uri identifies the plugin type; runtime details -- the running
// instance, the port descriptors, the latency -- lives in `lv2/` and is looked up from
// it.
struct InstanceAutomation {
    double base = 0;
    AutomationCurve curve{};
    bool operator==(const InstanceAutomation&) const = default;
};

struct InsertControl {
    std::uint32_t port = 0;         // port index, as the plugin numbers it
    double value = 0.0;
    AutomationCurve automation{};
    // The opaque snapshot already contains this parameter's current value,
    // independently for each actual instance. Keep value as the UI/automation
    // base, but do not overwrite restored state with it during construction.
    // A subsequent explicit host edit clears this flag, even for the same value.
    bool fromState = false;

    // Transient host edit provenance. It survives history copies but is not
    // serialized or part of persistent value equality.
    std::uint64_t revision = 0;
    // Optional overrides in actual processor order (left/right for dual mono).
    // Absence inherits the legacy shared curve. An explicit empty or disabled
    // curve instead uses its own base, so clearing one side cannot re-enable the
    // shared curve or replace the other side's independently captured value.
    std::array<std::optional<InstanceAutomation>, 2> instanceAutomation{};
    [[nodiscard]] bool hasInstanceAutomation() const noexcept {
        return instanceAutomation[0].has_value() || instanceAutomation[1].has_value();
    }
    [[nodiscard]] bool hasAutomationData() const noexcept {
        return automation.hasData() || hasInstanceAutomation();
    }
    [[nodiscard]] std::size_t automationPointCount() const noexcept {
        auto count = automation.points.size();
        for (const auto& instance : instanceAutomation) if (instance) count += instance->curve.points.size();
        return count;
    }
    [[nodiscard]] const AutomationCurve& automationAt(std::size_t actual) const noexcept {
        return actual < instanceAutomation.size() && instanceAutomation[actual]
            ? instanceAutomation[actual]->curve : automation;
    }
    [[nodiscard]] double automationBase(std::size_t actual) const noexcept {
        return actual < instanceAutomation.size() && instanceAutomation[actual]
            ? instanceAutomation[actual]->base : value;
    }
    [[nodiscard]] bool automationActive() const noexcept {
        return automationAt(0).active() || automationAt(1).active();
    }
    [[nodiscard]] bool sameAutomation(const InsertControl& other) const noexcept {
        return automation == other.automation && instanceAutomation == other.instanceAutomation;
    }
    [[nodiscard]] bool operator==(const InsertControl& other) const {
        return port == other.port && value == other.value && sameAutomation(other) && fromState == other.fromState;
    }
};

// Session-local identity of a logical insert, not a processor/module pointer.
// Copies share it so snapshots, retired chains and Undo refer to the same insert.
// Real duplication must fork it. The token cannot be recycled while a snapshot
// or pending observer still owns it; it is never written to project files.
class InsertIdentity {
    struct Token {};
    std::shared_ptr<const Token> token_ = std::make_shared<const Token>();
public:
    [[nodiscard]] bool operator==(const InsertIdentity&) const noexcept = default;
};

struct Insert {
    std::string uri;
    bool bypass = false;

    // Only the controls that have been moved away from their default. A plugin with a
    // thousand ports -- and one installed here has -- should not put a thousand lines
    // in a project file to say nothing.
    std::vector<InsertControl> controls;
    // Pre-format-13 CLAP projects stored visible parameter positions. Preserve
    // that addressing until the installed plugin can migrate them to param IDs.
    bool legacyClapControls = false;
    // Immutable snapshots in actual instance order (one, or left/right dual mono).
    // A missing plugin still carries its complete state through saves and Undo.
    std::vector<PluginState> instanceState{};
    InsertIdentity identity{};
    std::uint64_t settingsRevision = 0; // transient; distinguishes newer controls from older Redo state

    // Persistent value equality deliberately excludes session-local identity.
    // Undo additionally compares membership identities (see SessionState).
    [[nodiscard]] bool operator==(const Insert& other) const {
        return uri == other.uri && bypass == other.bypass && controls == other.controls
            && legacyClapControls == other.legacyClapControls && instanceState == other.instanceState;
    }
    void forkIdentity() { identity = InsertIdentity{}; }

    [[nodiscard]] const InsertControl* control(std::uint32_t port) const noexcept
    {
        for (const InsertControl& c : controls) {
            if (c.port == port)
                return &c;
        }
        return nullptr;
    }

    void setControl(std::uint32_t port, double value, std::uint64_t revision = 0)
    {
        if (revision > settingsRevision) settingsRevision = revision;
        for (InsertControl& c : controls) {
            if (c.port == port) {
                c.value = value;
                for (auto& instance : c.instanceAutomation) if (instance) instance->base = value;
                c.fromState = false;
                if (revision) c.revision = revision;
                return;
            }
        }
        controls.push_back(InsertControl{.port = port, .value = value, .revision = revision});
    }
};

} // namespace omawave::session
