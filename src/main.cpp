#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/utils/clipboard.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace {

constexpr int kPopupTag = 0x53504331;
constexpr std::size_t kMaxDecodedBytes = 1024 * 1024;
constexpr std::size_t kMaxCodeChars = 1500000;
constexpr std::size_t kMaxFieldBytes = 32768;
constexpr std::uint32_t kMaxObjects = 10000;
constexpr std::uint32_t kMaxColorActions = 4096;
constexpr int kMaxGroupId = 9999;

// The only trigger object IDs accepted from a style code. Other trigger
// objects can affect gameplay and are deliberately not transferred.
bool isStyleTriggerID(int objectID) {
    switch (objectID) {
        case 899:  // color
        case 1007: // alpha
        case 1006: // pulse
        case 1585: // animation
            return true;
        default:
            return false;
    }
}

struct ColorRecord {
    std::int32_t id = 0;
    std::array<std::uint8_t, 3> from{};
    std::array<std::uint8_t, 3> to{};
    std::uint8_t blending = 0;
    std::int32_t playerColor = 0;
    float fromOpacity = 1.f;
    float toOpacity = 1.f;
};

struct ObjectRecord {
    std::uint8_t kind = 0; // 0 = decoration, 1 = allowlisted style trigger
    std::string saveString;
};

struct StyleData {
    std::string name;
    std::string creator;
    std::int32_t background = 0;
    std::int32_t ground = 0;
    std::int32_t middleGround = 0;
    std::vector<ColorRecord> colors;
    std::vector<ObjectRecord> objects;
};

class ByteWriter {
public:
    std::vector<std::uint8_t> bytes;

    void putU8(std::uint8_t value) {
        bytes.push_back(value);
    }

    void putU32(std::uint32_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
    }

    void putI32(std::int32_t value) {
        putU32(static_cast<std::uint32_t>(value));
    }

    void putFloat(float value) {
        std::uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        putU32(bits);
    }

    bool putString(std::string const& value) {
        if (value.size() > kMaxFieldBytes) {
            return false;
        }
        putU32(static_cast<std::uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
        return bytes.size() <= kMaxDecodedBytes;
    }
};

class ByteReader {
public:
    ByteReader(std::uint8_t const* data, std::size_t size)
      : m_data(data), m_size(size) {}

    bool getU8(std::uint8_t& value) {
        if (m_offset >= m_size) {
            return false;
        }
        value = m_data[m_offset++];
        return true;
    }

    bool getU32(std::uint32_t& value) {
        if (m_size - m_offset < 4) {
            return false;
        }
        value =
            static_cast<std::uint32_t>(m_data[m_offset]) |
            (static_cast<std::uint32_t>(m_data[m_offset + 1]) << 8) |
            (static_cast<std::uint32_t>(m_data[m_offset + 2]) << 16) |
            (static_cast<std::uint32_t>(m_data[m_offset + 3]) << 24);
        m_offset += 4;
        return true;
    }

    bool getI32(std::int32_t& value) {
        std::uint32_t raw = 0;
        if (!getU32(raw)) {
            return false;
        }
        value = static_cast<std::int32_t>(raw);
        return true;
    }

    bool getFloat(float& value) {
        std::uint32_t bits = 0;
        if (!getU32(bits)) {
            return false;
        }
        std::memcpy(&value, &bits, sizeof(value));
        return std::isfinite(value);
    }

    bool getString(std::string& value) {
        std::uint32_t length = 0;
        if (!getU32(length) || length > kMaxFieldBytes ||
            length > m_size - m_offset) {
            return false;
        }
        value.assign(
            reinterpret_cast<char const*>(m_data + m_offset),
            static_cast<std::size_t>(length)
        );
        m_offset += length;
        return true;
    }

    bool atEnd() const {
        return m_offset == m_size;
    }

private:
    std::uint8_t const* m_data = nullptr;
    std::size_t m_size = 0;
    std::size_t m_offset = 0;
};

std::uint32_t fnv1a(std::uint8_t const* data, std::size_t size) {
    std::uint32_t hash = 2166136261u;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

std::string base64Encode(std::vector<std::uint8_t> const& input) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);

    for (std::size_t i = 0; i < input.size(); i += 3) {
        std::uint32_t value = static_cast<std::uint32_t>(input[i]) << 16;

        if (i + 1 < input.size()) {
            value |= static_cast<std::uint32_t>(input[i + 1]) << 8;
        }
        if (i + 2 < input.size()) {
            value |= static_cast<std::uint32_t>(input[i + 2]);
        }

        output.push_back(alphabet[(value >> 18) & 0x3f]);
        output.push_back(alphabet[(value >> 12) & 0x3f]);
        output.push_back(i + 1 < input.size() ? alphabet[(value >> 6) & 0x3f] : '=');
        output.push_back(i + 2 < input.size() ? alphabet[value & 0x3f] : '=');
    }

    return output;
}

int base64Value(char value) {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

bool base64Decode(std::string const& input, std::vector<std::uint8_t>& output) {
    if (input.empty() || input.size() > kMaxCodeChars || input.size() % 4 != 0) {
        return false;
    }

    output.clear();
    output.reserve((input.size() / 4) * 3);

    for (std::size_t i = 0; i < input.size(); i += 4) {
        int a = base64Value(input[i]);
        int b = base64Value(input[i + 1]);
        if (a < 0 || b < 0) {
            return false;
        }

        bool pad2 = input[i + 2] == '=';
        bool pad3 = input[i + 3] == '=';
        int c = pad2 ? 0 : base64Value(input[i + 2]);
        int d = pad3 ? 0 : base64Value(input[i + 3]);

        if (c < 0 || d < 0 || (pad2 && !pad3)) {
            return false;
        }
        if ((pad2 || pad3) && i + 4 != input.size()) {
            return false;
        }

        std::uint32_t value =
            (static_cast<std::uint32_t>(a) << 18) |
            (static_cast<std::uint32_t>(b) << 12) |
            (static_cast<std::uint32_t>(c) << 6) |
            static_cast<std::uint32_t>(d);

        output.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
        if (!pad2) {
            output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        }
        if (!pad3) {
            output.push_back(static_cast<std::uint8_t>(value & 0xff));
        }

        if (output.size() > kMaxDecodedBytes) {
            return false;
        }
    }

    return true;
}

bool encodeStyle(StyleData const& style, std::string& code) {
    if (style.colors.size() > kMaxColorActions ||
        style.objects.size() > kMaxObjects) {
        return false;
    }

    ByteWriter writer;
    writer.bytes.insert(writer.bytes.end(), {'S', 'P', 'C', '1'});
    writer.putU8(1);

    if (!writer.putString(style.name) ||
        !writer.putString(style.creator)) {
        return false;
    }

    writer.putI32(style.background);
    writer.putI32(style.ground);
    writer.putI32(style.middleGround);
    writer.putU32(static_cast<std::uint32_t>(style.colors.size()));

    for (auto const& color : style.colors) {
        writer.putI32(color.id);
        for (auto channel : color.from) writer.putU8(channel);
        for (auto channel : color.to) writer.putU8(channel);
        writer.putU8(color.blending);
        writer.putI32(color.playerColor);
        writer.putFloat(color.fromOpacity);
        writer.putFloat(color.toOpacity);
    }

    writer.putU32(static_cast<std::uint32_t>(style.objects.size()));
    for (auto const& object : style.objects) {
        writer.putU8(object.kind);
        if (!writer.putString(object.saveString)) {
            return false;
        }
    }

    if (writer.bytes.size() + 4 > kMaxDecodedBytes) {
        return false;
    }

    writer.putU32(fnv1a(writer.bytes.data(), writer.bytes.size()));
    code = base64Encode(writer.bytes);
    return code.size() <= kMaxCodeChars;
}

bool decodeStyle(std::string const& code, StyleData& style, std::string& error) {
    std::vector<std::uint8_t> bytes;
    if (!base64Decode(code, bytes) || bytes.size() < 9) {
        error = "Invalid or oversized StylePaletteCopier code.";
        return false;
    }

    std::uint32_t storedHash =
        static_cast<std::uint32_t>(bytes[bytes.size() - 4]) |
        (static_cast<std::uint32_t>(bytes[bytes.size() - 3]) << 8) |
        (static_cast<std::uint32_t>(bytes[bytes.size() - 2]) << 16) |
        (static_cast<std::uint32_t>(bytes[bytes.size() - 1]) << 24);

    if (fnv1a(bytes.data(), bytes.size() - 4) != storedHash) {
        error = "The code is incomplete or has been altered.";
        return false;
    }

    ByteReader reader(bytes.data(), bytes.size() - 4);
    std::uint8_t header[5]{};
    for (auto& byte : header) {
        if (!reader.getU8(byte)) {
            error = "The code header is incomplete.";
            return false;
        }
    }

    if (header[0] != 'S' || header[1] != 'P' ||
        header[2] != 'C' || header[3] != '1' ||
        header[4] != 1) {
        error = "Unsupported style-code version.";
        return false;
    }

    style = StyleData{};

    if (!reader.getString(style.name) || !reader.getString(style.creator) ||
        !reader.getI32(style.background) || !reader.getI32(style.ground) ||
        !reader.getI32(style.middleGround)) {
        error = "The style metadata is invalid.";
        return false;
    }

    if (style.background < 0 || style.background > 10000 ||
        style.ground < 0 || style.ground > 10000 ||
        style.middleGround < 0 || style.middleGround > 10000) {
        error = "The style contains out-of-range background settings.";
        return false;
    }

    std::uint32_t colorCount = 0;
    if (!reader.getU32(colorCount) || colorCount > kMaxColorActions) {
        error = "The style contains too many color channels.";
        return false;
    }

    style.colors.reserve(colorCount);
    for (std::uint32_t i = 0; i < colorCount; ++i) {
        ColorRecord color;
        if (!reader.getI32(color.id) || color.id < 0 || color.id > 100000) {
            error = "A color channel ID is invalid.";
            return false;
        }
        for (auto& channel : color.from) {
            if (!reader.getU8(channel)) {
                error = "A color record is incomplete.";
                return false;
            }
        }
        for (auto& channel : color.to) {
            if (!reader.getU8(channel)) {
                error = "A color record is incomplete.";
                return false;
            }
        }
        if (!reader.getU8(color.blending) || color.blending > 1 ||
            !reader.getI32(color.playerColor) ||
            !reader.getFloat(color.fromOpacity) ||
            !reader.getFloat(color.toOpacity) ||
            color.fromOpacity < 0.f || color.fromOpacity > 1.f ||
            color.toOpacity < 0.f || color.toOpacity > 1.f) {
            error = "A color record contains invalid values.";
            return false;
        }
        style.colors.push_back(color);
    }

    std::uint32_t objectCount = 0;
    if (!reader.getU32(objectCount) || objectCount > kMaxObjects) {
        error = "The style contains too many objects.";
        return false;
    }

    style.objects.reserve(objectCount);
    for (std::uint32_t i = 0; i < objectCount; ++i) {
        ObjectRecord object;
        if (!reader.getU8(object.kind) || object.kind > 1 ||
            !reader.getString(object.saveString) ||
            object.saveString.empty()) {
            error = "An object record is invalid.";
            return false;
        }
        style.objects.push_back(std::move(object));
    }

    if (!reader.atEnd()) {
        error = "The code contains unexpected trailing data.";
        return false;
    }

    return true;
}

GJBaseGameLayer* getActiveGameLayer() {
    if (auto* editor = LevelEditorLayer::get()) {
        return editor;
    }
    return PlayLayer::get();
}

bool isDecoration(GameObject* object) {
    // GameObjectType::Decoration is value 7 in the 2.2081 bindings.
    return object && static_cast<int>(object->m_objectType) == 7;
}

StyleData scanLayer(GJBaseGameLayer* layer) {
    StyleData result;
    if (!layer) {
        return result;
    }

    if (layer->m_level) {
        result.name = layer->m_level->m_levelName.c_str();
        result.creator = layer->m_level->m_creatorName.c_str();
    }

    if (layer->m_levelSettings) {
        result.background = layer->m_levelSettings->m_backgroundIndex;
        result.ground = layer->m_levelSettings->m_groundIndex;
        result.middleGround = layer->m_levelSettings->m_middleGroundIndex;
    }

    if (auto* manager = layer->m_effectManager) {
        if (auto* actions = manager->getAllColorActions()) {
            auto count = std::min<std::uint32_t>(
                actions->count(),
                kMaxColorActions
            );

            result.colors.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                auto* action =
                    static_cast<ColorAction*>(actions->objectAtIndex(i));
                if (!action || action->m_colorID < 0 ||
                    action->m_colorID > 100000) {
                    continue;
                }

                ColorRecord record;
                record.id = action->m_colorID;
                record.from = {
                    action->m_fromColor.r,
                    action->m_fromColor.g,
                    action->m_fromColor.b
                };
                record.to = {
                    action->m_toColor.r,
                    action->m_toColor.g,
                    action->m_toColor.b
                };
                record.blending = action->m_blending ? 1 : 0;
                record.playerColor = action->m_playerColor;
                record.fromOpacity = std::clamp(action->m_fromOpacity, 0.f, 1.f);
                record.toOpacity = std::clamp(action->m_toOpacity, 0.f, 1.f);
                result.colors.push_back(record);
            }
        }
    }

    if (auto* objects = layer->m_objects) {
        auto count = std::min<std::uint32_t>(objects->count(), kMaxObjects);
        result.objects.reserve(count);

        for (std::uint32_t i = 0; i < count; ++i) {
            auto* object = static_cast<GameObject*>(objects->objectAtIndex(i));
            if (!object) {
                continue;
            }

            std::uint8_t kind = 0;
            if (isDecoration(object)) {
                kind = 0;
            } else if (object->m_isTrigger &&
                       isStyleTriggerID(object->m_objectID)) {
                kind = 1;
            } else {
                continue;
            }

            auto saveString = object->getSaveString(layer);
            if (saveString.empty() || saveString.size() > kMaxFieldBytes) {
                continue;
            }

            result.objects.push_back({
                kind,
                std::string(saveString.c_str())
            });
        }
    }

    return result;
}

bool hasGroup(int group, std::unordered_set<int> const& used) {
    return group > 0 && used.find(group) != used.end();
}

void collectGroups(
    GameObject* object,
    std::unordered_set<int>& groups
) {
    if (!object || !object->m_groups) {
        return;
    }

    for (std::size_t i = 0; i < 10; ++i) {
        auto group = (*object->m_groups)[i];
        if (group > 0 && group <= kMaxGroupId) {
            groups.insert(group);
        }
    }
}

bool remapImportedGroups(
    LevelEditorLayer* editor,
    CCArray* added,
    std::string& error
) {
    if (!editor || !added || !editor->m_objects) {
        return true;
    }

    std::unordered_set<int> occupied;
    for (std::uint32_t i = 0; i < editor->m_objects->count(); ++i) {
        auto* object =
            static_cast<GameObject*>(editor->m_objects->objectAtIndex(i));
        collectGroups(object, occupied);
    }

    // The paste operation has already inserted the objects. Their group IDs
    // are remapped to fresh IDs so imported visual triggers do not target
    // groups belonging to the user's existing layout.
    std::unordered_map<int, short> remap;
    int nextCandidate = 1;

    auto mapGroup = [&](int oldGroup) -> short {
        if (oldGroup <= 0 || oldGroup > kMaxGroupId) {
            return static_cast<short>(oldGroup);
        }
        auto found = remap.find(oldGroup);
        if (found != remap.end()) {
            return found->second;
        }

        while (nextCandidate <= kMaxGroupId &&
               hasGroup(nextCandidate, occupied)) {
            ++nextCandidate;
        }
        if (nextCandidate > kMaxGroupId) {
            return 0;
        }

        auto mapped = static_cast<short>(nextCandidate++);
        occupied.insert(mapped);
        remap.emplace(oldGroup, mapped);
        return mapped;
    };

    std::vector<GameObject*> imported;
    imported.reserve(added->count());

    for (std::uint32_t i = 0; i < added->count(); ++i) {
        auto* object = static_cast<GameObject*>(added->objectAtIndex(i));
        if (!object) {
            continue;
        }
        imported.push_back(object);

        if (object->m_groups) {
            for (std::size_t groupIndex = 0; groupIndex < 10; ++groupIndex) {
                auto oldGroup = (*object->m_groups)[groupIndex];
                if (oldGroup > 0) {
                    if (mapGroup(oldGroup) == 0) {
                        error = "No unused group IDs are available for safe import.";
                        return false;
                    }
                }
            }
        }

        if (object->m_isTrigger &&
            isStyleTriggerID(object->m_objectID)) {
            auto* effect = static_cast<EffectGameObject*>(object);
            if (effect->m_targetGroupID > 0 &&
                mapGroup(effect->m_targetGroupID) == 0) {
                error = "No unused group IDs are available for trigger import.";
                return false;
            }
            if (effect->m_centerGroupID > 0 &&
                mapGroup(effect->m_centerGroupID) == 0) {
                error = "No unused group IDs are available for trigger import.";
                return false;
            }
        }
    }

    for (auto* object : imported) {
        if (object->m_groups) {
            for (std::size_t groupIndex = 0; groupIndex < 10; ++groupIndex) {
                auto oldGroup = (*object->m_groups)[groupIndex];
                auto found = remap.find(oldGroup);
                if (found != remap.end()) {
                    (*object->m_groups)[groupIndex] = found->second;
                }
            }
        }

        if (object->m_isTrigger &&
            isStyleTriggerID(object->m_objectID)) {
            auto* effect = static_cast<EffectGameObject*>(object);
            auto target = remap.find(effect->m_targetGroupID);
            if (target != remap.end()) {
                effect->m_targetGroupID = target->second;
            }
            auto center = remap.find(effect->m_centerGroupID);
            if (center != remap.end()) {
                effect->m_centerGroupID = center->second;
            }
        }
    }

    return true;
}

void applyPalette(StyleData const& style, LevelEditorLayer* editor) {
    if (!editor) {
        return;
    }

    if (editor->m_levelSettings) {
        editor->m_levelSettings->m_backgroundIndex = style.background;
        editor->m_levelSettings->m_groundIndex = style.ground;
        editor->m_levelSettings->m_middleGroundIndex = style.middleGround;
    }

    auto* manager = editor->m_effectManager;
    if (!manager) {
        return;
    }

    for (auto const& color : style.colors) {
        cocos2d::ccColor3B fromColor{
            color.from[0], color.from[1], color.from[2]
        };
        cocos2d::ccColor3B toColor{
            color.to[0], color.to[1], color.to[2]
        };

        auto* action = ColorAction::create(
            fromColor,
            toColor,
            0.f,
            0.0,
            color.blending != 0,
            color.playerColor,
            color.fromOpacity,
            color.toOpacity
        );

        if (!action) {
            continue;
        }

        action->m_colorID = color.id;
        manager->setColorAction(action, color.id);
    }

    manager->calculateBaseActiveColors();
    manager->processColors();
}

class StylePalettePopup final : public Popup {
protected:
    CCNode* m_scanPanel = nullptr;
    CCNode* m_importPanel = nullptr;
    TextInput* m_exportInput = nullptr;
    TextInput* m_importInput = nullptr;
    CCLabelBMFont* m_importInfo = nullptr;
    CCLabelBMFont* m_status = nullptr;
    std::string m_importCode;

    bool init() {
        if (!Popup::init(440.f, 285.f)) {
            return false;
        }

        setTitle("Style & Color Palette Copier");

        auto* tabs = CCMenu::create();
        if (!tabs) {
            return false;
        }

        auto* scanTab = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Scan / Export"),
            this,
            menu_selector(StylePalettePopup::onShowScan)
        );
        auto* importTab = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Import / Apply"),
            this,
            menu_selector(StylePalettePopup::onShowImport)
        );
        if (!scanTab || !importTab) {
            return false;
        }

        scanTab->setPosition(-85.f, 0.f);
        importTab->setPosition(85.f, 0.f);
        tabs->addChild(scanTab);
        tabs->addChild(importTab);
        m_mainLayer->addChildAtPosition(tabs, Anchor::Center, ccp(0.f, 92.f));

        m_scanPanel = CCNode::create();
        m_importPanel = CCNode::create();
        if (!m_scanPanel || !m_importPanel) {
            return false;
        }
        m_mainLayer->addChild(m_scanPanel);
        m_mainLayer->addChild(m_importPanel);

        auto* scanLabel = CCLabelBMFont::create(
            "Scan decoration, style triggers, and palette channels",
            "bigFont.fnt"
        );
        if (scanLabel) {
            scanLabel->setScale(0.42f);
            m_scanPanel->addChildAtPosition(
                scanLabel, Anchor::Center, ccp(0.f, 57.f)
            );
        }

        m_exportInput = TextInput::create(390.f, "Generated code appears here");
        if (!m_exportInput) {
            return false;
        }
        m_exportInput->setMaxCharCount(static_cast<int>(kMaxCodeChars));
        m_scanPanel->addChildAtPosition(
            m_exportInput, Anchor::Center, ccp(0.f, 17.f)
        );

        auto* scanMenu = CCMenu::create();
        auto* scanButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Scan Level Style & Palette"),
            this,
            menu_selector(StylePalettePopup::onScan)
        );
        auto* copyButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Copy Code"),
            this,
            menu_selector(StylePalettePopup::onCopy)
        );
        if (!scanMenu || !scanButton || !copyButton) {
            return false;
        }
        scanButton->setPosition(-95.f, 0.f);
        copyButton->setPosition(95.f, 0.f);
        scanMenu->addChild(scanButton);
        scanMenu->addChild(copyButton);
        m_scanPanel->addChildAtPosition(
            scanMenu, Anchor::Center, ccp(0.f, -28.f)
        );

        m_importInput = TextInput::create(390.f, "Paste a StylePaletteCopier code");
        if (!m_importInput) {
            return false;
        }
        m_importInput->setMaxCharCount(static_cast<int>(kMaxCodeChars));
        m_importInput->setCallback([this](std::string const& value) {
            m_importCode = value;
            updateImportInfo();
        });
        m_importPanel->addChildAtPosition(
            m_importInput, Anchor::Center, ccp(0.f, 42.f)
        );

        m_importInfo = CCLabelBMFont::create(
            "Style / Theme Name: (paste code)",
            "bigFont.fnt"
        );
        if (m_importInfo) {
            m_importInfo->setScale(0.43f);
            m_importPanel->addChildAtPosition(
                m_importInfo, Anchor::Center, ccp(0.f, 5.f)
            );
        }

        auto* applyMenu = CCMenu::create();
        auto* applyButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Apply Style to Current Editor Level"),
            this,
            menu_selector(StylePalettePopup::onApply)
        );
        if (!applyMenu || !applyButton) {
            return false;
        }
        applyMenu->addChild(applyButton);
        m_importPanel->addChildAtPosition(
            applyMenu, Anchor::Center, ccp(0.f, -38.f)
        );

        m_status = CCLabelBMFont::create("", "bigFont.fnt");
        if (m_status) {
            m_status->setScale(0.38f);
            m_mainLayer->addChildAtPosition(
                m_status, Anchor::Center, ccp(0.f, -105.f)
            );
        }

        m_scanPanel->setVisible(true);
        m_importPanel->setVisible(false);
        return true;
    }

    void updateImportInfo() {
        if (!m_importInfo) {
            return;
        }

        StyleData parsed;
        std::string error;
        if (!decodeStyle(m_importCode, parsed, error)) {
            m_importInfo->setString("Style / Theme Name: (invalid or incomplete code)");
            return;
        }

        std::string text =
            "Style / Theme Name: " + parsed.name +
            "    Original Creator: " + parsed.creator;
        m_importInfo->setString(text.c_str());
    }

    void setStatus(std::string const& text) {
        if (m_status) {
            m_status->setString(text.c_str());
        }
    }

    void onShowScan(CCObject*) {
        m_scanPanel->setVisible(true);
        m_importPanel->setVisible(false);
        setStatus("");
    }

    void onShowImport(CCObject*) {
        m_scanPanel->setVisible(false);
        m_importPanel->setVisible(true);
        setStatus("");
    }

    void onScan(CCObject*) {
        auto* layer = getActiveGameLayer();
        if (!layer) {
            setStatus("Open a level or the editor before scanning.");
            return;
        }

        auto style = scanLayer(layer);
        std::string code;
        if (!encodeStyle(style, code)) {
            setStatus("The style is too large to encode safely.");
            return;
        }

        if (m_exportInput) {
            m_exportInput->setString(gd::string(code.c_str()), false);
        }
        setStatus("Scan complete. Use Copy Code to copy the portable code.");
    }

    void onCopy(CCObject*) {
        if (!m_exportInput) {
            setStatus("Scan a level before copying.");
            return;
        }

        auto text = std::string(m_exportInput->getString().c_str());
        if (text.empty()) {
            setStatus("Scan a level before copying.");
            return;
        }

        utils::clipboard::write(text);
        setStatus("Code copied to the clipboard.");
    }

    void onApply(CCObject*) {
        auto* editor = LevelEditorLayer::get();
        if (!editor || !editor->m_editorUI) {
            setStatus("Import is available only in the level editor.");
            return;
        }

        StyleData style;
        std::string error;
        if (!decodeStyle(m_importCode, style, error)) {
            setStatus(error);
            return;
        }

        std::string pasteData;
        for (auto const& record : style.objects) {
            // Kind is checked again at import time; the serialized record is
            // parsed by the editor's native paste implementation.
            if (record.kind > 1 || record.saveString.empty()) {
                setStatus("The code contains an invalid object record.");
                return;
            }
            pasteData += record.saveString;
            if (pasteData.empty() || pasteData.back() != ';') {
                pasteData.push_back(';');
            }
        }

        CCArray* added = nullptr;
        if (!pasteData.empty()) {
            added = editor->m_editorUI->pasteObjects(
                gd::string(pasteData.c_str()),
                true,
                false
            );

            if (!added || added->count() != style.objects.size()) {
                setStatus("The editor did not accept every imported object.");
                return;
            }

            if (!remapImportedGroups(editor, added, error)) {
                setStatus(error);
                return;
            }

            editor->dirtifyTriggers();
            editor->m_editorUI->updateButtons();
            editor->m_editorUI->updateObjectInfoLabel();
        }

        applyPalette(style, editor);

        if (editor->m_editorUI) {
            editor->m_editorUI->updateButtons();
            editor->m_editorUI->updateObjectInfoLabel();
        }

        setStatus("Style applied. Existing level objects were not deleted.");
    }

public:
    static StylePalettePopup* create() {
        auto* result = new StylePalettePopup();
        if (result && result->init()) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }

    static void toggle() {
        auto* director = CCDirector::sharedDirector();
        auto* scene = director ? director->getRunningScene() : nullptr;
        if (!scene) {
            return;
        }

        if (auto* existing = scene->getChildByTag(kPopupTag)) {
            existing->removeFromParentAndCleanup(true);
            return;
        }

        auto* popup = create();
        if (!popup) {
            return;
        }
        popup->setTag(kPopupTag);
        popup->show();
    }
};

} // namespace

class $modify(StylePaletteKeyboardHook, CCKeyboardDispatcher) {
    void dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat) {
        CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat);

        if (key == KEY_F3 && isKeyDown && !isKeyRepeat) {
            if (LevelEditorLayer::get() || PlayLayer::get()) {
                StylePalettePopup::toggle();
            }
        }
    }
};
