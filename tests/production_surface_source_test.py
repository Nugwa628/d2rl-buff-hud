from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LAYOUT = (ROOT / "companion/data/global/ui/layouts/buff-panel/BuffHudhd.json").read_text(encoding="utf-8")
sources = "\n".join(p.read_text(encoding="utf-8") for p in sorted((ROOT / "src").rglob("*.cpp")))
sources += "\n" + "\n".join(p.read_text(encoding="utf-8") for p in sorted((ROOT / "src").rglob("*.hpp")))

banned = {
    "StartDebugTest": "fake buff display harness",
    "DebugClock": "synthetic debug clock",
    "DebugKeyBase": "fake buff key namespace",
    "MousePolicy": "mouse A/B policy",
    "buff-panel-tracker": "tracker telemetry command",
    "buff-panel-status": "duplicate status command",
    "rebuild-icons": "manual resolver rebuild command",
    "StatReadInterceptor": "unused stat interception bus",
    "ReadEffective(": "unused intercepted stat-read API",
    "PollQueueFailures": "HUD telemetry counter",
    "RejectedNoExpiry": "dead tracker telemetry counter",
    "Player Buff StatList Probe": "probe provenance string",
    "Skill Icon HUD Probe": "probe provenance string",
}
for token, description in banned.items():
    assert token not in sources, f"production source still contains {description}: {token}"

assert sources.count('RegisterConsoleCommand(\n            "buff-panel"') == 1
assert "ApplyInputIsolation" in sources
assert "UpdateHoverNamePresentation" in sources
assert "EnsurePanelOpen" not in sources
assert "PanelReadyLogged" not in sources
assert "Panels->getPanelInfo" not in sources
assert "RenderSnapshot();\n    QueuePoll();" in sources
# Hover names are the layout's own HoverName label, drawn by the game: the
# OverlayService frame callbacks the 1.1.0 label relied on never ran.
assert "Overlay->drawText" not in sources
assert "Overlay->measureText" not in sources
assert "PublishHoverOverlay" not in sources
assert "WriteHoverNameText" in sources
assert "RestoreHoverNameBuffer" in sources
# Left Ctrl + drag moves a group through the game window's own procedure.
assert "BuffWindowProc" in sources and "CallWindowProcW" in sources
assert "MK_CONTROL" in sources and "VK_LCONTROL" in sources
assert "std::array<char, HoverTextBytes> hoverText" in sources
assert "disable(slot.countdown)" in sources
assert "disable(slot.slot)" in sources
assert "disable(GridWidget)" in sources
assert "disable(HudPanel)" in sources
assert "anchorX * frame.width + rect.x" in sources, "grid origin is anchor * screen + local x"
assert "QueuePoll();" in sources
assert "set(slot.tooltip, occupied && state.tooltipVisible)" not in sources
assert "StatListBuffMetadataBytes = 0x30" in sources
assert "TooltipReserveBytes = TooltipReserveLength + 1" in sources
# A UI rebuild (graphics switch) destroys the native panel. Cached widget state
# must be dropped, never restored into the destroyed widgets.
assert "EnsureLivePanel()" in sources
ensure = sources.split("bool EnsureTooltipQualified(", 1)[1].split("\n}\n", 1)[0]
assert "RestoreQualifiedTooltipBuffer(" not in ensure, "tooltip reserve restored into a replaced widget"
close = sources.split("void ClosePanel() noexcept {", 1)[1].split("\n}\n", 1)[0]
assert "IsResolvedPanelLive()" in close, "qualified buffers restored without a live-panel check"
assert close.index("IsResolvedPanelLive()") < close.index("RestoreHoverNameBuffer()"), "hover label restored without a live-panel check"
assert '"type": "FocusableWidget"' not in LAYOUT
assert LAYOUT.count('"name": "HoverName"') == 1, "one panel-level hover label"
assert "__BUFF_HUD_HOVER_NAME_RESERVE" not in LAYOUT
assert "__BUFF_HUD_HOVER_NAME_RESERVE" not in sources
assert "qualifiedHoverNameBuffer" not in sources
assert '"fitToParent": true' in LAYOUT
print("PASS: production runtime surface contains no known probe/debug/telemetry remnants")
