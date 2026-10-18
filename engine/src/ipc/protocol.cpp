#include "protocol.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>

namespace omawave::ipc {
namespace {

std::string_view trim(std::string_view s)
{
    const auto notSpace = [](char c) { return c != ' ' && c != '\t' && c != '\r'; };
    while (!s.empty() && !notSpace(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && !notSpace(s.back()))
        s.remove_suffix(1);
    return s;
}

// Values are quoted so that paths containing spaces, '=' or unicode survive the round
// trip. Newlines are escaped as well as quotes and backslashes, which is what
// guarantees a value can never contain the blank-line message delimiter.
std::string quote(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size() + 2);
    out.push_back('"');
    for (const char c : raw) {
        switch (c) {
        case '"':
        case '\\':
            out.push_back('\\');
            out.push_back(c);
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    out.push_back('"');
    return out;
}

std::optional<std::string> unquote(std::string_view raw)
{
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"')
        return std::nullopt;
    raw.remove_prefix(1);
    raw.remove_suffix(1);

    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            ++i;
            switch (raw[i]) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            default: out.push_back(raw[i]); break;
            }
        } else {
            out.push_back(raw[i]);
        }
    }
    return out;
}

} // namespace

void Message::set(std::string key, std::string value)
{
    fields_.push_back(Field{std::move(key), std::move(value)});
}

void Message::set(std::string key, std::int64_t value)
{
    set(std::move(key), std::to_string(value));
}

void Message::set(std::string key, double value)
{
    // A general double with max_digits10 needs fewer than 32 characters, even at
    // the exponent limits. Avoid both locale dependence and to_string's rounding.
    char buffer[64];
    const auto converted = std::to_chars(buffer, buffer + sizeof buffer, value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    set(std::move(key), std::string(buffer, converted.ptr));
}

void Message::set(std::string key, bool value)
{
    set(std::move(key), std::string(value ? "true" : "false"));
}

const Message::Field* Message::find(std::string_view key) const
{
    const auto it = std::find_if(fields_.begin(), fields_.end(),
                                 [key](const Field& f) { return f.key == key; });
    return it == fields_.end() ? nullptr : &*it;
}

std::optional<std::string> Message::string(std::string_view key) const
{
    const Field* f = find(key);
    if (f == nullptr)
        return std::nullopt;
    return f->value;
}

std::optional<std::int64_t> Message::integer(std::string_view key) const
{
    const Field* f = find(key);
    if (f == nullptr)
        return std::nullopt;

    std::int64_t out = 0;
    const char* begin = f->value.data();
    const char* end = begin + f->value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{} || ptr != end)
        return std::nullopt;
    return out;
}

std::optional<double> Message::number(std::string_view key) const
{
    const Field* f = find(key);
    if (f == nullptr)
        return std::nullopt;

    double out = 0.0;
    const char* begin = f->value.data();
    const char* end = begin + f->value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{} || ptr != end)
        return std::nullopt;
    return out;
}

std::optional<bool> Message::boolean(std::string_view key) const
{
    const Field* f = find(key);
    if (f == nullptr)
        return std::nullopt;
    if (f->value == "true")
        return true;
    if (f->value == "false")
        return false;
    return std::nullopt;
}

std::string Message::encode() const
{
    std::string out = "type = " + quote(type_) + "\n";
    for (const Field& f : fields_)
        out += f.key + " = " + quote(f.value) + "\n";
    return out;
}

std::optional<Message> Message::decode(std::string_view body)
{
    Message msg;
    bool sawType = false;

    std::size_t pos = 0;
    while (pos <= body.size()) {
        const std::size_t nl = body.find('\n', pos);
        const std::string_view raw = trim(
            body.substr(pos, nl == std::string_view::npos ? std::string_view::npos
                                                          : nl - pos));
        if (nl == std::string_view::npos)
            pos = body.size() + 1;
        else
            pos = nl + 1;

        if (raw.empty())
            continue;

        const std::size_t eq = raw.find('=');
        if (eq == std::string_view::npos)
            return std::nullopt;

        const std::string_view key = trim(raw.substr(0, eq));
        const std::optional<std::string> value = unquote(trim(raw.substr(eq + 1)));
        if (key.empty() || !value.has_value())
            return std::nullopt;

        if (key == "type") {
            msg.type_ = *value;
            sawType = true;
        } else {
            msg.fields_.push_back(Field{std::string(key), *value});
        }
    }

    if (!sawType)
        return std::nullopt;
    return msg;
}

std::string Message::encodeForWire() const
{
    return encode() + std::string(kMessageDelimiter.substr(1));
}

void MessageStream::append(const char* data, std::size_t size)
{
    if (failed_)
        return;
    if (buffer_.size() + size > kMaxMessageBytes) {
        failed_ = true;
        error_ = "message exceeded " + std::to_string(kMaxMessageBytes) + " bytes";
        return;
    }
    buffer_.append(data, size);
}

std::optional<Message> MessageStream::next()
{
    if (failed_)
        return std::nullopt;

    std::size_t consumed = 0;
    while (true) {
        const std::size_t end = buffer_.find(kMessageDelimiter, consumed);
        if (end == std::string::npos) {
            buffer_.erase(0, consumed);
            return std::nullopt;           // more bytes still to come
        }

        auto message = Message::decode(std::string_view(buffer_).substr(consumed, end + 1 - consumed));
        consumed = end + kMessageDelimiter.size();
        if (message) {
            buffer_.erase(0, consumed);
            return message;
        }
        // A malformed message is recoverable -- the delimiter told us exactly where it
        // ended. Keep draining: another complete record may already be buffered.
        error_ = "malformed message body";
    }
}

} // namespace omawave::ipc
