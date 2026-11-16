#include "buff_hud.hpp"
#include "tooltip_text.hpp"

#include "core/buff_display_bus.hpp"
#include "core/buff_presentation.hpp"
#include "core/services.hpp"
#include "icon_frame_backend.hpp"
#include "panel_overlap.hpp"
#include "native/native_contract.hpp"
#include "skill_icon_resolver.hpp"

#include <Windows.h>

#include <D2RLPlugin/localization.h>

#include <algorithm>
#include <cmath>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>


namespace BuffPanel::Systems::BuffHud {
namespace {

using Internal::IconFrameBackendState;
using Internal::SkillIconDescriptor;

// Slots 0..20 are the buff group (BuffGrid/BuffSlotNN, driven through the
// WidgetService), slots 21..34 the debuff group (DebuffGrid/DebuffSlotNN,
// driven on the native widgets: D2RLoader's widget handle table has no room
// for a second group).
constexpr std::size_t BuffSlotCount = Core::MaximumDisplayedBuffs;
constexpr std::size_t DebuffSlotCount = 14;
constexpr std::size_t SlotCount = BuffSlotCount + DebuffSlotCount;
constexpr std::size_t BuffGroup = 0;
constexpr std::size_t DebuffGroup = 1;
constexpr std::size_t GroupCount = 2;
constexpr std::array<const char*, GroupCount> GroupNames{"buffs", "debuffs"};
constexpr std::array<const char*, GroupCount> GridNames{"BuffGrid", "DebuffGrid"};
// Saved positions per graphics mode: 0 legacy, 1 HD.
constexpr std::size_t ModeCount = 2;
constexpr std::array<const char*, ModeCount> ModeNames{"legacy", "hd"};
constexpr std::size_t AtlasCount = 9;
constexpr std::size_t ItemCodeBytes = 8;
// Item_<code> pictures of the companion layouts. The game does not apply a
// layout's "visible": false, and widgets cannot be enumerated, so these and
// every item_code of buff-hud-display.txt are hidden by name.
constexpr std::array<const char*, 15> LayoutItemCodes{
    "hp1", "hp2", "hp3", "hp4", "hp5", "mp1", "mp2", "mp3", "mp4", "mp5", "rvs", "rvl", "vps", "yps", "wms",
};
constexpr std::uint32_t FramesPerSecond = 25;
constexpr char TimerReserve[] = "__BUFF_PANEL_TIMER_RESERVE_00__";
constexpr std::size_t TimerReserveBytes = sizeof(TimerReserve);
using Internal::TooltipReserveBytes;
using Internal::TooltipReserveLength;
constexpr std::size_t HoverTextBytes = 127;
constexpr std::size_t TooltipReserveCodepoints = TooltipReserveLength / 3;
static_assert(TooltipReserveCodepoints * 3 == TooltipReserveLength);
constexpr std::size_t TooltipNativeScanBytes = 0x1000;
constexpr std::uint32_t NoRenderedSeconds = std::numeric_limits<std::uint32_t>::max();
constexpr std::int32_t NoRenderedValue = std::numeric_limits<std::int32_t>::min();
constexpr char PanelName[] = "buff-panel/BuffHud";
// While the panel is missing (D2R is rebuilding its UI), ask D2RLoader to open it
// again at most this often instead of on every UI poll.
constexpr std::uint32_t ReopenPollInterval = 30;
// hide_with_panels. The stash and the Horadric Cube hide the icons whenever
// one is open (owner, 2026-11-05: in legacy graphics the icons cover the
// stash's corner and its close button).
constexpr std::array<const char*, 3> HidingPanels{
    "BankExpansionLayout",
    "BankOriginalLayout",
    "HoradricCubeLayout",
};
// Side panels hide them only while an occupied slot overlaps the panel: a
// player fights with the inventory or the character panel open. In legacy
// graphics a left panel always reaches the first slot; a right panel the
// sixth or seventh. In HD only the top row reaches a right panel.
struct SidePanel final {
    const char* name;
    Internal::PanelSide side;
};
constexpr std::array<SidePanel, 12> SidePanels{{
    {"CharacterStatsPanel", Internal::PanelSide::Left},
    {"QuestLogPanelExpansion", Internal::PanelSide::Left},
    {"QuestLogPanelOriginal", Internal::PanelSide::Left},
    {"WaypointsPanelExpansion", Internal::PanelSide::Left},
    {"WaypointsPanelOriginal", Internal::PanelSide::Left},
    {"VendorPanelLayout", Internal::PanelSide::Left},
    {"TradeLayout", Internal::PanelSide::Left},
    {"HirelingInventoryPanel", Internal::PanelSide::Left},
    {"PartyPanel", Internal::PanelSide::Left},
    {"PlayerInventoryExpansionLayout", Internal::PanelSide::Right},
    {"PlayerInventoryOriginalLayout", Internal::PanelSide::Right},
    {"SkillsTreePanel", Internal::PanelSide::Right},
}};
// Further native panel names `buff-panel panels` reports (diagnostics only).
constexpr std::array<const char*, 4> ProbedPanels{
    "AdvancedStatsPanel",
    "HireablesPanel",
    "ImbueItemsPanel",
    "NpcDialogPanel",
};

enum class TooltipStorageEncoding : std::uint8_t {
    Unknown = 0,
    BlizzardUtf8 = 1,
    BlizzardUtf16 = 2,
};
static_assert(TimerReserveBytes == 32);
// The text of the HoverName TextBoxWidget: a name up to 63 bytes is written
// over it (the game keeps the reserve's buffer).
constexpr char NameReserve[] = "__BUFF_PANEL_NAME_RESERVE______________________________________";
constexpr std::size_t NameReserveBytes = sizeof(NameReserve);
static_assert(NameReserveBytes == 64);

constexpr std::array<Core::BuffIconAtlas, AtlasCount> AtlasOrder{
    Core::BuffIconAtlas::Amazon,
    Core::BuffIconAtlas::Sorceress,
    Core::BuffIconAtlas::Necromancer,
    Core::BuffIconAtlas::Paladin,
    Core::BuffIconAtlas::Barbarian,
    Core::BuffIconAtlas::Druid,
    Core::BuffIconAtlas::Assassin,
    Core::BuffIconAtlas::Warlock,
    Core::BuffIconAtlas::Global,
};

constexpr std::array<const char*, AtlasCount> AtlasWidgetNames{
    "IconAmazon",
    "IconSorceress",
    "IconNecromancer",
    "IconPaladin",
    "IconBarbarian",
    "IconDruid",
    "IconAssassin",
    "IconWarlock",
    "IconGlobal",
};

using FindTopLevelPanelFn = void*(__fastcall*)(const char* name) noexcept;
using FindChildWidgetByNameFn = void*(__fastcall*)(void* parent, const char* name) noexcept;
using WidgetSetVisibleFn = void(__fastcall*)(void* widget, bool visible) noexcept;
using WidgetSetCoordinateFn = void(__fastcall*)(void* widget, std::int32_t value) noexcept;

const D2RL::PluginContext* Context{};
const D2RL::PanelService* Panels{};
const D2RL::WidgetService* Widgets{};
const D2RL::ThreadService* Threads{};
const D2RL::LifecycleService* Lifecycle{};
D2RL::Panels::RegistrationHandle RegisteredPanel{D2RL::Panels::InvalidHandle};
D2RL::Lifecycle::ListenerHandle DataTablesListener{D2RL::Lifecycle::InvalidHandle};
std::array<D2RL::Lifecycle::ListenerHandle, 3> GameplayListeners{};

FindTopLevelPanelFn FindTopLevelPanel{};
FindChildWidgetByNameFn FindChildWidgetByName{};
// Native widget setters; null when their fingerprint does not match, which
// switches the debuff group, the hover label and moving off.
WidgetSetVisibleFn NativeSetVisibleFn{};
WidgetSetCoordinateFn NativeSetXFn{};
WidgetSetCoordinateFn NativeSetYFn{};

std::atomic<std::uint64_t> CurrentSessionGeneration{};
std::atomic<bool> PollScheduled{};
struct SlotHandles final {
    D2RL::Widgets::WidgetHandle slot{D2RL::Widgets::InvalidHandle};
    std::array<D2RL::Widgets::WidgetHandle, AtlasCount> icons{};
    // Native Item_<code> picture currently shown instead of an atlas icon, if any.
    void* item{};
    D2RL::Widgets::WidgetHandle countdown{D2RL::Widgets::InvalidHandle};
    D2RL::Widgets::WidgetHandle tooltip{D2RL::Widgets::InvalidHandle};
};

struct SlotRenderState final {
    bool visible{};
    std::uint64_t key{};
    Core::BuffIconAtlas atlas{Core::BuffIconAtlas::Global};
    std::uint16_t requestedFrame{};
    std::uint16_t renderedFrame{};
    char itemCode[ItemCodeBytes]{};
    Core::BuffDisplayMode displayMode{Core::BuffDisplayMode::Timer};
    std::uint32_t seconds{NoRenderedSeconds};
    std::int32_t currentValue{NoRenderedValue};
    std::int32_t maximumValue{NoRenderedValue};
    std::int32_t sourceSkillId{Core::NoSourceSkillId};
    bool timerVisible{};
    bool tooltipVisible{};
    std::array<char, HoverTextBytes> hoverText{};
    std::uintptr_t qualifiedTimerBuffer{};
    std::uintptr_t qualifiedTooltipWidget{};
    std::uintptr_t qualifiedTooltipBuffer{};
    std::size_t qualifiedTooltipFieldOffset{std::numeric_limits<std::size_t>::max()};
    TooltipStorageEncoding qualifiedTooltipEncoding{TooltipStorageEncoding::Unknown};
};

D2RL::Widgets::WidgetHandle HudPanel{D2RL::Widgets::InvalidHandle};
D2RL::Widgets::WidgetHandle GridWidget{D2RL::Widgets::InvalidHandle};
// The hidden "UiSpace" widget of a layout names the height of the UI space it
// is authored in (HD: 2160); 0 means the classic screen.
D2RL::Widgets::WidgetHandle UiSpaceWidget{D2RL::Widgets::InvalidHandle};
std::array<SlotHandles, BuffSlotCount> Handles{};
std::array<SlotRenderState, SlotCount> RenderStates{};
bool HandlesResolved{};

// Native widgets of the live panel, looked up once per panel instance.
struct NativeSlot final {
    void* slot{};
    std::array<void*, AtlasCount> icons{};
    void* countdown{};
};
std::array<NativeSlot, SlotCount> NativeSlots{};
std::array<void*, GroupCount> NativeGrids{};
void* NativeHoverName{};
bool DebuffsReady{};

// The native enabled bit, learned like the visible bit (QualifyEnabledFlag).
bool EnabledFlagQualified{};
std::size_t EnabledFlagOffset{};
std::uint8_t EnabledFlagMask{};
bool EnabledFlagSetWhenEnabled{};

// The hover label: its qualified text buffer and what it shows.
std::uintptr_t QualifiedNameBuffer{};
bool HoverNameShown{};
std::array<char, NameReserveBytes> HoverNameText{};

// Group positions. PositionMutex guards SavedPositions, Drag and Hits: the
// game window's message procedure (drag) and the UI poll (apply, save) share them.
std::mutex PositionMutex{};
std::array<std::array<Core::PanelPosition, GroupCount>, ModeCount> SavedPositions{};
bool SaveRequested{};
// The JSON spot of each grid (its local x/y), read from the first panel of a mode.
struct GroupDefault final {
    bool known{};
    std::int32_t x{};
    std::int32_t y{};
};
std::array<std::array<GroupDefault, GroupCount>, ModeCount> GroupDefaults{};

// Where the UI screen sits in the game window's client area.
struct UiFrame final {
    bool valid{};
    bool hd{};
    double width{};
    double height{};
    double scaleX{};
    double scaleY{};
    double left{};
};

// One group on the UI screen: its grid's top-left and the boxes of its
// occupied slots (UI units).
struct GroupHit final {
    bool valid{};
    double left{};
    double top{};
    double width{};
    double height{};
    std::size_t count{};
    std::array<Internal::UiBox, BuffSlotCount> slots{};
    std::array<std::size_t, BuffSlotCount> slotIndex{};
};

struct HitSnapshot final {
    bool valid{};
    UiFrame frame{};
    std::array<GroupHit, GroupCount> groups{};
};
HitSnapshot Hits{};

struct DragState final {
    bool active{};
    std::size_t group{};
    std::size_t mode{};
    double grabX{};
    double grabY{};
};
DragState Drag{};
std::atomic<std::uint64_t> DragsCompleted{};

HWND SubclassedWindow{};
WNDPROC PreviousWndProc{};
// Native BuffHud panel the handles and qualified buffers belong to. D2R
// destroys and rebuilds every UI panel when the graphics mode changes (G) or the
// UI reloads; a different or missing pointer means all cached widget state is stale.
void* ResolvedNativePanel{};
std::uint32_t PollsWithoutPanel{};
std::uint64_t HiddenItemGeneration{std::numeric_limits<std::uint64_t>::max()};
constexpr std::size_t VisibleFlagScanBytes = 0x200;
bool VisibleFlagQualified{};
std::size_t VisibleFlagOffset{};
std::uint8_t VisibleFlagMask{};
bool VisibleFlagSetWhenShown{};
std::uint64_t LastRenderedRevision{std::numeric_limits<std::uint64_t>::max()};
bool HoverGeometryLogged{};
bool HideWithPanels{true};
// The open panel the icons are hidden for, or null. HudShownApplied is the last
// visibility given to HudPanel while HudShownKnown (reset with the handles).
const char* HidingPanel{};
bool HudShownKnown{};
bool HudShownApplied{};

[[nodiscard]] std::size_t AtlasIndex(Core::BuffIconAtlas atlas) noexcept {
    for (std::size_t i = 0; i < AtlasOrder.size(); ++i) {
        if (AtlasOrder[i] == atlas) return i;
    }
    return AtlasOrder.size() - 1;
}

[[nodiscard]] bool IsReadableRange(const void* address, std::size_t size) noexcept {
    if (address == nullptr || size == 0) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) == 0
        || info.State != MEM_COMMIT
        || (info.Protect & PAGE_GUARD) != 0
        || (info.Protect & PAGE_NOACCESS) != 0) {
        return false;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto regionStart = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto regionEnd = regionStart + info.RegionSize;
    return start >= regionStart && start <= regionEnd && size <= regionEnd - start;
}

[[nodiscard]] bool IsWritableRange(void* address, std::size_t size) noexcept {
    if (!IsReadableRange(address, size)) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) == 0) return false;
    const DWORD baseProtection = info.Protect & 0xFFU;
    return baseProtection == PAGE_READWRITE
        || baseProtection == PAGE_WRITECOPY
        || baseProtection == PAGE_EXECUTE_READWRITE
        || baseProtection == PAGE_EXECUTE_WRITECOPY;
}

template <typename T>
[[nodiscard]] bool ReadNativeField(const void* base, std::size_t offset, T& value) noexcept {
    if (base == nullptr) return false;
    const auto baseValue = reinterpret_cast<std::uintptr_t>(base);
    if (offset > static_cast<std::size_t>(UINTPTR_MAX - baseValue)) return false;
    const auto* address = reinterpret_cast<const void*>(baseValue + offset);
    if (!IsReadableRange(address, sizeof(T))) return false;
    std::memcpy(&value, address, sizeof(T));
    return true;
}

[[nodiscard]] bool IsTimerReserve(const char* buffer) noexcept {
    return buffer != nullptr
        && IsReadableRange(buffer, TimerReserveBytes)
        && std::memcmp(buffer, TimerReserve, TimerReserveBytes) == 0;
}

void MakeTooltipReserve(std::size_t slotIndex, char (&output)[TooltipReserveBytes]) noexcept {
    // Use a per-slot *invisible* UTF-8 qualification token instead of a long
    // run of ASCII spaces. The old reserve was visually blank but D2R still
    // measured all 127 spaces when the native tooltip field could not be
    // uniquely qualified, producing a huge empty tooltip. U+200B ZERO WIDTH
    // SPACE and U+200C ZERO WIDTH NON-JOINER give us 42 zero-width codepoints
    // (126 UTF-8 bytes) while the low five codepoints encode the slot id so the
    // backing std::string is unique for every one of the 21 reusable slots.
    constexpr std::array<unsigned char, 3> ZeroWidthSpace{0xE2, 0x80, 0x8B};
    constexpr std::array<unsigned char, 3> ZeroWidthNonJoiner{0xE2, 0x80, 0x8C};
    std::memset(output, 0, TooltipReserveBytes);
    for (std::size_t i = 0; i < TooltipReserveCodepoints; ++i) {
        const bool idBit = i < 5 && ((slotIndex >> i) & 1U) != 0;
        const auto& codepoint = idBit ? ZeroWidthNonJoiner : ZeroWidthSpace;
        std::memcpy(output + i * 3, codepoint.data(), codepoint.size());
    }
    output[TooltipReserveLength] = '\0';
}

void MakeTooltipReserveUtf16(std::size_t slotIndex, std::array<std::uint16_t, TooltipReserveCodepoints + 1>& output) noexcept {
    output.fill(0);
    for (std::size_t i = 0; i < TooltipReserveCodepoints; ++i) {
        const bool idBit = i < 5 && ((slotIndex >> i) & 1U) != 0;
        output[i] = static_cast<std::uint16_t>(idBit ? 0x200CU : 0x200BU);
    }
}

[[nodiscard]] bool Utf8ToUtf16(
    std::string_view input,
    std::array<std::uint16_t, TooltipReserveCodepoints + 1>& output,
    std::size_t& outputLength) noexcept {
    output.fill(0);
    outputLength = 0;
    std::size_t i{};
    while (i < input.size()) {
        const auto b0 = static_cast<unsigned char>(input[i]);
        std::uint32_t cp{};
        std::size_t count{};
        if (b0 < 0x80U) {
            cp = b0; count = 1;
        } else if ((b0 & 0xE0U) == 0xC0U && i + 1 < input.size()) {
            cp = static_cast<std::uint32_t>(b0 & 0x1FU) << 6;
            cp |= static_cast<unsigned char>(input[i + 1]) & 0x3FU;
            count = 2;
            if (cp < 0x80U) return false;
        } else if ((b0 & 0xF0U) == 0xE0U && i + 2 < input.size()) {
            cp = static_cast<std::uint32_t>(b0 & 0x0FU) << 12;
            cp |= static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 1]) & 0x3FU) << 6;
            cp |= static_cast<unsigned char>(input[i + 2]) & 0x3FU;
            count = 3;
            if (cp < 0x800U || (cp >= 0xD800U && cp <= 0xDFFFU)) return false;
        } else if ((b0 & 0xF8U) == 0xF0U && i + 3 < input.size()) {
            cp = static_cast<std::uint32_t>(b0 & 0x07U) << 18;
            cp |= static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 1]) & 0x3FU) << 12;
            cp |= static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 2]) & 0x3FU) << 6;
            cp |= static_cast<unsigned char>(input[i + 3]) & 0x3FU;
            count = 4;
            if (cp < 0x10000U || cp > 0x10FFFFU) return false;
        } else {
            return false;
        }
        for (std::size_t j = 1; j < count; ++j) {
            if ((static_cast<unsigned char>(input[i + j]) & 0xC0U) != 0x80U) return false;
        }
        if (cp <= 0xFFFFU) {
            if (outputLength >= TooltipReserveCodepoints) return false;
            output[outputLength++] = static_cast<std::uint16_t>(cp);
        } else {
            if (outputLength + 2 > TooltipReserveCodepoints) return false;
            cp -= 0x10000U;
            output[outputLength++] = static_cast<std::uint16_t>(0xD800U + (cp >> 10));
            output[outputLength++] = static_cast<std::uint16_t>(0xDC00U + (cp & 0x3FFU));
        }
        i += count;
    }
    return true;
}

[[nodiscard]] bool SetVisible(D2RL::Widgets::WidgetHandle handle, bool visible) noexcept;
[[nodiscard]] bool SetEnabled(D2RL::Widgets::WidgetHandle handle, bool enabled) noexcept;
[[nodiscard]] bool ApplyInputIsolation() noexcept;

[[nodiscard]] void* ResolveNativeSlot(std::size_t slotIndex) noexcept;
[[nodiscard]] void* ResolveNativeSlotChild(std::size_t slotIndex, const char* childName) noexcept;

// D2RLoader keeps a small table of widget handles (one per distinct path), and
// the slots already use most of it, so Item_<code> pictures are switched on the
// native widgets. The visibility bit is learned from a Countdown, whose
// visibility the WidgetService does control: toggle it and keep the one bit
// that follows. Without a single such bit, item pictures stay unqualified.
[[nodiscard]] bool QualifyVisibleFlag() noexcept {
    if (VisibleFlagQualified) return true;
    const auto countdown = Handles[0].countdown;
    const auto* native = static_cast<const std::uint8_t*>(ResolveNativeSlotChild(0, "Countdown"));
    if (native == nullptr || !IsReadableRange(native, VisibleFlagScanBytes)) return false;

    std::array<std::uint8_t, VisibleFlagScanBytes> shown{};
    std::array<std::uint8_t, VisibleFlagScanBytes> hidden{};
    std::array<std::uint8_t, VisibleFlagScanBytes> shownAgain{};
    if (!SetVisible(countdown, true)) return false;
    std::memcpy(shown.data(), native, shown.size());
    const bool hiddenSet = SetVisible(countdown, false);
    std::memcpy(hidden.data(), native, hidden.size());
    const bool shownSet = SetVisible(countdown, true);
    std::memcpy(shownAgain.data(), native, shownAgain.size());
    // RenderSlot shows the countdown again where a slot needs it.
    (void)SetVisible(countdown, false);
    if (!hiddenSet || !shownSet) return false;

    std::size_t candidates = 0;
    for (std::size_t offset = 0; offset < shown.size(); ++offset) {
        const auto difference = static_cast<std::uint8_t>(shown[offset] ^ hidden[offset]);
        if (shown[offset] != shownAgain[offset] || difference == 0) continue;
        if ((difference & (difference - 1)) != 0) return false;
        ++candidates;
        VisibleFlagOffset = offset;
        VisibleFlagMask = difference;
        VisibleFlagSetWhenShown = (shown[offset] & difference) != 0;
    }
    VisibleFlagQualified = candidates == 1;
    return VisibleFlagQualified;
}

[[nodiscard]] bool SetNativeVisible(void* widget, bool visible) noexcept {
    if (!VisibleFlagQualified || widget == nullptr) return false;
    auto* flag = static_cast<std::uint8_t*>(widget) + VisibleFlagOffset;
    if (!IsWritableRange(flag, 1)) return false;
    *flag = visible == VisibleFlagSetWhenShown
        ? static_cast<std::uint8_t>(*flag | VisibleFlagMask)
        : static_cast<std::uint8_t>(*flag & ~VisibleFlagMask);
    return true;
}

// The enabled bit, learned the same way from Countdown 0 through the
// WidgetService's setWidgetEnabled. A disabled widget is no mouse target, so
// the debuff group and the hover label, which have no handles, use it.
[[nodiscard]] bool QualifyEnabledFlag() noexcept {
    if (EnabledFlagQualified) return true;
    const auto countdown = Handles[0].countdown;
    const auto* native = static_cast<const std::uint8_t*>(ResolveNativeSlotChild(0, "Countdown"));
    if (native == nullptr || !IsReadableRange(native, VisibleFlagScanBytes)) return false;

    std::array<std::uint8_t, VisibleFlagScanBytes> enabled{};
    std::array<std::uint8_t, VisibleFlagScanBytes> disabled{};
    std::array<std::uint8_t, VisibleFlagScanBytes> enabledAgain{};
    if (!SetEnabled(countdown, true)) return false;
    std::memcpy(enabled.data(), native, enabled.size());
    const bool disabledSet = SetEnabled(countdown, false);
    std::memcpy(disabled.data(), native, disabled.size());
    const bool enabledSet = SetEnabled(countdown, true);
    std::memcpy(enabledAgain.data(), native, enabledAgain.size());
    (void)SetEnabled(countdown, false);
    if (!disabledSet || !enabledSet) return false;

    std::size_t candidates = 0;
    for (std::size_t offset = 0; offset < enabled.size(); ++offset) {
        const auto difference = static_cast<std::uint8_t>(enabled[offset] ^ disabled[offset]);
        if (enabled[offset] != enabledAgain[offset] || difference == 0) continue;
        if ((difference & (difference - 1)) != 0) return false;
        ++candidates;
        EnabledFlagOffset = offset;
        EnabledFlagMask = difference;
        EnabledFlagSetWhenEnabled = (enabled[offset] & difference) != 0;
    }
    EnabledFlagQualified = candidates == 1;
    return EnabledFlagQualified;
}

[[nodiscard]] bool SetNativeEnabled(void* widget, bool enabledState) noexcept {
    if (!EnabledFlagQualified || widget == nullptr) return false;
    auto* flag = static_cast<std::uint8_t*>(widget) + EnabledFlagOffset;
    if (!IsWritableRange(flag, 1)) return false;
    *flag = enabledState == EnabledFlagSetWhenEnabled
        ? static_cast<std::uint8_t>(*flag | EnabledFlagMask)
        : static_cast<std::uint8_t>(*flag & ~EnabledFlagMask);
    return true;
}

// Shows or hides a native widget through the game's own setter (which also
// runs the widget's show / hide method), else through the visible bit.
[[nodiscard]] bool NativeShow(void* widget, bool visible) noexcept {
    if (widget == nullptr) return false;
    if (NativeSetVisibleFn != nullptr) {
        if (!IsWritableRange(widget, 0x80)) return false;
        NativeSetVisibleFn(widget, visible);
        return true;
    }
    return SetNativeVisible(widget, visible);
}

struct NativeRect final {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t width{};
    std::int32_t height{};
};

[[nodiscard]] bool ReadNativeRect(const void* widget, NativeRect& rect) noexcept {
    return ReadNativeField(widget, Native::Contract::WidgetLocalRectOffset, rect);
}

// The anchor of a widget in its parent, two floats at +0x48.
[[nodiscard]] bool ReadNativeAnchor(const void* widget, float& x, float& y) noexcept {
    constexpr std::size_t AnchorOffset = 0x48;
    if (!ReadNativeField(widget, AnchorOffset, x) || !ReadNativeField(widget, AnchorOffset + 4, y)) return false;
    return x >= 0.0F && x <= 1.0F && y >= 0.0F && y <= 1.0F;
}

// Moves a widget to local (x, y) in its parent; only for widgets that do not
// fit their parent (the grids and the hover label).
[[nodiscard]] bool NativeMove(void* widget, std::int32_t x, std::int32_t y) noexcept {
    constexpr std::size_t FitToParentOffset = 0x52;
    NativeRect rect{};
    std::uint8_t fitsParent{1};
    if (NativeSetXFn == nullptr || NativeSetYFn == nullptr || !ReadNativeRect(widget, rect)
        || !ReadNativeField(widget, FitToParentOffset, fitsParent) || fitsParent != 0) {
        return false;
    }
    if (rect.x != x) NativeSetXFn(widget, x);
    if (rect.y != y) NativeSetYFn(widget, y);
    return true;
}

// Shows or hides an Item_<code> container together with its Picture.
[[nodiscard]] bool SetItemPictureVisible(void* item, bool visible) noexcept {
    void* picture = item != nullptr ? FindChildWidgetByName(item, "Picture") : nullptr;
    return SetNativeVisible(picture, visible) && SetNativeVisible(item, visible);
}

[[nodiscard]] void* FindItemPicture(void* slot, const char* code) noexcept {
    if (slot == nullptr || FindChildWidgetByName == nullptr) return nullptr;
    char widgetName[16]{};
    std::snprintf(widgetName, sizeof(widgetName), "Item_%s", code);
    return FindChildWidgetByName(slot, widgetName);
}

// Hides every known Item_ picture of every slot, then lets RenderSnapshot
// re-apply the pictures of occupied slots.
void HideItemPictures() noexcept {
    const auto generation = Core::BuffPresentations().Generation();
    const auto tableCodes = Core::BuffPresentations().ItemCodes();
    const bool qualified = QualifyVisibleFlag();
    std::size_t found = 0;
    std::size_t hidden = 0;
    for (std::size_t i = 0; i < Handles.size(); ++i) {
        void* slot = ResolveNativeSlot(i);
        auto hide = [&](const char* code) noexcept {
            void* item = FindItemPicture(slot, code);
            if (item == nullptr) return;
            ++found;
            hidden += SetItemPictureVisible(item, false) ? 1 : 0;
        };
        for (const auto* code : LayoutItemCodes) hide(code);
        for (const auto& code : tableCodes) hide(code.c_str());
        Handles[i].item = nullptr;
        RenderStates[i].visible = false;
    }
    if (generation != HiddenItemGeneration) {
        char line[160]{};
        std::snprintf(line, sizeof(line),
            "Buff HUD: item pictures found=%zu hidden=%zu visibleFlag=%s offset=0x%zx mask=0x%02x.",
            found, hidden, qualified ? "qualified" : "not-qualified", VisibleFlagOffset,
            static_cast<unsigned>(VisibleFlagMask));
        found == hidden ? Context->LogInfo(line) : Context->LogWarn(line);
    }
    HiddenItemGeneration = generation;
    LastRenderedRevision = std::numeric_limits<std::uint64_t>::max();
}

// HD layouts name their UI space height in the hidden UiSpace widget; legacy
// ones leave it 0.
[[nodiscard]] bool IsHdLayout() noexcept {
    D2RL::Widgets::Rect space{};
    return UiSpaceWidget != D2RL::Widgets::InvalidHandle && Widgets != nullptr
        && Widgets->getWidgetRect(Context, UiSpaceWidget, &space) == D2RL::Widgets::Result::Success
        && space.height > 0;
}

// Looks up the native widgets of both groups and the hover label, and keeps
// each grid's JSON spot as its mode's default the first time it is seen.
void ResolveNativeTree(void* panel) noexcept {
    NativeSlots = {};
    NativeGrids = {};
    NativeHoverName = nullptr;
    DebuffsReady = false;
    if (panel == nullptr || FindChildWidgetByName == nullptr) return;
    for (std::size_t group = 0; group < GroupCount; ++group) {
        NativeGrids[group] = FindChildWidgetByName(panel, GridNames[group]);
    }
    NativeHoverName = FindChildWidgetByName(panel, "HoverName");
    bool debuffsComplete = NativeGrids[DebuffGroup] != nullptr;
    for (std::size_t i = 0; i < SlotCount; ++i) {
        auto& native = NativeSlots[i];
        native.slot = ResolveNativeSlot(i);
        if (native.slot != nullptr) {
            for (std::size_t atlas = 0; atlas < AtlasCount; ++atlas) {
                native.icons[atlas] = FindChildWidgetByName(native.slot, AtlasWidgetNames[atlas]);
            }
            native.countdown = FindChildWidgetByName(native.slot, "Countdown");
        }
        if (i >= BuffSlotCount) {
            debuffsComplete = debuffsComplete && native.slot != nullptr && native.countdown != nullptr
                && std::find(native.icons.begin(), native.icons.end(), nullptr) == native.icons.end();
        }
    }
    DebuffsReady = debuffsComplete && NativeSetVisibleFn != nullptr;

    const std::size_t mode = IsHdLayout() ? 1 : 0;
    std::lock_guard lock(PositionMutex);
    for (std::size_t group = 0; group < GroupCount; ++group) {
        NativeRect rect{};
        auto& known = GroupDefaults[mode][group];
        if (!known.known && ReadNativeRect(NativeGrids[group], rect)) {
            known = {.known = true, .x = rect.x, .y = rect.y};
        }
    }
}

// The debuff group and the hover label have no WidgetService handles: keep
// every one of their widgets disabled (no mouse target) and hidden until used.
void IsolateNativeWidgets() noexcept {
    const bool qualified = QualifyEnabledFlag();
    std::size_t disabled = 0;
    std::size_t total = 0;
    auto isolate = [&](void* widget) noexcept {
        if (widget == nullptr) return;
        ++total;
        disabled += SetNativeEnabled(widget, false) ? 1 : 0;
    };
    isolate(NativeGrids[DebuffGroup]);
    isolate(NativeHoverName);
    for (std::size_t i = BuffSlotCount; i < SlotCount; ++i) {
        const auto& native = NativeSlots[i];
        isolate(native.slot);
        for (void* icon : native.icons) isolate(icon);
        isolate(native.countdown);
        isolate(native.slot != nullptr ? FindChildWidgetByName(native.slot, "Frame") : nullptr);
        (void)NativeShow(native.countdown, false);
        (void)NativeShow(native.slot, false);
    }
    (void)NativeShow(NativeHoverName, false);
    HoverNameShown = false;
    if (Context == nullptr) return;
    char line[256]{};
    std::snprintf(line, sizeof(line),
        "Buff HUD: native widgets disabled=%zu/%zu enabledFlag=%s offset=0x%zx mask=0x%02x debuffs=%s hoverLabel=%s mode=%s.",
        disabled, total, qualified ? "qualified" : "not-qualified", EnabledFlagOffset,
        static_cast<unsigned>(EnabledFlagMask), DebuffsReady ? "ready" : "missing",
        NativeHoverName != nullptr ? "ready" : "missing", IsHdLayout() ? "hd" : "legacy");
    disabled == total && qualified ? Context->LogInfo(line) : Context->LogWarn(line);
}

[[nodiscard]] bool ResolveWidgetHandles() noexcept {
    if (HandlesResolved) return true;
    if (Context == nullptr || Widgets == nullptr) return false;

    HudPanel = D2RL::Widgets::InvalidHandle;
    GridWidget = D2RL::Widgets::InvalidHandle;
    Handles = {};

    void* nativePanel = FindTopLevelPanel != nullptr ? FindTopLevelPanel(PanelName) : nullptr;
    if (nativePanel == nullptr) return false;

    if (Widgets->findPanel(Context, PanelName, &HudPanel) != D2RL::Widgets::Result::Success
        || Widgets->findWidget(Context, HudPanel, "BuffGrid", &GridWidget) != D2RL::Widgets::Result::Success) {
        return false;
    }
    if (Widgets->findWidget(Context, HudPanel, "UiSpace", &UiSpaceWidget) != D2RL::Widgets::Result::Success) {
        UiSpaceWidget = D2RL::Widgets::InvalidHandle;
    }

    for (std::size_t i = 0; i < Handles.size(); ++i) {
        char slotName[24]{};
        std::snprintf(slotName, sizeof(slotName), "BuffSlot%02zu", i);
        auto& slot = Handles[i];
        if (Widgets->findWidget(Context, GridWidget, slotName, &slot.slot)
            != D2RL::Widgets::Result::Success) {
            Handles = {};
            return false;
        }
        for (std::size_t atlas = 0; atlas < AtlasCount; ++atlas) {
            if (Widgets->findWidget(Context, slot.slot, AtlasWidgetNames[atlas], &slot.icons[atlas])
                != D2RL::Widgets::Result::Success) {
                Handles = {};
                return false;
            }
        }
        if (Widgets->findWidget(Context, slot.slot, "Countdown", &slot.countdown)
            != D2RL::Widgets::Result::Success) {
            Handles = {};
            return false;
        }
        slot.tooltip = D2RL::Widgets::InvalidHandle;

        // Atlas buttons remain disabled permanently. Hover presentation is a
        // plain TextBoxWidget driven by pointer observation, so no focusable or
        // clickable surface sits over the buff icon.
        for (const auto icon : slot.icons) {
            if (!SetEnabled(icon, false)) {
            }
        }
    }

    HandlesResolved = true;
    ResolvedNativePanel = nativePanel;
    ResolveNativeTree(nativePanel);
    HideItemPictures();
    (void)ApplyInputIsolation();
    IsolateNativeWidgets();
    return true;
}

void RestoreQualifiedCountdownBuffers() noexcept {
    for (auto& state : RenderStates) {
        if (state.qualifiedTimerBuffer == 0) continue;
        auto* buffer = reinterpret_cast<char*>(state.qualifiedTimerBuffer);
        if (IsWritableRange(buffer, TimerReserveBytes)) {
            std::memcpy(buffer, TimerReserve, TimerReserveBytes);
        }
    }
}

void RestoreQualifiedTooltipBuffer(std::size_t slotIndex, SlotRenderState& state) noexcept {
    if (slotIndex >= SlotCount
        || state.qualifiedTooltipWidget == 0
        || state.qualifiedTooltipBuffer == 0
        || state.qualifiedTooltipFieldOffset == std::numeric_limits<std::size_t>::max()
        || state.qualifiedTooltipEncoding == TooltipStorageEncoding::Unknown) {
        return;
    }

    const auto stringObjectAddress = state.qualifiedTooltipWidget + state.qualifiedTooltipFieldOffset;
    if (!IsWritableRange(
            reinterpret_cast<void*>(stringObjectAddress + Native::Contract::BlizzardStringSizeOffset),
            sizeof(std::uint64_t))) {
        return;
    }

    std::uint64_t length{};
    if (state.qualifiedTooltipEncoding == TooltipStorageEncoding::BlizzardUtf8) {
        if (!IsWritableRange(reinterpret_cast<void*>(state.qualifiedTooltipBuffer), TooltipReserveBytes)) return;
        char reserve[TooltipReserveBytes]{};
        MakeTooltipReserve(slotIndex, reserve);
        std::memcpy(reinterpret_cast<void*>(state.qualifiedTooltipBuffer), reserve, TooltipReserveBytes);
        length = TooltipReserveLength;
    } else if (state.qualifiedTooltipEncoding == TooltipStorageEncoding::BlizzardUtf16) {
        constexpr std::size_t Utf16Bytes = (TooltipReserveCodepoints + 1) * sizeof(std::uint16_t);
        if (!IsWritableRange(reinterpret_cast<void*>(state.qualifiedTooltipBuffer), Utf16Bytes)) return;
        std::array<std::uint16_t, TooltipReserveCodepoints + 1> reserve{};
        MakeTooltipReserveUtf16(slotIndex, reserve);
        std::memcpy(reinterpret_cast<void*>(state.qualifiedTooltipBuffer), reserve.data(), Utf16Bytes);
        length = TooltipReserveCodepoints;
    } else {
        return;
    }

    std::memcpy(
        reinterpret_cast<void*>(stringObjectAddress + Native::Contract::BlizzardStringSizeOffset),
        &length,
        sizeof(length));
}

void RestoreQualifiedTooltipBuffers() noexcept {
    for (std::size_t slotIndex = 0; slotIndex < RenderStates.size(); ++slotIndex) {
        RestoreQualifiedTooltipBuffer(slotIndex, RenderStates[slotIndex]);
    }
}

// The cached widget tree is the live one only while D2R still returns the same
// native panel. Qualified buffers of a destroyed panel must never be written.
[[nodiscard]] bool IsResolvedPanelLive() noexcept {
    return ResolvedNativePanel != nullptr
        && FindTopLevelPanel != nullptr
        && FindTopLevelPanel(PanelName) == ResolvedNativePanel;
}

void InvalidateWidgetHandles() noexcept {
    ResolvedNativePanel = nullptr;
    HudPanel = D2RL::Widgets::InvalidHandle;
    GridWidget = D2RL::Widgets::InvalidHandle;
    UiSpaceWidget = D2RL::Widgets::InvalidHandle;
    Handles = {};
    RenderStates = {};
    NativeSlots = {};
    NativeGrids = {};
    NativeHoverName = nullptr;
    DebuffsReady = false;
    QualifiedNameBuffer = 0;
    HoverNameShown = false;
    HoverNameText = {};
    {
        std::lock_guard lock(PositionMutex);
        Hits = {};
    }
    HandlesResolved = false;
    HudShownKnown = false;
    LastRenderedRevision = std::numeric_limits<std::uint64_t>::max();
}

[[nodiscard]] bool SetVisible(D2RL::Widgets::WidgetHandle handle, bool visible) noexcept {
    if (Context == nullptr || Widgets == nullptr || handle == D2RL::Widgets::InvalidHandle) return false;
    return Widgets->setWidgetVisible(Context, handle, visible) == D2RL::Widgets::Result::Success;
}

[[nodiscard]] bool SetEnabled(D2RL::Widgets::WidgetHandle handle, bool enabled) noexcept {
    if (Context == nullptr || Widgets == nullptr || handle == D2RL::Widgets::InvalidHandle) return false;
    return Widgets->setWidgetEnabled(Context, handle, enabled) == D2RL::Widgets::Result::Success;
}

// Call only on the UI thread, after ResolveWidgetHandles. Only atlas
// ButtonWidgets need interaction isolation. Hover-name TextBoxWidgets and the
// plain Widget hierarchy are presentation-only and never own mouse clicks.
[[nodiscard]] bool ApplyInputIsolation() noexcept {
    if (!HandlesResolved || Context == nullptr || Widgets == nullptr) return false;
    bool allSucceeded = true;
    auto disable = [&](D2RL::Widgets::WidgetHandle handle) noexcept {
        if (handle == D2RL::Widgets::InvalidHandle) return;
        if (!SetEnabled(handle, false)) allSucceeded = false;
    };

    // D2R treats enabled generic HUD widgets as UI mouse targets even when
    // their visible state is false. Keep the complete BuffHud hierarchy
    // disabled; rendering/visibility updates still work through this state.
    for (auto& slot : Handles) {
        for (const auto icon : slot.icons) disable(icon);
        disable(slot.countdown);
        disable(slot.slot);
    }
    disable(GridWidget);
    disable(HudPanel);

    if (!allSucceeded) Context->LogWarn(
        "Buff HUD: one or more presentation widgets could not be disabled; gameplay click-through may be incomplete.");
    return allSucceeded;
}

[[nodiscard]] void* ResolveNativeSlot(std::size_t slotIndex) noexcept {
    if (FindTopLevelPanel == nullptr || FindChildWidgetByName == nullptr || slotIndex >= SlotCount) {
        return nullptr;
    }
    const bool debuff = slotIndex >= BuffSlotCount;
    void* panel = FindTopLevelPanel(PanelName);
    void* grid = panel != nullptr ? FindChildWidgetByName(panel, debuff ? "DebuffGrid" : "BuffGrid") : nullptr;
    char slotName[24]{};
    if (debuff) {
        std::snprintf(slotName, sizeof(slotName), "DebuffSlot%02zu", slotIndex - BuffSlotCount);
    } else {
        std::snprintf(slotName, sizeof(slotName), "BuffSlot%02zu", slotIndex);
    }
    return grid != nullptr ? FindChildWidgetByName(grid, slotName) : nullptr;
}

[[nodiscard]] void* ResolveNativeSlotChild(std::size_t slotIndex, const char* childName) noexcept {
    if (childName == nullptr) return nullptr;
    void* slot = ResolveNativeSlot(slotIndex);
    return slot != nullptr ? FindChildWidgetByName(slot, childName) : nullptr;
}

[[nodiscard]] bool WriteCountdownText(std::size_t slotIndex, const char* text) noexcept {
    if (slotIndex >= SlotCount || text == nullptr) return false;
    const auto length = std::strlen(text);
    if (length + 1 > TimerReserveBytes) return false;

    void* widget = ResolveNativeSlotChild(slotIndex, "Countdown");
    std::uintptr_t pointer{};
    if (widget == nullptr
        || !ReadNativeField(widget, Native::Contract::HudTextPointerOffset, pointer)
        || pointer == 0) {
        return false;
    }

    auto* buffer = reinterpret_cast<char*>(pointer);
    if (!IsWritableRange(buffer, TimerReserveBytes)) {
        return false;
    }

    auto& state = RenderStates[slotIndex];
    if (state.qualifiedTimerBuffer != pointer) {
        if (!IsTimerReserve(buffer)) {
            return false;
        }
        state.qualifiedTimerBuffer = pointer;
    }

    std::memcpy(buffer, TimerReserve, TimerReserveBytes);
    std::memcpy(buffer, text, length + 1);
    if (std::memcmp(buffer, text, length + 1) != 0) {
        return false;
    }
    return true;
}

[[nodiscard]] bool QualifyTooltipString(
    std::size_t slotIndex,
    void* widget,
    SlotRenderState& state) noexcept {
    if (widget == nullptr || slotIndex >= SlotCount) return false;

    char expectedUtf8[TooltipReserveBytes]{};
    MakeTooltipReserve(slotIndex, expectedUtf8);
    std::array<std::uint16_t, TooltipReserveCodepoints + 1> expectedUtf16{};
    MakeTooltipReserveUtf16(slotIndex, expectedUtf16);
    constexpr std::size_t Utf16Bytes = (TooltipReserveCodepoints + 1) * sizeof(std::uint16_t);

    std::size_t candidateCount{};
    std::size_t candidateOffset{};
    std::uintptr_t candidateBuffer{};
    TooltipStorageEncoding candidateEncoding{TooltipStorageEncoding::Unknown};

    // Build 93847 already has one independently runtime-qualified Blizzard
    // string layout: TooltipsPanel+0x180. It is {data@+0x00, size@+0x08,
    // capacity/flags@+0x10}, not an MSVC std::string. The previous BuffHud
    // implementation searched FocusableWidget for an MSVC basic_string tuple,
    // which explains the runtime witness tooltipQualFailures>0 with
    // skillNames=ready/name=Fade. Qualify the same blz::basic_string layout
    // here. Accept both char and 16-bit character storage because UI layout
    // fields are not guaranteed to use the same character width.
    for (std::size_t offset = 0;
         offset + Native::Contract::BlizzardStringCapacityFlagsOffset + sizeof(std::uint64_t) <= TooltipNativeScanBytes;
         offset += alignof(std::uintptr_t)) {
        std::uintptr_t dataPointer{};
        std::uint64_t size{};
        std::uint64_t capacityFlags{};
        if (!ReadNativeField(widget, offset + Native::Contract::BlizzardStringDataOffset, dataPointer)
            || !ReadNativeField(widget, offset + Native::Contract::BlizzardStringSizeOffset, size)
            || !ReadNativeField(widget, offset + Native::Contract::BlizzardStringCapacityFlagsOffset, capacityFlags)) {
            continue;
        }

        const auto capacity = capacityFlags & Native::Contract::BlizzardStringCapacityMask;
        const bool embedded = (capacityFlags & Native::Contract::BlizzardStringEmbeddedFlag) != 0;
        if (embedded
            || dataPointer == 0
            || capacity > Native::Contract::MaximumTooltipCapacityBytes) {
            continue;
        }

        TooltipStorageEncoding encoding{TooltipStorageEncoding::Unknown};
        if (size == TooltipReserveLength
            && capacity >= TooltipReserveLength
            && IsWritableRange(reinterpret_cast<void*>(dataPointer), TooltipReserveBytes)
            && Internal::MatchesTooltipReserve(reinterpret_cast<const void*>(dataPointer), expectedUtf8)) {
            encoding = TooltipStorageEncoding::BlizzardUtf8;
        } else if (size == TooltipReserveCodepoints
            && capacity >= TooltipReserveCodepoints
            && IsWritableRange(reinterpret_cast<void*>(dataPointer), Utf16Bytes)
            && std::memcmp(reinterpret_cast<const void*>(dataPointer), expectedUtf16.data(), Utf16Bytes) == 0) {
            encoding = TooltipStorageEncoding::BlizzardUtf16;
        }
        if (encoding == TooltipStorageEncoding::Unknown) continue;

        ++candidateCount;
        candidateOffset = offset;
        candidateBuffer = dataPointer;
        candidateEncoding = encoding;
    }

    if (candidateCount != 1) {
        return false;
    }

    state.qualifiedTooltipWidget = reinterpret_cast<std::uintptr_t>(widget);
    state.qualifiedTooltipBuffer = candidateBuffer;
    state.qualifiedTooltipFieldOffset = candidateOffset;
    state.qualifiedTooltipEncoding = candidateEncoding;
    return true;
}

[[nodiscard]] bool EnsureTooltipQualified(std::size_t slotIndex, SlotRenderState& state) noexcept {
    if (slotIndex >= SlotCount) return false;
    void* widget = ResolveNativeSlotChild(slotIndex, "Tooltip");
    if (widget == nullptr) {
        return false;
    }

    const auto widgetAddress = reinterpret_cast<std::uintptr_t>(widget);
    if (state.qualifiedTooltipWidget == widgetAddress
        && state.qualifiedTooltipBuffer != 0
        && state.qualifiedTooltipFieldOffset != std::numeric_limits<std::size_t>::max()
        && state.qualifiedTooltipEncoding != TooltipStorageEncoding::Unknown) {
        return true;
    }

    // A different native widget means the old one was destroyed with its panel
    // (UI rebuild). Restoring the reserve into it would write freed memory.
    state.qualifiedTooltipWidget = 0;
    state.qualifiedTooltipBuffer = 0;
    state.qualifiedTooltipFieldOffset = std::numeric_limits<std::size_t>::max();
    state.qualifiedTooltipEncoding = TooltipStorageEncoding::Unknown;
    return QualifyTooltipString(slotIndex, widget, state);
}

[[nodiscard]] bool WriteTooltipText(std::size_t slotIndex, const char* text) noexcept {
    if (slotIndex >= SlotCount || text == nullptr) return false;
    const auto utf8Length = std::strlen(text);
    if (utf8Length > TooltipReserveLength) {
        return false;
    }

    auto& state = RenderStates[slotIndex];
    if (!EnsureTooltipQualified(slotIndex, state)) {
        return false;
    }

    const auto stringObject = state.qualifiedTooltipWidget + state.qualifiedTooltipFieldOffset;
    std::uintptr_t dataPointer{};
    std::uint64_t capacityFlags{};
    if (!ReadNativeField(
            reinterpret_cast<void*>(state.qualifiedTooltipWidget),
            state.qualifiedTooltipFieldOffset + Native::Contract::BlizzardStringDataOffset,
            dataPointer)
        || !ReadNativeField(
            reinterpret_cast<void*>(state.qualifiedTooltipWidget),
            state.qualifiedTooltipFieldOffset + Native::Contract::BlizzardStringCapacityFlagsOffset,
            capacityFlags)
        || dataPointer == 0
        || dataPointer != state.qualifiedTooltipBuffer) {
        return false;
    }

    const auto capacity = capacityFlags & Native::Contract::BlizzardStringCapacityMask;
    const bool embedded = (capacityFlags & Native::Contract::BlizzardStringEmbeddedFlag) != 0;
    if (embedded
        || !IsWritableRange(
            reinterpret_cast<void*>(stringObject + Native::Contract::BlizzardStringSizeOffset),
            sizeof(std::uint64_t))) {
        return false;
    }

    std::uint64_t nativeLength{};
    if (state.qualifiedTooltipEncoding == TooltipStorageEncoding::BlizzardUtf8) {
        if (capacity < TooltipReserveLength
            || !IsWritableRange(reinterpret_cast<void*>(dataPointer), TooltipReserveBytes)) {
            return false;
        }
        auto* buffer = reinterpret_cast<char*>(dataPointer);
        if (!Internal::StoreTooltipText(buffer, std::string_view(text, utf8Length))) {
            return false;
        }
        nativeLength = utf8Length;
        if (std::memcmp(buffer, text, utf8Length + 1) != 0) {
            return false;
        }
    } else if (state.qualifiedTooltipEncoding == TooltipStorageEncoding::BlizzardUtf16) {
        if (capacity < TooltipReserveCodepoints) {
            return false;
        }
        std::array<std::uint16_t, TooltipReserveCodepoints + 1> converted{};
        std::size_t convertedLength{};
        if (!Utf8ToUtf16(text, converted, convertedLength)) {
            return false;
        }
        constexpr std::size_t Utf16Bytes = (TooltipReserveCodepoints + 1) * sizeof(std::uint16_t);
        if (!IsWritableRange(reinterpret_cast<void*>(dataPointer), Utf16Bytes)) {
            return false;
        }
        std::memset(reinterpret_cast<void*>(dataPointer), 0, Utf16Bytes);
        if (convertedLength != 0) {
            std::memcpy(reinterpret_cast<void*>(dataPointer), converted.data(), convertedLength * sizeof(std::uint16_t));
        }
        nativeLength = convertedLength;
    } else {
        return false;
    }

    std::memcpy(
        reinterpret_cast<void*>(stringObject + Native::Contract::BlizzardStringSizeOffset),
        &nativeLength,
        sizeof(nativeLength));

    std::uint64_t verifyLength{};
    if (!ReadNativeField(
            reinterpret_cast<void*>(state.qualifiedTooltipWidget),
            state.qualifiedTooltipFieldOffset + Native::Contract::BlizzardStringSizeOffset,
            verifyLength)
        || verifyLength != nativeLength) {
        return false;
    }

    return true;
}

// The hover label is the layout's own HoverName TextBoxWidget, drawn by the
// game with the panel (the 1.1.0 label was drawn in D2RLoader's overlay layer,
// which players did not see over the game). Its text buffer is found by
// its reserve string, like a Countdown's, and the name is written over it.
[[nodiscard]] bool WriteHoverNameText(const char* text) noexcept {
    if (text == nullptr || NativeHoverName == nullptr) return false;
    std::uintptr_t pointer{};
    if (!ReadNativeField(NativeHoverName, Native::Contract::HudTextPointerOffset, pointer) || pointer == 0) {
        return false;
    }
    auto* buffer = reinterpret_cast<char*>(pointer);
    if (!IsWritableRange(buffer, NameReserveBytes)) return false;
    if (QualifiedNameBuffer != pointer) {
        if (std::memcmp(buffer, NameReserve, NameReserveBytes) != 0) return false;
        QualifiedNameBuffer = pointer;
    }
    // Cut at a UTF-8 character boundary so the name always fits the reserve.
    std::size_t length = std::strlen(text);
    if (length + 1 > NameReserveBytes) {
        length = NameReserveBytes - 1;
        while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0U) == 0x80U) --length;
    }
    std::memcpy(buffer, NameReserve, NameReserveBytes);
    std::memcpy(buffer, text, length);
    buffer[length] = '\0';
    return true;
}

void RestoreHoverNameBuffer() noexcept {
    if (QualifiedNameBuffer == 0) return;
    auto* buffer = reinterpret_cast<char*>(QualifiedNameBuffer);
    if (IsWritableRange(buffer, NameReserveBytes)) std::memcpy(buffer, NameReserve, NameReserveBytes);
}

void HideHoverName() noexcept {
    if (!HoverNameShown) return;
    (void)NativeShow(NativeHoverName, false);
    HoverNameShown = false;
    HoverNameText = {};
}

// Shows `text` centred above the slot box (below it at the top of the
// screen), kept on the UI screen.
void ShowHoverName(const char* text, const Internal::UiBox& slot, const UiFrame& frame) noexcept {
    NativeRect label{};
    if (NativeHoverName == nullptr || !ReadNativeRect(NativeHoverName, label)
        || label.width <= 0 || label.height <= 0) {
        HideHoverName();
        return;
    }
    if (!HoverNameShown || std::strncmp(HoverNameText.data(), text, HoverNameText.size() - 1) != 0) {
        if (!WriteHoverNameText(text)) {
            HideHoverName();
            return;
        }
        std::snprintf(HoverNameText.data(), HoverNameText.size(), "%s", text);
    }
    const double gap = frame.hd ? 8.0 : 2.0;
    double x = (slot.left + slot.right) * 0.5 - label.width * 0.5;
    double y = slot.top - gap - label.height;
    if (y < 0.0) y = slot.bottom + gap;
    x = std::clamp(x, 0.0, std::max(0.0, frame.width - label.width));
    y = std::clamp(y, 0.0, std::max(0.0, frame.height - label.height));
    float anchorX{};
    float anchorY{};
    if (!ReadNativeAnchor(NativeHoverName, anchorX, anchorY)) anchorX = anchorY = 0.0F;
    const auto localX = static_cast<std::int32_t>(std::lround(x - anchorX * frame.width));
    const auto localY = static_cast<std::int32_t>(std::lround(y - anchorY * frame.height));
    if (!NativeMove(NativeHoverName, localX, localY)) {
        HideHoverName();
        return;
    }
    if (!HoverNameShown) {
        if (!NativeShow(NativeHoverName, true)) return;
        HoverNameShown = true;
    }
}

void ClearTooltip(std::size_t slotIndex) noexcept {
    if (slotIndex >= SlotCount) return;
    auto& state = RenderStates[slotIndex];
    state.tooltipVisible = false;
    state.hoverText.fill('\0');
}

void ApplyTooltip(
    std::size_t slotIndex,
    const Core::BuffDisplayEntry& entry) noexcept {
    if (slotIndex >= SlotCount || !ResolveWidgetHandles()) return;

    auto& state = RenderStates[slotIndex];
    state.tooltipVisible = false;
    state.hoverText.fill('\0');
    // A buff-hud-display.txt name wins over the source skill's localized name.
    if (!Core::BuffPresentations().FindName(entry.stateId, state.hoverText.data(), state.hoverText.size())
        && (entry.sourceSkillId == Core::NoSourceSkillId
            || !Internal::TryResolveSkillName(
                entry.sourceSkillId,
                state.hoverText.data(),
                state.hoverText.size()))) {
        state.hoverText.fill('\0');
        return;
    }
    state.tooltipVisible = state.hoverText[0] != '\0';
}

// The game's own top-level window, also while another application has the
// focus (a windowed game behind other windows still draws its UI and reads the
// cursor). Cached until the window goes away.
[[nodiscard]] HWND GameWindow() noexcept {
    static HWND cached{};
    if (cached != nullptr && IsWindow(cached)) return cached;
    cached = nullptr;
    EnumWindows(
        [](HWND window, LPARAM) -> BOOL {
            DWORD processId{};
            GetWindowThreadProcessId(window, &processId);
            char title[64]{};
            if (processId == GetCurrentProcessId() && IsWindowVisible(window)
                && GetWindowTextA(window, title, sizeof(title)) > 0
                && std::strncmp(title, "Diablo II: Resurrected", 22) == 0) {
                cached = window;
                return FALSE;
            }
            return TRUE;
        },
        0);
    return cached;
}

// The size of the game's UI screen in layout units: the UiSpace height at the
// client's aspect (HD), else ScreenSizeX x ScreenSizeY (classic: 800x600 or
// 640x480, wider with a widescreen classic plugin). False until the game has
// set a plausible classic size.
[[nodiscard]] bool GameUiScreen(LONG clientWidth, LONG clientHeight, D2RL::Widgets::Rect& screen) noexcept {
    D2RL::Widgets::Rect space{};
    if (UiSpaceWidget != D2RL::Widgets::InvalidHandle
        && Widgets->getWidgetRect(Context, UiSpaceWidget, &space) == D2RL::Widgets::Result::Success
        && space.height > 0) {
        screen = {.x = 0, .y = 0,
                  .width = static_cast<std::int32_t>(
                      static_cast<double>(space.height) * clientWidth / clientHeight + 0.5),
                  .height = space.height};
        return true;
    }
    if (Context == nullptr || Context->exeBase == 0) return false;
    const auto width = *reinterpret_cast<const volatile std::int32_t*>(
        Context->exeBase + Native::Contract::ScreenSizeXRva);
    const auto height = *reinterpret_cast<const volatile std::int32_t*>(
        Context->exeBase + Native::Contract::ScreenSizeYRva);
    if (width < 320 || width > 16384 || height < 200 || height > 16384) return false;
    screen = {.x = 0, .y = 0, .width = width, .height = height};
    return true;
}

// Where the UI screen sits in the client area. D2RLoader returns the authored
// 0x0 rect of the fit-to-parent root: the root is then the game's UI screen,
// which D2R fits to the client height and centres (classic graphics get side
// bars).
[[nodiscard]] bool ComputeUiFrame(UiFrame& frame) noexcept {
    frame = {};
    if (!HandlesResolved || Context == nullptr || Widgets == nullptr) return false;
    HWND window = GameWindow();
    RECT client{};
    if (window == nullptr || IsIconic(window) || !GetClientRect(window, &client)) return false;
    const auto clientWidth = client.right - client.left;
    const auto clientHeight = client.bottom - client.top;
    if (clientWidth <= 0 || clientHeight <= 0) return false;
    D2RL::Widgets::Rect root{};
    if (Widgets->getWidgetRect(Context, HudPanel, &root) != D2RL::Widgets::Result::Success) return false;
    const bool rootFromScreen = root.width <= 0 || root.height <= 0;
    if (rootFromScreen && !GameUiScreen(clientWidth, clientHeight, root)) return false;
    frame.width = root.width;
    frame.height = root.height;
    frame.hd = IsHdLayout();
    if (rootFromScreen) {
        frame.scaleX = frame.scaleY = static_cast<double>(clientHeight) / frame.height;
        frame.left = (static_cast<double>(clientWidth) - frame.width * frame.scaleX) * 0.5;
    } else {
        frame.scaleX = static_cast<double>(clientWidth) / frame.width;
        frame.scaleY = static_cast<double>(clientHeight) / frame.height;
        frame.left = 0.0;
    }
    frame.valid = frame.width > 0.0 && frame.height > 0.0 && frame.scaleX > 0.0 && frame.scaleY > 0.0;
    return frame.valid;
}

// The grid's box on the UI screen: anchor * screen + local x / y.
[[nodiscard]] bool GridBox(std::size_t group, const UiFrame& frame, GroupHit& hit) noexcept {
    hit = {};
    NativeRect rect{};
    float anchorX{};
    float anchorY{};
    void* grid = NativeGrids[group];
    if (!frame.valid || !ReadNativeRect(grid, rect) || !ReadNativeAnchor(grid, anchorX, anchorY)
        || rect.width <= 0 || rect.height <= 0) {
        return false;
    }
    hit.left = anchorX * frame.width + rect.x;
    hit.top = anchorY * frame.height + rect.y;
    hit.width = rect.width;
    hit.height = rect.height;
    hit.valid = true;
    return true;
}

// Puts each grid at its saved spot for this graphics mode (a fraction of the
// UI screen for its top-left corner), else at its JSON spot, always wholly on
// the screen. With `clampSaved` the clamped spot is written back (a drag ends).
void ApplyGroupPositions(const UiFrame& frame, bool clampSaved) noexcept {
    if (!frame.valid || NativeSetXFn == nullptr || NativeSetYFn == nullptr) return;
    const std::size_t mode = frame.hd ? 1 : 0;
    for (std::size_t group = 0; group < GroupCount; ++group) {
        void* grid = NativeGrids[group];
        NativeRect rect{};
        float anchorX{};
        float anchorY{};
        if (grid == nullptr || !ReadNativeRect(grid, rect) || !ReadNativeAnchor(grid, anchorX, anchorY)) continue;
        Core::PanelPosition saved{};
        GroupDefault fallback{};
        {
            std::lock_guard lock(PositionMutex);
            saved = SavedPositions[mode][group];
            fallback = GroupDefaults[mode][group];
        }
        if (!fallback.known) continue;
        const double baseX = anchorX * frame.width;
        const double baseY = anchorY * frame.height;
        double left = saved.IsSet() ? saved.x * frame.width : baseX + fallback.x;
        double top = saved.IsSet() ? saved.y * frame.height : baseY + fallback.y;
        left = std::clamp(left, 0.0, std::max(0.0, frame.width - rect.width));
        top = std::clamp(top, 0.0, std::max(0.0, frame.height - rect.height));
        if (clampSaved && saved.IsSet()) {
            std::lock_guard lock(PositionMutex);
            SavedPositions[mode][group] = {
                .x = static_cast<float>(left / frame.width),
                .y = static_cast<float>(top / frame.height)};
        }
        (void)NativeMove(grid,
            static_cast<std::int32_t>(std::lround(left - baseX)),
            static_cast<std::int32_t>(std::lround(top - baseY)));
    }
}

// The boxes of the occupied slots of both groups, on the UI screen.
[[nodiscard]] HitSnapshot CollectHits(const UiFrame& frame) noexcept {
    HitSnapshot hits{};
    hits.frame = frame;
    if (!frame.valid) return hits;
    for (std::size_t group = 0; group < GroupCount; ++group) {
        auto& hit = hits.groups[group];
        if (!GridBox(group, frame, hit)) continue;
        const std::size_t first = group == BuffGroup ? 0 : BuffSlotCount;
        const std::size_t last = group == BuffGroup ? BuffSlotCount : SlotCount;
        for (std::size_t i = first; i < last && hit.count < hit.slots.size(); ++i) {
            NativeRect slot{};
            if (!RenderStates[i].visible || !ReadNativeRect(NativeSlots[i].slot, slot)) continue;
            hit.slots[hit.count] = {hit.left + slot.x, hit.top + slot.y,
                                    hit.left + slot.x + slot.width, hit.top + slot.y + slot.height};
            hit.slotIndex[hit.count] = i;
            ++hit.count;
        }
    }
    hits.valid = true;
    return hits;
}

// The cursor on the UI screen; false when it is outside the game window.
[[nodiscard]] bool CursorOnUi(const UiFrame& frame, double& x, double& y) noexcept {
    HWND window = GameWindow();
    POINT point{};
    if (window == nullptr || !GetCursorPos(&point) || !ScreenToClient(window, &point)) return false;
    x = (static_cast<double>(point.x) - frame.left) / frame.scaleX;
    y = static_cast<double>(point.y) / frame.scaleY;
    return true;
}

[[nodiscard]] bool Contains(const Internal::UiBox& box, double x, double y) noexcept {
    return x >= box.left && x < box.right && y >= box.top && y < box.bottom;
}

// Shows the hovered slot's name (buff or debuff) in the hover label.
void UpdateHoverNamePresentation(const HitSnapshot& hits) noexcept {
    double x{};
    double y{};
    bool dragging{};
    {
        std::lock_guard lock(PositionMutex);
        dragging = Drag.active;
    }
    if (!hits.valid || dragging || !CursorOnUi(hits.frame, x, y)) {
        HideHoverName();
        return;
    }
    if (!HoverGeometryLogged && Context != nullptr) {
        const auto& buffs = hits.groups[BuffGroup];
        const auto& debuffs = hits.groups[DebuffGroup];
        char line[384]{};
        std::snprintf(line, sizeof(line),
            "BUFF_HOVER_GEOMETRY mode=%s screen=%.0fx%.0f scale=%.3f left=%.1f buffs=(%.1f,%.1f %.0fx%.0f) debuffs=(%.1f,%.1f %.0fx%.0f)",
            hits.frame.hd ? "hd" : "legacy", hits.frame.width, hits.frame.height, hits.frame.scaleY,
            hits.frame.left, buffs.left, buffs.top, buffs.width, buffs.height,
            debuffs.left, debuffs.top, debuffs.width, debuffs.height);
        Context->LogInfo(line);
        HoverGeometryLogged = true;
    }
    for (const auto& group : hits.groups) {
        for (std::size_t k = 0; k < group.count; ++k) {
            const auto& state = RenderStates[group.slotIndex[k]];
            if (!state.tooltipVisible || state.hoverText[0] == '\0' || !Contains(group.slots[k], x, y)) continue;
            ShowHoverName(state.hoverText.data(), group.slots[k], hits.frame);
            return;
        }
    }
    HideHoverName();
}

[[nodiscard]] bool ResolveEntryIcon(
    const Core::BuffDisplayEntry& entry,
    SkillIconDescriptor& descriptor) noexcept {
    descriptor = {};
    if (entry.iconFrame != Core::AutoIconFrame) {
        descriptor.frame = entry.iconFrame;
        descriptor.atlas = entry.iconAtlas == Core::BuffIconAtlas::Auto
            ? Core::BuffIconAtlas::Global
            : entry.iconAtlas;
        return true;
    }
    if (entry.sourceSkillId != Core::NoSourceSkillId
        && Internal::TryResolveSkillIcon(entry.sourceSkillId, descriptor)) {
        return true;
    }
    descriptor = {.atlas = Core::BuffIconAtlas::Global, .frame = 0};
    return false;
}

[[nodiscard]] bool IsFrameInFuture(std::uint32_t expire, std::uint32_t now) noexcept {
    return static_cast<std::int32_t>(expire - now) > 0;
}

[[nodiscard]] std::uint32_t RemainingSeconds(std::uint32_t expire, std::uint32_t now) noexcept {
    const auto delta = static_cast<std::uint32_t>(expire - now);
    return (delta + FramesPerSecond - 1) / FramesPerSecond;
}

void FormatSeconds(std::uint32_t seconds, char (&output)[16]) noexcept {
    if (seconds >= 3600) {
        const auto hours = seconds / 3600;
        const auto minutes = (seconds % 3600) / 60;
        std::snprintf(output, sizeof(output), "%u:%02u", hours, minutes);
    } else if (seconds >= 60) {
        std::snprintf(output, sizeof(output), "%u:%02u", seconds / 60, seconds % 60);
    } else {
        std::snprintf(output, sizeof(output), "%u", seconds);
    }
}

[[nodiscard]] bool IsDebuffSlot(std::size_t slotIndex) noexcept {
    return slotIndex >= BuffSlotCount && slotIndex < SlotCount;
}

// A buff slot goes through its WidgetService handles, a debuff slot through
// its native widgets.
void ShowSlotWidget(std::size_t slotIndex, bool visible) noexcept {
    if (IsDebuffSlot(slotIndex)) {
        (void)NativeShow(NativeSlots[slotIndex].slot, visible);
    } else if (slotIndex < BuffSlotCount) {
        (void)SetVisible(Handles[slotIndex].slot, visible);
    }
}

void ShowCountdown(std::size_t slotIndex, bool visible) noexcept {
    if (IsDebuffSlot(slotIndex)) {
        (void)NativeShow(NativeSlots[slotIndex].countdown, visible);
    } else if (slotIndex < BuffSlotCount) {
        (void)SetVisible(Handles[slotIndex].countdown, visible);
    }
}

// Keeps every atlas button disabled (click-through) and shows only `selected`
// (none with AtlasCount).
void SelectAtlas(std::size_t slotIndex, std::size_t selected) noexcept {
    for (std::size_t i = 0; i < AtlasCount; ++i) {
        if (IsDebuffSlot(slotIndex)) {
            void* icon = NativeSlots[slotIndex].icons[i];
            (void)SetNativeEnabled(icon, false);
            (void)NativeShow(icon, i == selected);
        } else {
            (void)SetEnabled(Handles[slotIndex].icons[i], false);
            (void)SetVisible(Handles[slotIndex].icons[i], i == selected);
        }
    }
}

[[nodiscard]] bool ApplyIcon(std::size_t slotIndex, const SkillIconDescriptor& icon) noexcept {
    if (slotIndex >= SlotCount || !ResolveWidgetHandles()) return false;
    const auto selected = AtlasIndex(icon.atlas);
    // BuffHud is display-only. Even when a buff is active, its atlas button
    // must stay click-through, so keep every ButtonWidget disabled and only
    // use visibility to choose the currently rendered atlas.
    SelectAtlas(slotIndex, selected);

    void* nativeIcon = NativeSlots[slotIndex].icons[selected];
    const auto declaredFrame = static_cast<std::uint16_t>(slotIndex * 2);
    if (Internal::TryApplyIconFrame(nativeIcon, icon.frame, declaredFrame)) return true;

    // Fail closed if the qualified live-frame path cannot be applied. The atlas
    // selection remains valid, but the slot keeps its JSON witness frame and the
    // fallback counter makes the failure visible in `buff-panel status`.
    return false;
}

void HideItemIcon(std::size_t slotIndex) noexcept {
    if (slotIndex >= BuffSlotCount) return;
    auto& item = Handles[slotIndex].item;
    if (item == nullptr) return;
    (void)SetItemPictureVisible(item, false);
    item = nullptr;
}

// Item pictures are optional hidden layout containers named Item_<code> (buff
// slots only). A code without a container in this slot keeps the atlas icon.
[[nodiscard]] bool ApplyItemIcon(std::size_t slotIndex, const char* itemCode) noexcept {
    HideItemIcon(slotIndex);
    if (slotIndex >= BuffSlotCount || itemCode == nullptr || itemCode[0] == '\0' || !VisibleFlagQualified) {
        return false;
    }
    auto& handles = Handles[slotIndex];
    void* item = FindItemPicture(ResolveNativeSlot(slotIndex), itemCode);
    if (item == nullptr) return false;
    SelectAtlas(slotIndex, AtlasCount);
    if (!SetItemPictureVisible(item, true)) return false;
    handles.item = item;
    return true;
}

void HideSlot(std::size_t slotIndex) noexcept {
    if (slotIndex >= SlotCount || !ResolveWidgetHandles()) return;
    auto& state = RenderStates[slotIndex];
    if (!state.visible) return;

    // Visibility alone does not remove ButtonWidget hit targets. Once an
    // occupied slot becomes empty, disable every atlas child before hiding it.
    // Initial empty slots were already disabled by ResolveWidgetHandles().
    for (std::size_t i = 0; i < AtlasCount; ++i) {
        if (IsDebuffSlot(slotIndex)) {
            (void)SetNativeEnabled(NativeSlots[slotIndex].icons[i], false);
        } else {
            (void)SetEnabled(Handles[slotIndex].icons[i], false);
        }
    }
    ClearTooltip(slotIndex);
    state.tooltipVisible = false;

    ShowSlotWidget(slotIndex, false);
    const auto qualifiedTimerBuffer = state.qualifiedTimerBuffer;
    const auto qualifiedTooltipWidget = state.qualifiedTooltipWidget;
    const auto qualifiedTooltipBuffer = state.qualifiedTooltipBuffer;
    const auto qualifiedTooltipFieldOffset = state.qualifiedTooltipFieldOffset;
    const auto qualifiedTooltipEncoding = state.qualifiedTooltipEncoding;
    state = {};
    state.qualifiedTimerBuffer = qualifiedTimerBuffer;
    state.qualifiedTooltipWidget = qualifiedTooltipWidget;
    state.qualifiedTooltipBuffer = qualifiedTooltipBuffer;
    state.qualifiedTooltipFieldOffset = qualifiedTooltipFieldOffset;
    state.qualifiedTooltipEncoding = qualifiedTooltipEncoding;
}

void RenderSlot(
    std::size_t slotIndex,
    const Core::BuffDisplayEntry& entry,
    std::uint32_t nowFrame,
    bool hasClock) noexcept {
    if (slotIndex >= SlotCount || !ResolveWidgetHandles()) return;

    SkillIconDescriptor icon{};
    const bool iconResolved = ResolveEntryIcon(entry, icon);
    (void)iconResolved;
    char itemCode[ItemCodeBytes]{};
    (void)Core::BuffPresentations().FindItemCode(entry.stateId, itemCode, sizeof(itemCode));
    auto& state = RenderStates[slotIndex];
    const bool assignmentChanged = !state.visible
        || state.key != entry.key
        || state.atlas != icon.atlas
        || state.requestedFrame != icon.frame
        || std::strcmp(state.itemCode, itemCode) != 0
        || state.displayMode != entry.displayMode
        || state.sourceSkillId != entry.sourceSkillId;

    if (assignmentChanged) {
        ShowSlotWidget(slotIndex, false);
        const bool iconApplied = ApplyItemIcon(slotIndex, itemCode) || ApplyIcon(slotIndex, icon);
        std::memcpy(state.itemCode, itemCode, sizeof(state.itemCode));
        state.key = entry.key;
        state.atlas = icon.atlas;
        state.requestedFrame = icon.frame;
        state.renderedFrame = iconApplied
            ? icon.frame
            : static_cast<std::uint16_t>(slotIndex * 2);
        state.displayMode = entry.displayMode;
        state.sourceSkillId = entry.sourceSkillId;
        state.seconds = NoRenderedSeconds;
        state.currentValue = NoRenderedValue;
        state.maximumValue = NoRenderedValue;
        state.timerVisible = false;
        state.tooltipVisible = false;
        ApplyTooltip(slotIndex, entry);
        state.visible = true;
        ShowSlotWidget(slotIndex, true);
    }

    if (entry.displayMode == Core::BuffDisplayMode::Resource) {
        if (state.timerVisible && state.currentValue == entry.currentValue) {
            return;
        }

        char text[32]{};
        std::snprintf(text, sizeof(text), "%d", entry.currentValue);
        if (WriteCountdownText(slotIndex, text)) {
            ShowCountdown(slotIndex, true);
            state.timerVisible = true;
            state.seconds = NoRenderedSeconds;
            state.currentValue = entry.currentValue;
            state.maximumValue = entry.maximumValue;
        }
        return;
    }

    const bool wantsTimer = entry.expireGameFrame != 0;
    if (!wantsTimer) {
        if (state.timerVisible) {
            ShowCountdown(slotIndex, false);
            state.timerVisible = false;
            state.seconds = NoRenderedSeconds;
            state.currentValue = NoRenderedValue;
            state.maximumValue = NoRenderedValue;
        }
        return;
    }

    if (!hasClock) {
        if (state.seconds != NoRenderedSeconds || !state.timerVisible) {
            if (WriteCountdownText(slotIndex, "--")) {
                ShowCountdown(slotIndex, true);
                state.timerVisible = true;
                state.seconds = NoRenderedSeconds;
            }
        }
        return;
    }

    const auto seconds = RemainingSeconds(entry.expireGameFrame, nowFrame);
    if (state.timerVisible && state.seconds == seconds) return;

    char text[16]{};
    FormatSeconds(seconds, text);
    if (WriteCountdownText(slotIndex, text)) {
        ShowCountdown(slotIndex, true);
        state.timerVisible = true;
        state.seconds = seconds;
    }
}

[[nodiscard]] bool EffectiveFrame(
    const Core::BuffDisplaySnapshot& snapshot,
    std::uint32_t& frame) noexcept {
    if (!snapshot.hasGameFrame) { frame = 0; return false; }
    frame = snapshot.currentGameFrame;
    return true;
}

void HideAllSlots() noexcept {
    for (std::size_t i = 0; i < SlotCount; ++i) {
        ShowSlotWidget(i, false);
        ShowCountdown(i, false);
        ClearTooltip(i);
    }
    HideHoverName();
}

void OpenPanel() noexcept;

// Returns false while the panel is missing. A rebuilt panel starts from its
// JSON defaults, so drop every handle, render state and qualified buffer of the
// old one without touching its memory, then resolve and hide the new slots.
[[nodiscard]] bool EnsureLivePanel() noexcept {
    if (FindTopLevelPanel == nullptr) return false;
    void* live = FindTopLevelPanel(PanelName);
    if (HandlesResolved && live == ResolvedNativePanel) return true;

    if (HandlesResolved && Context != nullptr) {
        Context->LogInfo(live != nullptr
            ? "Buff HUD: BuffHud panel was rebuilt by the game UI; re-resolving its widgets."
            : "Buff HUD: BuffHud panel was removed by the game UI; waiting for it to return.");
    }
    // A rebuilt panel (HD <-> legacy) has its own geometry: log it once again.
    HoverGeometryLogged = false;
    InvalidateWidgetHandles();
    if (live == nullptr) {
        if (++PollsWithoutPanel >= ReopenPollInterval) {
            PollsWithoutPanel = 0;
            OpenPanel();
        }
        return false;
    }
    PollsWithoutPanel = 0;
    if (!ResolveWidgetHandles()) return false;
    HideAllSlots();
    return true;
}

// Reads the visibility bit QualifyVisibleFlag learned on a native widget. false
// when the bit is unknown or the widget's memory is not readable.
[[nodiscard]] bool IsNativeShown(const void* widget) noexcept {
    if (!VisibleFlagQualified || widget == nullptr) return false;
    const auto* flag = static_cast<const std::uint8_t*>(widget) + VisibleFlagOffset;
    if (!IsReadableRange(flag, 1)) return false;
    return ((*flag & VisibleFlagMask) != 0) == VisibleFlagSetWhenShown;
}

[[nodiscard]] bool IsNativePanelOpen(const char* name) noexcept {
    return IsNativeShown(FindTopLevelPanel(name));
}

// True when an occupied slot of either group overlaps the side panel.
[[nodiscard]] bool OccupiedSlotOverlaps(Internal::PanelSide side, const HitSnapshot& hits) noexcept {
    if (!hits.valid) return false;
    const auto panel = Internal::SidePanelBox(side, hits.frame.width, hits.frame.height, hits.frame.hd);
    for (const auto& group : hits.groups) {
        for (std::size_t k = 0; k < group.count; ++k) {
            if (Internal::Overlaps(group.slots[k], panel)) return true;
        }
    }
    return false;
}

// The open native panel the icons hide for, or null: a HidingPanels entry, or
// a SidePanels entry an occupied slot overlaps. A panel counts as open when it
// exists and its own visibility bit says shown.
[[nodiscard]] const char* OpenHidingPanel(const HitSnapshot& hits) noexcept {
    // HideItemPictures qualifies the bit on the first render.
    if (FindTopLevelPanel == nullptr || !VisibleFlagQualified || Widgets == nullptr) return nullptr;
    for (const auto* name : HidingPanels) {
        if (IsNativePanelOpen(name)) return name;
    }
    for (const auto& panel : SidePanels) {
        if (IsNativePanelOpen(panel.name) && OccupiedSlotOverlaps(panel.side, hits)) return panel.name;
    }
    return nullptr;
}

// Shows or hides the whole BuffHud panel; calls the WidgetService only when the
// wanted state changes or the panel was rebuilt.
void ApplyHudShown(bool shown) noexcept {
    if (HudShownKnown && HudShownApplied == shown) return;
    if (!SetVisible(HudPanel, shown)) return;
    HudShownKnown = true;
    HudShownApplied = shown;
}

void FormatPosition(const Core::PanelPosition& position, char (&x)[16], char (&y)[16]) noexcept {
    // Plain digits whatever the C locale: 4 decimals of a fraction, or -1.
    auto format = [](float value, char (&out)[16]) noexcept {
        if (value < 0.0F) {
            std::snprintf(out, sizeof(out), "-1");
            return;
        }
        const auto scaled = static_cast<long>(std::lround(std::min(value, 1.0F) * 10000.0F));
        std::snprintf(out, sizeof(out), "%ld.%04ld", scaled / 10000, scaled % 10000);
    };
    format(position.x, x);
    format(position.y, y);
}

// Writes the saved positions (and reset_* = false) into buff-panel.toml,
// keeping its other lines and comments.
void SavePositions(bool clearResets) noexcept {
    if (Context == nullptr) return;
    std::array<std::array<Core::PanelPosition, GroupCount>, ModeCount> positions{};
    {
        std::lock_guard lock(PositionMutex);
        positions = SavedPositions;
    }
    std::string text;
    {
        std::array<char, 8192> buffer{};
        std::uint32_t required{};
        if (Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required)) {
            text = buffer.data();
        }
    }
    for (std::size_t mode = 0; mode < ModeCount; ++mode) {
        for (std::size_t group = 0; group < GroupCount; ++group) {
            char x[16]{};
            char y[16]{};
            FormatPosition(positions[mode][group], x, y);
            const std::string prefix = std::string(ModeNames[mode]) + "_" + GroupNames[group];
            text = Core::SetConfigValue(text, prefix + "_x", x);
            text = Core::SetConfigValue(text, prefix + "_y", y);
        }
    }
    if (clearResets) {
        text = Core::SetConfigValue(text, "reset_buffs", "false");
        text = Core::SetConfigValue(text, "reset_debuffs", "false");
    }
    const bool written = Context->WriteConfig(text.c_str());
    char line[320]{};
    char bx[16]{}, by[16]{}, dx[16]{}, dy[16]{}, lbx[16]{}, lby[16]{}, ldx[16]{}, ldy[16]{};
    FormatPosition(positions[1][BuffGroup], bx, by);
    FormatPosition(positions[1][DebuffGroup], dx, dy);
    FormatPosition(positions[0][BuffGroup], lbx, lby);
    FormatPosition(positions[0][DebuffGroup], ldx, ldy);
    std::snprintf(line, sizeof(line),
        "Buff HUD: %s buff-panel.toml positions hd buffs=(%s,%s) debuffs=(%s,%s) legacy buffs=(%s,%s) debuffs=(%s,%s).",
        written ? "saved" : "FAILED to save", bx, by, dx, dy, lbx, lby, ldx, ldy);
    written ? Context->LogInfo(line) : Context->LogWarn(line);
}

void InstallWindowHook() noexcept;

void RenderSnapshot() noexcept {
    if (!EnsureLivePanel()) return;
    InstallWindowHook();
    // A table reload can name item codes the last pass did not hide.
    if (Core::BuffPresentations().Generation() != HiddenItemGeneration) HideItemPictures();

    bool saveNow{};
    {
        std::lock_guard lock(PositionMutex);
        saveNow = SaveRequested;
        SaveRequested = false;
    }
    UiFrame frame{};
    if (ComputeUiFrame(frame)) ApplyGroupPositions(frame, saveNow);
    if (saveNow) SavePositions(false);

    auto snapshot = Core::BuffDisplays().Snapshot();
    std::uint32_t nowFrame{};
    const bool hasClock = EffectiveFrame(snapshot, nowFrame);

    // The display bus is not gameplay state. Once authoritative time says an
    // entry is over, removing it from the display registry is safe and keeps
    // stale producers from occupying HUD slots forever.
    if (hasClock) {
        std::array<std::uint64_t, Core::MaximumPublishedBuffs> expired{};
        std::size_t expiredCount{};
        for (std::size_t i = 0; i < snapshot.count; ++i) {
            const auto& entry = snapshot.entries[i];
            if (entry.expireGameFrame != 0
                && !IsFrameInFuture(entry.expireGameFrame, nowFrame)) {
                expired[expiredCount++] = entry.key;
            }
        }
        if (expiredCount != 0) {
            for (std::size_t i = 0; i < expiredCount; ++i) (void)Core::BuffDisplays().Remove(expired[i]);
            snapshot = Core::BuffDisplays().Snapshot();
        }
    }

    std::array<Core::BuffDisplayEntry, Core::MaximumPublishedBuffs> ordered{};
    std::size_t orderedCount{};
    for (std::size_t i = 0; i < snapshot.count; ++i) ordered[orderedCount++] = snapshot.entries[i];
    std::sort(
        ordered.begin(),
        ordered.begin() + static_cast<std::ptrdiff_t>(orderedCount),
        [](const Core::BuffDisplayEntry& lhs, const Core::BuffDisplayEntry& rhs) noexcept {
            if (lhs.priority != rhs.priority) return lhs.priority > rhs.priority;
            return lhs.sequence < rhs.sequence;
        });

    // Buffs fill the buff group from its first slot, debuffs the debuff group.
    std::size_t buffs = 0;
    std::size_t debuffs = 0;
    for (std::size_t i = 0; i < orderedCount; ++i) {
        if (ordered[i].debuff) {
            if (DebuffsReady && debuffs < DebuffSlotCount) {
                RenderSlot(BuffSlotCount + debuffs++, ordered[i], nowFrame, hasClock);
            }
        } else if (buffs < BuffSlotCount) {
            RenderSlot(buffs++, ordered[i], nowFrame, hasClock);
        }
    }
    for (std::size_t i = buffs; i < BuffSlotCount; ++i) HideSlot(i);
    for (std::size_t i = BuffSlotCount + debuffs; i < SlotCount; ++i) {
        if (DebuffsReady) HideSlot(i);
    }

    auto hits = CollectHits(frame);
    const char* hiding = HideWithPanels ? OpenHidingPanel(hits) : nullptr;
    if (hiding != HidingPanel && Context != nullptr) {
        char line[160]{};
        std::snprintf(line, sizeof(line), hiding != nullptr
            ? "Buff HUD: %s is open over the icons; icons hidden."
            : "Buff HUD: %s closed or clear of the icons; icons shown again.",
            hiding != nullptr ? hiding : HidingPanel);
        Context->LogInfo(line);
    }
    HidingPanel = hiding;
    ApplyHudShown(hiding == nullptr);
    if (hiding != nullptr) hits.valid = false;
    UpdateHoverNamePresentation(hits);
    {
        std::lock_guard lock(PositionMutex);
        Hits = hits;
    }
    LastRenderedRevision = snapshot.revision;
}

// Moving a group: Left Ctrl + left button down on one of its icons grabs it,
// the mouse moves it, releasing the button drops it and saves its spot. These
// clicks never reach the game. Runs on the game window's thread.
[[nodiscard]] bool BeginDrag(LPARAM lParam) noexcept {
    const double x = static_cast<short>(LOWORD(lParam));
    const double y = static_cast<short>(HIWORD(lParam));
    std::lock_guard lock(PositionMutex);
    if (!Hits.valid || !Hits.frame.valid || Drag.active) return false;
    const double ux = (x - Hits.frame.left) / Hits.frame.scaleX;
    const double uy = y / Hits.frame.scaleY;
    for (std::size_t group = 0; group < GroupCount; ++group) {
        const auto& hit = Hits.groups[group];
        for (std::size_t k = 0; k < hit.count; ++k) {
            if (!Contains(hit.slots[k], ux, uy)) continue;
            Drag = {.active = true, .group = group, .mode = Hits.frame.hd ? 1U : 0U,
                    .grabX = ux - hit.left, .grabY = uy - hit.top};
            return true;
        }
    }
    return false;
}

void UpdateDrag(LPARAM lParam) noexcept {
    const double x = static_cast<short>(LOWORD(lParam));
    const double y = static_cast<short>(HIWORD(lParam));
    std::lock_guard lock(PositionMutex);
    if (!Drag.active || !Hits.frame.valid) return;
    const auto& frame = Hits.frame;
    const double left = (x - frame.left) / frame.scaleX - Drag.grabX;
    const double top = y / frame.scaleY - Drag.grabY;
    SavedPositions[Drag.mode][Drag.group] = {
        .x = static_cast<float>(std::clamp(left / frame.width, 0.0, 1.0)),
        .y = static_cast<float>(std::clamp(top / frame.height, 0.0, 1.0))};
}

[[nodiscard]] bool EndDrag() noexcept {
    std::lock_guard lock(PositionMutex);
    if (!Drag.active) return false;
    Drag.active = false;
    SaveRequested = true;
    DragsCompleted.fetch_add(1, std::memory_order_relaxed);
    return true;
}

[[nodiscard]] bool IsDragging() noexcept {
    std::lock_guard lock(PositionMutex);
    return Drag.active;
}

LRESULT CALLBACK BuffWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        // Left Ctrl: the click carries MK_CONTROL and the left Ctrl key is
        // down (Right Ctrl alone leaves the click to the game).
        if ((wParam & MK_CONTROL) != 0 && (GetKeyState(VK_LCONTROL) & 0x8000) != 0 && BeginDrag(lParam)) return 0;
        break;
    case WM_MOUSEMOVE:
        if (IsDragging()) {
            if ((wParam & MK_LBUTTON) == 0) {
                (void)EndDrag();
            } else {
                UpdateDrag(lParam);
                wParam &= ~static_cast<WPARAM>(MK_LBUTTON);
            }
        }
        break;
    case WM_LBUTTONUP:
        if (EndDrag()) return 0;
        break;
    case WM_KILLFOCUS:
    case WM_CAPTURECHANGED:
        (void)EndDrag();
        break;
    case WM_ACTIVATEAPP:
        if (wParam == FALSE) (void)EndDrag();
        break;
    default:
        break;
    }
    const WNDPROC previous = PreviousWndProc;
    return previous != nullptr
        ? CallWindowProcW(previous, window, message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);
}

void RemoveWindowHook() noexcept {
    if (SubclassedWindow == nullptr) return;
    if (IsWindow(SubclassedWindow)) {
        const auto current = GetWindowLongPtrW(SubclassedWindow, GWLP_WNDPROC);
        if (current == reinterpret_cast<LONG_PTR>(&BuffWindowProc)) {
            (void)SetWindowLongPtrW(SubclassedWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(PreviousWndProc));
        } else if (Context != nullptr) {
            // Someone subclassed the window after us: our procedure stays in
            // their chain and keeps forwarding.
            Context->LogWarn("Buff HUD: game window procedure changed after ours; left in place.");
            SubclassedWindow = nullptr;
            return;
        }
    }
    SubclassedWindow = nullptr;
    PreviousWndProc = nullptr;
}

// Subclasses the game window once it exists (again after it is recreated).
void InstallWindowHook() noexcept {
    if (NativeSetXFn == nullptr || NativeSetYFn == nullptr) return;
    HWND window = GameWindow();
    if (window == nullptr || window == SubclassedWindow) return;
    RemoveWindowHook();
    PreviousWndProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC));
    SetLastError(0);
    const auto previous = SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&BuffWindowProc));
    if (previous == 0 && GetLastError() != 0) {
        PreviousWndProc = nullptr;
        if (Context != nullptr) Context->LogWarn("Buff HUD: cannot subclass the game window; Ctrl+drag is off.");
        return;
    }
    PreviousWndProc = reinterpret_cast<WNDPROC>(previous);
    SubclassedWindow = window;
    if (Context != nullptr) Context->LogInfo("Buff HUD: Left Ctrl + drag on the icons moves the buff and debuff groups.");
}

void QueuePoll() noexcept;

void __cdecl PollOnUiThread(const D2RL::PluginContext* context, void*) noexcept {
    PollScheduled.store(false, std::memory_order_release);
    if (context == nullptr || context != Context
        || CurrentSessionGeneration.load(std::memory_order_acquire) == 0) {
        return;
    }

    RenderSnapshot();
    QueuePoll();
}

void QueuePoll() noexcept {
    if (Context == nullptr || Threads == nullptr
        || CurrentSessionGeneration.load(std::memory_order_acquire) == 0) {
        return;
    }
    bool expected = false;
    if (!PollScheduled.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return;
    }
    if (Threads->runOnUiThread(Context, &PollOnUiThread, nullptr)
        != D2RL::Threads::Result::Success) {
        PollScheduled.store(false, std::memory_order_release);
    }
}

void OpenPanel() noexcept {
    if (Context == nullptr || Panels == nullptr
        || RegisteredPanel == D2RL::Panels::InvalidHandle) return;
    const auto result = Panels->openPanel(Context, RegisteredPanel);
    if (result != D2RL::Panels::Result::Success) {
        return;
    }
    InvalidateWidgetHandles();
    if (ResolveWidgetHandles()) HideAllSlots();
}

void ClosePanel() noexcept {
    if (IsResolvedPanelLive()) {
        HideHoverName();
        RestoreQualifiedCountdownBuffers();
        RestoreQualifiedTooltipBuffers();
        RestoreHoverNameBuffer();
    }
    if (Context != nullptr && Panels != nullptr
        && RegisteredPanel != D2RL::Panels::InvalidHandle) {
        (void)Panels->closePanel(Context, RegisteredPanel);
    }
    InvalidateWidgetHandles();
}

void __cdecl RebuildIconsOnGameThread(const D2RL::PluginContext*, void*) noexcept {
    const auto previous = Internal::SkillIconStatus();
    const auto revision = previous.tableRevision == 0 ? 1 : previous.tableRevision;
    const bool ready = Internal::RebuildSkillIconCache(revision);
    if (Context != nullptr) {
        const auto status = Internal::SkillIconStatus();
        char line[384]{};
        std::snprintf(
            line,
            sizeof(line),
            "Buff HUD: manual skill presentation cache rebuild %s bank=%u skills=%u desc=%u rowSize=%u/%u candidates(link=%u class=%u name=%u) offsets(link=0x%X class=0x%X icon=0x%X nameId=0x%X) names=%s.",
            ready ? "succeeded" : "FAILED",
            status.bank,
            status.skillCount,
            status.skillDescCount,
            status.skillsRowSize,
            status.skillDescRowSize,
            status.linkCandidateCount,
            status.classCandidateCount,
            status.nameCandidateCount,
            status.skillDescLinkOffset,
            status.skillClassOffset,
            status.iconCelOffset,
            status.skillNameStringIdOffset,
            status.namesReady ? "ready" : "not-qualified");
        ready ? Context->LogInfo(line) : Context->LogWarn(line);
    }
}

void __cdecl OnDataTablesLoaded(
    const D2RL::PluginContext*,
    const D2RL::Lifecycle::DataTablesLoadedEvent* event,
    void*) noexcept {
    if (event == nullptr) return;
    const bool ready = Internal::RebuildSkillIconCache(event->revision);
    if (Context == nullptr) return;
    const auto status = Internal::SkillIconStatus();
    char line[384]{};
    std::snprintf(
        line,
        sizeof(line),
        "Buff HUD: skill presentation table resolver %s revision=%llu bank=%u skills=%u desc=%u rowSize=%u/%u candidates(link=%u class=%u name=%u) offsets(link=0x%X class=0x%X icon=0x%X nameId=0x%X) names=%s.",
        ready ? "ready" : "not-qualified",
        static_cast<unsigned long long>(event->revision),
        status.bank,
        status.skillCount,
        status.skillDescCount,
        status.skillsRowSize,
        status.skillDescRowSize,
        status.linkCandidateCount,
        status.classCandidateCount,
        status.nameCandidateCount,
        status.skillDescLinkOffset,
        status.skillClassOffset,
        status.iconCelOffset,
        status.skillNameStringIdOffset,
        status.namesReady ? "ready" : "not-qualified");
    ready ? Context->LogInfo(line) : Context->LogWarn(line);
}

void __cdecl OnGameplayEvent(
    const D2RL::PluginContext*,
    const D2RL::Lifecycle::GameplayEvent* event,
    void*) noexcept {
    if (event == nullptr) return;

    switch (event->kind) {
    case D2RL::Lifecycle::GameplayEventKind::GameJoined:
        CurrentSessionGeneration.store(event->sessionGeneration, std::memory_order_release);
        Core::BuffDisplays().BeginSession(event->sessionGeneration);
        HoverGeometryLogged = false;
        break;
    case D2RL::Lifecycle::GameplayEventKind::LocalPlayerReady:
        if (CurrentSessionGeneration.load(std::memory_order_acquire) != event->sessionGeneration) {
            CurrentSessionGeneration.store(event->sessionGeneration, std::memory_order_release);
            Core::BuffDisplays().BeginSession(event->sessionGeneration);
        }
        OpenPanel();
        QueuePoll();
        if (!Internal::SkillIconStatus().ready && Threads != nullptr) {
            (void)Threads->runOnGameThread(Context, &RebuildIconsOnGameThread, nullptr);
        }
        break;
    case D2RL::Lifecycle::GameplayEventKind::GameLeft:
        Core::BuffDisplays().EndSession(event->sessionGeneration);
        CurrentSessionGeneration.store(0, std::memory_order_release);
        PollScheduled.store(false, std::memory_order_release);
        HoverGeometryLogged = false;
        ClosePanel();
        break;
    default:
        break;
    }
}

void PrintStatus(const D2RL::PluginContext* context) noexcept {
    if (context == nullptr) return;
    const auto snapshot = Core::BuffDisplays().Snapshot();
    const auto icons = Internal::SkillIconStatus();
    std::size_t shownBuffs = 0;
    std::size_t shownDebuffs = 0;
    std::string debuffNames;
    for (std::size_t i = 0; i < SlotCount; ++i) {
        if (!RenderStates[i].visible) continue;
        if (i < BuffSlotCount) {
            ++shownBuffs;
            continue;
        }
        ++shownDebuffs;
        if (!debuffNames.empty()) debuffNames += '|';
        debuffNames += RenderStates[i].hoverText.data();
    }
    HitSnapshot hits{};
    std::array<std::array<Core::PanelPosition, GroupCount>, ModeCount> saved{};
    bool dragging{};
    {
        std::lock_guard lock(PositionMutex);
        hits = Hits;
        saved = SavedPositions;
        dragging = Drag.active;
    }
    char positions[256]{};
    char bx[16]{}, by[16]{}, dx[16]{}, dy[16]{}, lbx[16]{}, lby[16]{}, ldx[16]{}, ldy[16]{};
    FormatPosition(saved[1][BuffGroup], bx, by);
    FormatPosition(saved[1][DebuffGroup], dx, dy);
    FormatPosition(saved[0][BuffGroup], lbx, lby);
    FormatPosition(saved[0][DebuffGroup], ldx, ldy);
    std::snprintf(positions, sizeof(positions),
        "saved(hd buffs=%s,%s debuffs=%s,%s legacy buffs=%s,%s debuffs=%s,%s) grids(buffs=%.0f,%.0f debuffs=%.0f,%.0f on %.0fx%.0f %s)",
        bx, by, dx, dy, lbx, lby, ldx, ldy,
        hits.groups[BuffGroup].left, hits.groups[BuffGroup].top,
        hits.groups[DebuffGroup].left, hits.groups[DebuffGroup].top,
        hits.frame.width, hits.frame.height, hits.frame.hd ? "hd" : "legacy");
    char line[1280]{};
    std::snprintf(line, sizeof(line),
        "Buff HUD 1.2.0: displayed=%zu shown=%zu/%zu debuffs=%zu/%zu session=%llu frame=%u panel=%s hideWithPanels=%s hiddenBy=%s companion=enabled skillIcons=%s skillNames=%s tableRevision=%llu hoverLabel=%s hover=\"%s\" debuffNames=\"%s\" %s drag=%s drags=%llu.",
        snapshot.count, shownBuffs, BuffSlotCount, shownDebuffs, DebuffSlotCount,
        static_cast<unsigned long long>(snapshot.sessionGeneration),
        snapshot.currentGameFrame,
        RegisteredPanel != D2RL::Panels::InvalidHandle ? "registered" : "missing",
        HideWithPanels ? "on" : "off",
        HidingPanel != nullptr ? HidingPanel : "none",
        icons.ready ? "ready" : "not-qualified",
        icons.namesReady ? "ready" : "not-qualified",
        static_cast<unsigned long long>(icons.tableRevision),
        NativeHoverName != nullptr ? "native" : "missing",
        HoverNameShown ? HoverNameText.data() : "",
        debuffNames.c_str(),
        positions,
        dragging ? "active" : SubclassedWindow != nullptr ? "ready" : "off",
        static_cast<unsigned long long>(DragsCompleted.load(std::memory_order_relaxed)));
    context->WriteConsoleMessage(line);
    // The console panel is not always open (or not readable): the log keeps it.
    context->LogInfo(line);
}

// `buff-panel panels [Name ...]`: for each native panel name (the hiding and
// probed lists, or the names given) whether it exists and is shown.
void PrintPanels(const D2RL::PluginContext* context, std::string_view names) noexcept {
    if (context == nullptr) return;
    std::string line = "Buff HUD panels:";
    auto add = [&](std::string_view name, bool hides) {
        const std::string text(name);
        void* panel = FindTopLevelPanel != nullptr ? FindTopLevelPanel(text.c_str()) : nullptr;
        line += ' ';
        line += text;
        line += panel == nullptr ? "=missing" : IsNativeShown(panel) ? "=shown" : "=hidden";
        if (hides) line += "(hides)";
    };
    if (names.empty()) {
        for (const auto* name : HidingPanels) add(name, true);
        for (const auto& panel : SidePanels) add(panel.name, false);
        for (const auto* name : ProbedPanels) add(name, false);
    } else {
        for (std::size_t at = 0; at < names.size();) {
            const auto start = names.find_first_not_of(" \t\r\n", at);
            if (start == std::string_view::npos) break;
            const auto end = std::min(names.find_first_of(" \t\r\n", start), names.size());
            add(names.substr(start, end - start), false);
            at = end;
        }
    }
    line += VisibleFlagQualified ? " visibleFlag=qualified" : " visibleFlag=not-qualified";
    context->WriteConsoleMessage(line.c_str());
    context->LogInfo(line.c_str());
}

D2RL::ConsoleCommandResult __cdecl BuffCommand(
    D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept {
    if (command == nullptr || command->plugin == nullptr) return D2RL::ConsoleCommandResult::Failed;
    const std::string_view args = command->args != nullptr
        ? std::string_view(command->args, command->argsLength) : std::string_view{};
    const auto first = args.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        PrintStatus(command->plugin); return D2RL::ConsoleCommandResult::Handled;
    }
    const auto last = args.find_last_not_of(" \t\r\n");
    const auto trimmed = args.substr(first, last - first + 1);
    if (trimmed == "status") {
        PrintStatus(command->plugin); return D2RL::ConsoleCommandResult::Handled;
    }
    if (trimmed.substr(0, 6) == "panels" && (trimmed.size() == 6 || trimmed[6] == ' ' || trimmed[6] == '\t')) {
        PrintPanels(command->plugin, trimmed.substr(6)); return D2RL::ConsoleCommandResult::Handled;
    }
    if (trimmed == "reset" || trimmed == "reset buffs" || trimmed == "reset debuffs") {
        const bool buffs = trimmed != "reset debuffs";
        const bool debuffs = trimmed != "reset buffs";
        {
            std::lock_guard lock(PositionMutex);
            for (auto& mode : SavedPositions) {
                if (buffs) mode[BuffGroup] = {};
                if (debuffs) mode[DebuffGroup] = {};
            }
            SaveRequested = true;
        }
        command->plugin->WriteConsoleMessage(buffs && debuffs
            ? "Buff HUD: buffs and debuffs back by the globes."
            : buffs ? "Buff HUD: buffs back by the life globe." : "Buff HUD: debuffs back by the mana globe.");
        return D2RL::ConsoleCommandResult::Handled;
    }
    command->plugin->WriteConsoleMessage("Usage: buff-panel [status | panels [Name ...] | reset [buffs | debuffs]]");
    return D2RL::ConsoleCommandResult::InvalidArguments;
}

[[nodiscard]] bool RegisterLayoutAndPanel() noexcept {
    if (Context == nullptr || Panels == nullptr) return false;
    // D2RLoader resolves the layout from our companion MPQ, including packed
    // active-mod overrides. Do not shadow it with a stale in-memory copy.
    const D2RL::Panels::PanelRegistration panel{
        .structSize = D2RL::Panels::PanelRegistrationSize,
        .flags = D2RL::Panels::PanelFlags::None,
        .localId = "BuffHud",
    };
    if (Panels->registerPanel(Context, &panel, &RegisteredPanel)
        != D2RL::Panels::Result::Success) {
        Context->LogError("Buff HUD: failed to register plugin-owned BuffHud panel.");
        return false;
    }
    return true;
}

[[nodiscard]] bool RegisterLifecycle() noexcept {
    if (Context == nullptr || Lifecycle == nullptr) return false;
    const D2RL::Lifecycle::DataTablesLoadedListener tableListener{
        .structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize,
        .flags = 0,
        .callback = &OnDataTablesLoaded,
        .userData = nullptr,
    };
    if (Lifecycle->registerDataTablesLoadedListener(Context, &tableListener, &DataTablesListener)
        != D2RL::Lifecycle::Result::Success) {
        return false;
    }

    constexpr std::array kinds{
        D2RL::Lifecycle::GameplayEventKind::GameJoined,
        D2RL::Lifecycle::GameplayEventKind::LocalPlayerReady,
        D2RL::Lifecycle::GameplayEventKind::GameLeft,
    };
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        const D2RL::Lifecycle::GameplayEventListener listener{
            .structSize = D2RL::Lifecycle::GameplayEventListenerSize,
            .flags = 0,
            .kind = kinds[i],
            .reserved = 0,
            .callback = &OnGameplayEvent,
            .userData = nullptr,
        };
        if (Lifecycle->registerGameplayEventListener(Context, &listener, &GameplayListeners[i])
            != D2RL::Lifecycle::Result::Success) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ValidateNativeUiContract() noexcept {
    if (Context == nullptr || Context->exeBase == 0) return false;
    if (!Context->CheckExpectedBytes(
            Native::Contract::FindTopLevelPanelRva,
            Native::Contract::FindTopLevelPanelExpected.data(),
            static_cast<std::uint32_t>(Native::Contract::FindTopLevelPanelExpected.size()))
        || !Context->CheckExpectedBytes(
            Native::Contract::FindChildWidgetByNameRva,
            Native::Contract::FindChildWidgetByNameExpected.data(),
            static_cast<std::uint32_t>(Native::Contract::FindChildWidgetByNameExpected.size()))) {
        Context->LogError("Buff HUD: native UI resolver fingerprint mismatch; refusing BuffHud initialization.");
        return false;
    }
    FindTopLevelPanel = reinterpret_cast<FindTopLevelPanelFn>(
        Context->exeBase + Native::Contract::FindTopLevelPanelRva);
    FindChildWidgetByName = reinterpret_cast<FindChildWidgetByNameFn>(
        Context->exeBase + Native::Contract::FindChildWidgetByNameRva);

    // Optional: without the native setters the buff group still works, but
    // the debuff group, the hover label and moving stay off.
    const bool settersMatch = Context->CheckExpectedBytes(
            Native::Contract::WidgetSetVisibleRva,
            Native::Contract::WidgetSetVisibleExpected.data(),
            static_cast<std::uint32_t>(Native::Contract::WidgetSetVisibleExpected.size()))
        && Context->CheckExpectedBytes(
            Native::Contract::WidgetSetXRva,
            Native::Contract::WidgetSetXExpected.data(),
            static_cast<std::uint32_t>(Native::Contract::WidgetSetXExpected.size()))
        && Context->CheckExpectedBytes(
            Native::Contract::WidgetSetYRva,
            Native::Contract::WidgetSetYExpected.data(),
            static_cast<std::uint32_t>(Native::Contract::WidgetSetYExpected.size()));
    if (settersMatch) {
        NativeSetVisibleFn = reinterpret_cast<WidgetSetVisibleFn>(
            Context->exeBase + Native::Contract::WidgetSetVisibleRva);
        NativeSetXFn = reinterpret_cast<WidgetSetCoordinateFn>(Context->exeBase + Native::Contract::WidgetSetXRva);
        NativeSetYFn = reinterpret_cast<WidgetSetCoordinateFn>(Context->exeBase + Native::Contract::WidgetSetYRva);
    } else {
        Context->LogWarn("Buff HUD: native widget setter fingerprint mismatch; debuffs, hover names and moving are off.");
    }
    return true;
}

} // namespace

bool Initialize(const D2RL::PluginContext* context, const Core::PluginConfig& config) noexcept {
    Shutdown();
    if (context == nullptr) return false;
    Context = context;
    HideWithPanels = config.hideWithPanels;
    {
        std::lock_guard lock(PositionMutex);
        SavedPositions[0] = {config.legacyBuffs, config.legacyDebuffs};
        SavedPositions[1] = {config.hdBuffs, config.hdDebuffs};
        for (auto& mode : SavedPositions) {
            if (config.resetBuffs) mode[BuffGroup] = {};
            if (config.resetDebuffs) mode[DebuffGroup] = {};
        }
    }

    const auto& services = Core::Services();
    Panels = services.panels;
    Widgets = services.widgets;
    Threads = services.threads;
    Lifecycle = services.lifecycle;

    const bool panelsReady = Panels != nullptr
        && D2RL::HasPanelServiceField(Panels, D2RL::PanelServiceRequiredSize);
    const bool widgetsReady = Widgets != nullptr
        && D2RL::HasWidgetServiceField(Widgets, D2RL::WidgetServiceRequiredSize);
    const bool threadsReady = Threads != nullptr
        && D2RL::HasThreadServiceField(Threads, D2RL::ThreadServiceRequiredSize);
    const bool lifecycleReady = Lifecycle != nullptr
        && D2RL::HasLifecycleServiceField(Lifecycle, D2RL::LifecycleServiceRequiredSize);

    if (!panelsReady || !widgetsReady || !threadsReady || !lifecycleReady) {
        char serviceLine[256]{};
        std::snprintf(
            serviceLine,
            sizeof(serviceLine),
            "Buff HUD: required PluginSDK service unavailable/undersized: Panel=%d Widget=%d Thread=%d Lifecycle=%d.",
            panelsReady ? 1 : 0,
            widgetsReady ? 1 : 0,
            threadsReady ? 1 : 0,
            lifecycleReady ? 1 : 0);
        Context->LogError(serviceLine);
        Shutdown();
        return false;
    }

    // D2RLoader ABI 4 does not guarantee that every named service is active for
    // every loader build/runtime phase. Localization is presentation-only here:
    // icons/timers/resources remain fully functional without it. The skill cache
    // retries LocalizationService when data tables are ready, so a service that
    // becomes available later still restores localized hover names.
    const bool localizationReady = Core::LocalizationService() != nullptr;
    if (!localizationReady) {
        Context->LogWarn(
            "Buff HUD: LocalizationService unavailable at plugin initialization; BuffHud remains enabled and localized hover names will be retried when the skill cache is rebuilt.");
    }

    if (!ValidateNativeUiContract()
        || !Internal::InitializeIconFrameBackend(Context)
        || !RegisterLayoutAndPanel()
        || !RegisterLifecycle()) {
        Shutdown();
        return false;
    }
    // reset_buffs / reset_debuffs act once: write the default spots and the
    // reset keys back as false.
    if (config.resetBuffs || config.resetDebuffs) SavePositions(true);

    if (!Context->RegisterConsoleCommand(
            "buff-panel",
            &BuffCommand,
            "Show Buff HUD status; 'buff-panel panels' lists the native panels that hide it.")) {
        Context->LogWarn("Buff HUD: console command 'buff-panel' could not be registered.");
    }
    Context->LogInfo(
        "Buff HUD 1.2.0 BuffHud initialized: 21 buff and 14 debuff display-only slots, companion-resource layout, fixed gameplay input isolation, timer/resource rendering, and runtime skill icon/name resolution.");
    return true;
}

void Shutdown() noexcept {
    RemoveWindowHook();

    Core::BuffDisplays().EndSession();
    Internal::ResetSkillIconCache();
    Internal::ShutdownIconFrameBackend();
    CurrentSessionGeneration.store(0, std::memory_order_release);
    PollScheduled.store(false, std::memory_order_release);
    // Restore any native countdown backing buffers we qualified before dropping
    // their pointers. This is bounded/readability-checked and does not call UI
    // services from the loader shutdown thread.
    RestoreQualifiedCountdownBuffers();
    RestoreQualifiedTooltipBuffers();
    RestoreHoverNameBuffer();
    InvalidateWidgetHandles();
    PollsWithoutPanel = 0;
    VisibleFlagQualified = false;
    EnabledFlagQualified = false;
    NativeSetVisibleFn = nullptr;
    NativeSetXFn = nullptr;
    NativeSetYFn = nullptr;
    {
        std::lock_guard lock(PositionMutex);
        SavedPositions = {};
        GroupDefaults = {};
        Drag = {};
        SaveRequested = false;
    }
    HidingPanel = nullptr;

    RegisteredPanel = D2RL::Panels::InvalidHandle;
    DataTablesListener = D2RL::Lifecycle::InvalidHandle;
    GameplayListeners = {};
    FindTopLevelPanel = nullptr;
    FindChildWidgetByName = nullptr;
    Panels = nullptr;
    Widgets = nullptr;
    Threads = nullptr;
    Lifecycle = nullptr;
    Context = nullptr;
}

} // namespace BuffPanel::Systems::BuffHud
