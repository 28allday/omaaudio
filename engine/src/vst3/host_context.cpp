#include "host_context.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <new>
#include <variant>
#include <vector>

namespace omawave::vst3 {
using namespace Steinberg;
using namespace Steinberg::Vst;
namespace {
bool same(const char* raw, const FUID& id) { return raw && std::memcmp(raw, id.toTUID(), 16) == 0; }
template<class Interface> class Object : public Interface {
public:
    tresult PLUGIN_API queryInterface(const TUID id, void** output) override {
        if (!output) return kInvalidArgument;
        *output = nullptr;
        if (!same(id, Interface::iid) && !same(id, FUnknown::iid)) return kNoInterface;
        *output = static_cast<Interface*>(this); addRef(); return kResultOk;
    }
    uint32 PLUGIN_API addRef() override { return ++references_; }
    uint32 PLUGIN_API release() override { const auto count = --references_; if (!count) delete this; return count; }
protected:
    virtual ~Object() = default;
private:
    std::atomic<uint32> references_{1};
};
constexpr std::size_t kKeyLimit = 1024, kValueLimit = 1024 * 1024, kTotalLimit = 4 * kValueLimit, kCountLimit = 1024;
using Binary = std::vector<std::uint8_t>;
using Value = std::variant<int64, double, std::u16string, Binary>;
std::size_t bytes(const Value& value) {
    return std::visit([](const auto& v) -> std::size_t {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::u16string>) return (v.size() + 1) * sizeof(char16);
        else if constexpr (std::is_same_v<T, Binary>) return v.size();
        else return sizeof(v);
    }, value);
}
bool validKey(const char* key) { return key && ::strnlen(key, kKeyLimit + 1) <= kKeyLimit; }

class Attributes final : public Object<IAttributeList> {
public:
    tresult PLUGIN_API setInt(AttrID key, int64 value) override { return put(key, value); }
    tresult PLUGIN_API getInt(AttrID key, int64& value) override { return get(key, value); }
    tresult PLUGIN_API setFloat(AttrID key, double value) override { return put(key, value); }
    tresult PLUGIN_API getFloat(AttrID key, double& value) override { return get(key, value); }
    tresult PLUGIN_API setString(AttrID key, const TChar* value) override {
        if (!value || !validKey(key)) return kInvalidArgument;
        std::size_t size = 0;
        while (size < kValueLimit / sizeof(TChar) && value[size]) ++size;
        if (size == kValueLimit / sizeof(TChar)) return kOutOfMemory;
        try { return put(key, std::u16string(value, size)); } catch (const std::bad_alloc&) { return kOutOfMemory; }
    }
    tresult PLUGIN_API getString(AttrID key, TChar* value, uint32 size) override {
        if (!value || size < sizeof(TChar) || size % sizeof(TChar)) return kInvalidArgument;
        const auto* text = find<std::u16string>(key);
        if (!text) return validKey(key) ? kResultFalse : kInvalidArgument;
        const auto count = std::min(text->size(), size / sizeof(TChar) - 1);
        std::memcpy(value, text->data(), count * sizeof(TChar)); value[count] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API setBinary(AttrID key, const void* data, uint32 size) override {
        if (!validKey(key) || (!data && size)) return kInvalidArgument;
        if (size > kValueLimit) return kOutOfMemory;
        try {
            Binary value(size);
            if (size) std::memcpy(value.data(), data, size);
            return put(key, std::move(value));
        } catch (const std::bad_alloc&) { return kOutOfMemory; }
    }
    tresult PLUGIN_API getBinary(AttrID key, const void*& data, uint32& size) override {
        data = nullptr; size = 0;
        const auto* value = find<Binary>(key);
        if (!value) return validKey(key) ? kResultFalse : kInvalidArgument;
        data = value->data(); size = static_cast<uint32>(value->size()); return kResultOk;
    }
private:
    template<class T> const T* find(AttrID key) const {
        if (!validKey(key)) return nullptr;
        const auto found = values_.find(key);
        return found == values_.end() ? nullptr : std::get_if<T>(&found->second);
    }
    template<class T> tresult get(AttrID key, T& value) const {
        const auto* result = find<T>(key);
        if (!result) return validKey(key) ? kResultFalse : kInvalidArgument;
        value = *result; return kResultOk;
    }
    tresult put(AttrID key, Value value) {
        if (!validKey(key)) return kInvalidArgument;
        const auto found = values_.find(key);
        const auto oldSize = found == values_.end() ? 0 : bytes(found->second) + found->first.size();
        const auto size = bytes(value) + std::strlen(key);
        if (size > kTotalLimit - (total_ - oldSize)
            || (found == values_.end() && values_.size() == kCountLimit)) return kOutOfMemory;
        try {
            values_.insert_or_assign(key, std::move(value)); total_ = total_ - oldSize + size;
            return kResultOk;
        } catch (const std::bad_alloc&) { return kOutOfMemory; }
    }
    std::map<std::string, Value, std::less<>> values_;
    std::size_t total_ = 0;
};

class Message final : public Object<IMessage> {
public:
    FIDString PLUGIN_API getMessageID() override { return id_.empty() ? nullptr : id_.c_str(); }
    void PLUGIN_API setMessageID(FIDString id) override {
        if (!id) { id_.clear(); return; }
        if (::strnlen(id, kKeyLimit + 1) > kKeyLimit) return;
        try { id_ = id; } catch (const std::bad_alloc&) {}
    }
    IAttributeList* PLUGIN_API getAttributes() override { return attributes_; }
private:
    std::string id_;
    IPtr<IAttributeList> attributes_ = owned(new Attributes);
};

class Context final : public Object<IHostApplication> {
public:
    tresult PLUGIN_API getName(String128 name) override {
        if (!name) return kInvalidArgument;
        std::fill(name, name + 128, TChar{});
        constexpr char16_t text[] = u"OmaAudio"; std::copy(std::begin(text), std::end(text), name);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID cid, TUID iid, void** output) override {
        if (!output) return kInvalidArgument;
        *output = nullptr;
        try {
            IPtr<FUnknown> object;
            if (same(cid, IMessage::iid)) object = owned(new Message);
            else if (same(cid, IAttributeList::iid)) object = owned(new Attributes);
            else return kNoInterface;
            return object->queryInterface(iid, output);
        } catch (const std::bad_alloc&) { return kOutOfMemory; }
    }
};
}

IPtr<IHostApplication> makeHostContext() { return owned(new Context); }
std::string utf8(const char16* text, std::size_t units)
{
    std::string result;
    for (std::size_t i = 0; text && i < units && text[i]; ++i) {
        std::uint32_t code = text[i];
        if (code >= 0xD800 && code <= 0xDBFF) {
            if (i + 1 < units && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
                code = 0x10000 + ((code - 0xD800) << 10) + (text[++i] - 0xDC00);
            } else code = 0xFFFD;
        } else if (code >= 0xDC00 && code <= 0xDFFF) code = 0xFFFD;
        if (code < 0x80) result += static_cast<char>(code);
        else if (code < 0x800) {
            result += static_cast<char>(0xC0 | (code >> 6)); result += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            if (code >= 0x10000) { result += static_cast<char>(0xF0 | (code >> 18)); result += static_cast<char>(0x80 | ((code >> 12) & 0x3F)); }
            else result += static_cast<char>(0xE0 | (code >> 12));
            result += static_cast<char>(0x80 | ((code >> 6) & 0x3F)); result += static_cast<char>(0x80 | (code & 0x3F));
        }
    }
    return result;
}
}
