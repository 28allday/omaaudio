#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omawave::ipc {

// Wire format: text messages separated by a blank line.
//
//   type = "status"
//   frame = "48000"
//   playing = "true"
//   <blank line>
//
// A NOTE ON TOML: BRIEF.md section 4 chooses TOML for project files. The licence
// table in section 2 contains no TOML parser and adding one needs an explicit
// decision, so this is a deliberately strict subset -- `key = "value"`, one per line,
// every value quoted, no nesting, no arrays, no comments. It is valid TOML but needs
// about thirty lines to parse rather than a dependency. Phase 3 will need real TOML
// for .omaaudio project files, and that is where the decision has to be made.
//
// A NOTE ON FRAMING: this started as length-prefixed binary frames so that bulk peak
// data could bypass the text parser. Peaks ended up travelling as a PNG file instead
// (see io/peak_texture.h -- QML cannot build a texture from bytes without a C++ image
// provider, which amendment A3 rules out). With only control text left on the wire, a
// length prefix bought nothing and cost the QML side a hand-written binary reader, so
// the delimiter form won. Quickshell's SplitParser handles it directly.
//
// Values are escaped on the way out, so a value can never contain the delimiter.

inline constexpr std::string_view kMessageDelimiter = "\n\n";

// Largest buffered message. An unbounded stream is how a stray or hostile writer
// turns into an allocation failure.
inline constexpr std::size_t kMaxMessageBytes = 1024u * 1024u;

class Message {
public:
    Message() = default;
    explicit Message(std::string type) : type_(std::move(type)) {}

    [[nodiscard]] const std::string& type() const noexcept { return type_; }
    void setType(std::string t) { type_ = std::move(t); }

    void set(std::string key, std::string value);
    void set(std::string key, std::int64_t value);
    void set(std::string key, double value);
    void set(std::string key, bool value);

    [[nodiscard]] std::optional<std::string> string(std::string_view key) const;
    [[nodiscard]] std::optional<std::int64_t> integer(std::string_view key) const;
    [[nodiscard]] std::optional<double> number(std::string_view key) const;
    [[nodiscard]] std::optional<bool> boolean(std::string_view key) const;

    // Body only, without the trailing delimiter.
    [[nodiscard]] std::string encode() const;

    // Body plus the delimiter, ready to write to the socket.
    [[nodiscard]] std::string encodeForWire() const;

    // Returns nullopt when the body is malformed rather than throwing, so a bad
    // message from the UI cannot take the engine down.
    [[nodiscard]] static std::optional<Message> decode(std::string_view body);

private:
    struct Field {
        std::string key;
        std::string value;
    };

    [[nodiscard]] const Field* find(std::string_view key) const;

    std::string type_;
    std::vector<Field> fields_;
};

// Incremental message extraction from a byte stream.
//
// Stream sockets split and coalesce writes freely, so a reader that assumes one read
// equals one message works right up until it does not.
class MessageStream {
public:
    void append(const char* data, std::size_t size);

    // Skips malformed records and returns the next complete valid message.
    // nullopt means more bytes are needed or the stream has failed; callers can
    // drain a coalesced read with a simple while loop.
    [[nodiscard]] std::optional<Message> next();

    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size(); }

private:
    std::string buffer_;
    bool failed_ = false;
    std::string error_;
};

} // namespace omawave::ipc
