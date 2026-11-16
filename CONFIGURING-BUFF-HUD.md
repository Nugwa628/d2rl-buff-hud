# Buff HUD 1.0.11 — buff-hud.txt

The runtime override is optional. Without it, Buff HUD uses the catalog embedded in `src/systems/buff_tracker/buff_tracker.cpp`.

Place an override at:

```text
<game>/mods/<mod>/<mod>.mpq/data/global/excel/d2rloader/buff-panel/buff-hud.txt
```

The exact tab-separated columns are:

```text
name	state_id	display_type	value_stat	max_stat	skill_id	value_shift	enabled
```

There is intentionally **no skill-level field**.

For `display_type=timer`:
- `value_stat`, `max_stat`, and `value_shift` must be `0`.
- `skill_id=0` means skill identity must come from the native StatList.
- `skill_id!=0` is a fallback only; a nonzero native StatList skill ID always wins.
- A valid finite future native expiry is required. There is no state-presence-only timer path.

For `display_type=resource`, `value_stat`, `max_stat`, and `skill_id` are required as before.

For `display_type=debuff` (a harmful state on the player: a curse, poison, freezing, ...):
- `value_stat`, `max_stat`, and `value_shift` must be `0`; `skill_id` (0 to 4095) only picks the icon and name when
  `buff-hud-display.txt` has none for the state.
- The icon shows while the player has the state, in the debuff group by the mana globe with a red frame. The time left
  shows when the game gives the state an end; curses and auras without one show no timer.

```text
debuff_amplify_damage	9	debuff	0	0	0	0	1
```

The embedded defaults include:

```text
shout	26	timer	0	0	138	0	1
battle_orders	32	timer	0	0	149	0	1
battle_command	51	timer	0	0	155	0	1
```

## buff-hud-display.txt — icon and name overrides

Buff HUD normally takes a buff's icon and tooltip name from its source skill. When a state has no
usable skill (a custom state, or a skill whose description has no icon), add a row to the optional
presentation table:

```text
<game>/mods/<mod>/<mod>.mpq/data/global/excel/d2rloader/buff-panel/buff-hud-display.txt
```

The exact tab-separated columns are:

```text
state_id	icon_atlas	icon_frame	name_key	name	item_code
```

- `state_id` is a state already tracked through `buff-hud.txt` (1-4095). Each state appears at most once.
- `icon_atlas` is one of `amazon`, `sorceress`, `necromancer`, `paladin`, `barbarian`, `druid`,
  `assassin`, `warlock` or `global` (any case). Empty keeps the automatic icon.
- `icon_frame` is the frame inside that atlas. It needs `icon_atlas`; `65535` is reserved.
- `name_key` is an optional localization string key. When it resolves, it is the tooltip name.
- `name` is the literal tooltip name used when `name_key` is empty or does not resolve (up to 63 ASCII characters).
- `item_code` is an optional item code (1-4 letters or digits, e.g. `hp5`). When the layout has an
  `Item_<code>` widget in the slot, that item's inventory picture replaces the icon; otherwise the
  atlas icon is used.

Rows that leave a field empty keep the automatic value for that field, so a state without a row
behaves exactly as before. The table is presentation only: it never adds or removes tracked buffs,
and an invalid table is logged and ignored instead of disabling the HUD.

Example (stock states; every row has all six columns, empty fields included):

```text
state_id	icon_atlas	icon_frame	name_key	name	item_code
32	barbarian	26			
51			MyBattleCommandName	Battle Command	
```

The companion ships a header-only default, which means no overrides.

### Item pictures

Both layouts declare one hidden `Widget` per item code in every `BuffSlot`, named `Item_<code>`,
holding one `ImageWidget` named `Picture`.
The shipped layouts cover the stock potions `hp1`-`hp5`, `mp1`-`mp5`, `rvs`, `rvl`, `vps`, `yps` and
`wms`. The HD widget points at the item's HD inventory sprite (for example
`Items/misc/potion/greater_healing_potion`), the legacy widget at its inventory DC6 (for example
`..\\items\\invhp5`). To use another item, add an `Item_<code>` container with the same name to all 21
slots of both `BuffHudhd.json` and `BuffHud.json`; `layout_parity` checks that both layouts agree.
The game does not apply a layout's `"visible": false`, so BuffHud hides every picture by name: the
stock codes above and every `item_code` used in `buff-hud-display.txt`. A layout must not declare
pictures for other codes (they would show in every slot); `item_pictures` checks this.
D2RLoader has too few widget handles for hundreds of pictures, so BuffHud switches them on the
native widgets. It learns the native visibility bit once by toggling a slot's
`Countdown` through the WidgetService. The log line `item pictures found=... hidden=...
visibleFlag=...` reports the result; if the bit is not qualified, slots keep their skill icons
(the pictures then cannot be hidden either).

## buff-panel.toml — turning the panel off, moving the icons

D2RLoader writes the plugin's default settings to `d2rloader\config\buff-panel.toml` (next to the plugin's own
`d2rloader` folder: the game folder, or the mod folder for a mod-scoped install) the first time it loads:

```toml
[buff_panel]
enabled = true
hide_with_panels = true
hd_buffs_x = -1
hd_buffs_y = -1
hd_debuffs_x = -1
hd_debuffs_y = -1
legacy_buffs_x = -1
legacy_buffs_y = -1
legacy_debuffs_x = -1
legacy_debuffs_y = -1
reset_buffs = false
reset_debuffs = false
```

Set `enabled = false` and start the game again: the plugin still loads, but shows no panel and installs nothing.

`hide_with_panels = true` (the default; a file without the key keeps it) hides the icons while the stash or the Horadric
Cube is open, in HD and legacy graphics, and shows them again when it closes. A side panel (character, quests, waypoints,
a vendor, trade, the mercenary, the party; inventory, skill tree) hides them only while it overlaps an icon on screen: in
legacy graphics a left panel always does, a right panel from the sixth or seventh icon; in HD only the top row reaches a
right panel. So a player can fight with the inventory open. `false` keeps them shown always. `buff-panel status` reports `hiddenBy=<panel>`;
`buff-panel panels [Name ...]` lists which native panels exist and are shown (the visibility bit is the one learned for
item pictures above: with `visibleFlag=not-qualified` nothing hides).

### Moving the icons

Buffs start by the life globe and fill from there; debuffs start by the mana globe. Hold **Left Ctrl** and drag any
icon with the left mouse button to move its group; the click does not reach the game, so the character does not walk.
Each group keeps its own spot, once for HD and once for legacy graphics: `hd_buffs_x` / `hd_buffs_y` and so on hold
the group's top-left corner as a share of the screen (0 to 1), written when you let go. A group always stays wholly on
the screen, also after a change of resolution. `-1` means the default spot by the globe.

To put a group back: set `reset_buffs = true` or `reset_debuffs = true` (the next game start moves it back in HD and
legacy and writes the key back as `false`), or type `buff-panel reset` in the console (`buff-panel reset buffs` or
`buff-panel reset debuffs` for one group).

`buff-panel status` shows the saved spots (`saved(...)`), where the groups are now (`grids(...)`), the shown debuffs
(`debuffNames=`) and the name under the mouse (`hover=`).
