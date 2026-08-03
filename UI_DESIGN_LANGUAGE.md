# Strata UI: a unified design language for Point Cloud Inspector

Status: design investigation and product direction  
Audience: product, design, and engineering  
Scope: desktop UI and UX for professional point-cloud users

## Executive decision

Point Cloud Inspector should use a stable five-zone workspace:

1. **Scene at the left** — the objects in the document and their visibility.
2. **Work in the center** — the viewport and spatial tools.
3. **Properties at the right** — settings for the current selection.
4. **Commands at the top** — global, scene-wide, and active-tool commands.
5. **State at the bottom** — concise status, coordinates, and background tasks.

This is the core of **Strata**, a design language built around one rule:

> Put every feature where its scope and target are already visible.

A user should not have to remember where a command was placed. Selecting a
layer reveals layer properties; choosing a spatial tool reveals tool options;
starting background work creates a task; application-wide behavior lives in
Preferences. The menu bar remains the complete, canonical command index, while
toolbars and context menus are shortcuts rather than exclusive locations.

The proposed default layout is:

```text
┌ Menu: File  Edit  View  Layer  Tools  Help                         ┐
├ Command bar: Open ▾  Fit scene  |  EDL  Point size  |  Layout ▾   ┤
├──────────────┬────┬─────────────────────────┬──────────────────────┤
│ Scene        │Tool│                         │ Inspector            │
│              │rail│       Viewport          │                      │
│ ☑ survey.laz │    │                         │ Appearance           │
│ ☑ ground.laz │    │                         │ Filters              │
│              │    │                         │ Information          │
├──────────────┴────┴─────────────────────────┴──────────────────────┤
│ Ready  |  X / Y / Z  |  4.5 M visible       │ Tasks 1  |  GPU     │
└────────────────────────────────────────────────────────────────────┘
```

Panels remain dockable and resizable. The layout above is a reliable default,
not a restriction on experienced users.

## Investigation basis

This proposal is based on:

- the current main-window construction in
  [`src/app/MainWindow.cpp`](src/app/MainWindow.cpp);
- the current Layers dock and property editor in
  [`src/app/PointCloudLayerPanel.cpp`](src/app/PointCloudLayerPanel.cpp);
- classification filtering in
  [`src/app/ClassificationFilterDialog.cpp`](src/app/ClassificationFilterDialog.cpp);
- viewport input behavior in
  [`src/renderer/rhi/RenderViewportWidget.cpp`](src/renderer/rhi/RenderViewportWidget.cpp);
- loading behavior in
  [`src/app/LoadingOverlay.cpp`](src/app/LoadingOverlay.cpp);
- current UI contracts in
  [`tests/ui/MainWindowTests.cpp`](tests/ui/MainWindowTests.cpp);
- visual inspection of the running macOS application with a 4.5-million-point
  LAZ file.

This is a heuristic and architecture investigation, not user research. The
information architecture can be adopted now, while terminology, density, and
shortcut choices should later be validated with several users doing real
inspection and comparison tasks.

## What the current UI gets right

The current UI already contains several sound foundations:

- The viewport is the primary work surface.
- A layer list gives loaded sources a persistent, visible representation.
- Layer visibility uses a familiar checkbox model.
- Selecting a layer updates its controls without changing other layers.
- Long-running loads expose progress and cancellation.
- Menus and the toolbar share the same Open action.
- Color controls respond to the attributes available in the selected source.
- Platform-native Qt controls provide usable keyboard and accessibility
  behavior.
- Diagnostic UI is compile-time separated from the normal product.

Strata preserves these strengths. It changes the organization and presentation,
not the underlying document model.

## Current UX findings

### 1. The Layers panel has too many jobs

The current right dock contains source activity, the layer list, editable color
controls, classification filtering, read-only metadata, and optional residency
diagnostics. These are different objects and different task types:

- managing the scene;
- editing the selected layer;
- monitoring asynchronous work;
- inspecting source information;
- profiling the renderer.

As more features arrive, this single panel can only become longer and harder to
scan.

### 2. Scope is not consistently visible

The selected layer controls are placed next to **Apply color to all**. The
classification dialog offers both **Apply to selected layer** and **Apply to all
layers**. A user must inspect the wording of each action to understand whether
it affects one layer or the whole scene.

Professional tools should make scope structural:

- one selected layer means “this layer”;
- multiple selected layers means “these layers”;
- scene settings are visibly separated from selection settings.

### 3. Essential actions are hidden in context menus

Zoom to layer, Isolate, Show all, Remove, Prioritize, Retry, and Dismiss are
available only by right-clicking a row. Context menus are efficient after a
feature has been learned, but poor as the only route to discovery.

Every important command needs a visible or menu-based route. A context menu may
duplicate it close to the object.

### 4. Navigation behavior is powerful but invisible

The viewport supports:

- left-drag orbit;
- right-drag pan;
- wheel dolly around a picked point;
- double-click to set a pivot;
- W/A/S/D and Q/E movement;
- Shift for faster movement;
- Alt for finer movement;
- F to fit the scene.

None of this is explained in the shell. Users have to guess or discover it
accidentally. The UI also does not communicate an active navigation or future
editing tool.

### 5. “Properties” mixes editing and inspection

Color source, color map, range, and classification filtering are editable.
Point count, attributes, bounds, CRS, and source path are informational.
Putting both in one flat form weakens hierarchy and makes frequent controls
harder to scan.

### 6. Activity is represented in three competing places

Loading can appear as:

- a viewport-blocking overlay;
- Source activity in the Layers dock;
- status-bar text.

These presentations do not establish which one is authoritative. Adding a
layer also should not make an already useful viewport unavailable.

### 7. The status bar does not scale

The normal status bar can be concise. Diagnostic builds place a large quantity
of renderer and memory telemetry in the same single line. That makes both
important state and deep diagnostics harder to read.

### 8. Visual hierarchy is mostly inherited, not designed

The current application uses native widget defaults, a custom dark loading
overlay, group boxes, form rows, a toolbar, and dense diagnostic text. Each
part is individually serviceable, but there is no shared spacing, hierarchy,
state-color, icon, or density system tying them together.

## The Strata model

### Principle 1: Scope before control

Show the target before showing its controls. A control's location should answer
“what will this change?” before the user reads its label.

- Application scope → application menu or Preferences.
- Scene scope → command bar or Scene inspector.
- Selected-object scope → Inspector.
- Active-tool scope → tool options next to the viewport.
- Background-process scope → Tasks.

### Principle 2: Nouns left, properties right

The Scene panel is a tree of nouns: scenes, groups, and layers. The Inspector is
where the selected noun is understood and changed. This left-to-right
relationship remains valid when the application later adds clipping volumes,
annotations, measurements, camera bookmarks, or derived layers.

### Principle 3: The viewport is for spatial intent

Anything initiated by pointing at geometry belongs on the viewport tool rail:
orbit, pan, select, inspect point, measure, clip, annotate, and section tools.
Its detailed options belong in a compact tool-options strip or the Inspector.

### Principle 4: State is persistent; activity is transient

- Persistent results become scene objects.
- In-progress work becomes a task.
- A short confirmation becomes a toast.
- A blocking dialog is reserved for required decisions, destructive actions
  without undo, or errors that prevent continued work.

### Principle 5: One command, many entrances

Each command has one implementation and state model, then may appear in:

- the canonical menu;
- the command bar;
- a context menu;
- the command palette;
- a keyboard shortcut.

No feature should exist only in a context menu. Labels, enabled state,
checkmarks, icons, shortcuts, and tooltips must stay synchronized.

### Principle 6: Progressive disclosure without concealment

Show common controls immediately. Put advanced controls in named, remembered
sections—not in unlabeled overflow or context menus. The user should be able to
predict the advanced section before opening it.

### Principle 7: Quiet chrome, vivid data

The point cloud and its color map should dominate the visual field. Application
chrome uses neutral surfaces and a single interaction accent. Status colors are
semantic and never compete with scientific or classification colors in the
viewport.

## Stable workspace zones

### Menu bar: complete command index

The menu bar is the reliable answer to “what can this application do?”

| Menu | Contents |
|---|---|
| File | New scene, Open files, Add files, Open recent, Close scene, Export, Preferences/Settings, Quit |
| Edit | Undo, Redo, Cut/Copy when relevant, Select all, Deselect |
| View | Fit scene, standard views, projection, panels, overlays, appearance, workspace layouts |
| Layer | Add, fit selected, show/hide, isolate, duplicate when supported, remove |
| Tools | Navigation and spatial tools, with shortcuts and checked active state |
| Help | Controls reference, documentation, report issue, About |

Platform conventions take precedence: for example, Preferences and About use
their standard macOS locations.

### Command bar: frequent global and scene commands

The command bar contains commands used repeatedly across many workflows:

- Open/Add split button;
- Fit scene;
- projection or standard-view controls when added;
- Eye-Dome Lighting;
- point size;
- workspace layout selector.

It does not contain layer-specific fields, metadata, diagnostics, or every
possible command. Use labeled controls when an icon is not universally
understood. An icon-only command always has a tooltip containing its name,
shortcut, and one-line effect.

### Scene panel: document structure

The Scene panel replaces the list portion of the current combined dock.

Each layer row contains:

- disclosure affordance if it gains children;
- visibility toggle;
- type icon;
- editable display name;
- compact state badge only when needed;
- optional point-count or progress suffix.

Selecting a row changes the Inspector. Multi-selection is supported. A small
panel header provides **Add**, search/filter when needed, and an overflow menu.
The menu bar and row context menu contain Fit, Isolate, Show/Hide, and Remove.

Loading placeholders appear in final layer order within the Scene tree. A
placeholder can display progress and be selected, cancelled, or retried. It
turns into a normal layer in place, avoiding a separate “Source activity”
hierarchy for ordinary loads.

### Tool rail: spatial interaction modes

Use a narrow vertical rail inside or directly beside the viewport:

- Navigate;
- Select;
- Inspect point;
- Measure;
- Clip/section;
- Annotate.

Only modes that change pointer behavior belong here. Commands such as Open,
Export, EDL, or Remove do not.

The active tool has a persistent selected state and accessible name. Pressing
Escape cancels an in-progress operation, then returns to Navigate when pressed
again. Temporary navigation modifiers should be documented and visually
reflected if they alter the cursor.

For the current product, Navigate can expose a first-run hint:

```text
Orbit: left-drag   Pan: right-drag   Zoom: wheel   Fit: F
```

The hint disappears after use and remains available from Help → Controls.

### Inspector: contextual properties

The Inspector always names its target:

```text
3445-343.laz
Point-cloud layer
```

For one point-cloud layer it contains remembered sections:

1. **Appearance** — color by, color map preview, range, opacity when added.
2. **Filters** — classification and future return/range filters.
3. **Transform** — only when transforms are supported.
4. **Information** — points, attributes, bounds, CRS, source.
5. **Advanced** — source/index/residency details appropriate to end users.

Appearance and Filters start expanded; Information starts collapsed once the
user has interacted with the layer. Section state is remembered.

With multiple selected layers:

- the header says “3 layers”;
- common editable properties remain available;
- different values show **Mixed** rather than silently choosing one;
- edits apply to the selection;
- unsupported properties explain which selected items are incompatible.

This removes separate **Apply to all** controls. To change all layers, use
Select All Layers, then edit once. Scope stays visible in the selection header.

With no selection, the Inspector shows **Scene** settings and a short prompt to
select a layer. It must never show stale properties for an unselected object.

### Status and Tasks: operational awareness

The status bar is a compact, stable summary:

- left: Ready, rendering, warning, or current interaction;
- center: cursor or picked X/Y/Z and active selection count;
- right: visible points, Tasks badge, and optional performance summary.

Clicking **Tasks** opens a bottom drawer containing each active, failed, or
recently completed operation. Task rows contain:

- source/action name;
- phase and progress;
- elapsed or remaining work when known;
- Cancel, Retry, Reveal, or Dismiss as applicable;
- error details behind a disclosure control.

Opening the first scene may use a non-blocking centered progress card because
there is no work to preserve. Adding files to an existing scene must leave the
viewport interactive and use Scene placeholders plus Tasks. Completed work
should disappear after a short history period unless it needs attention.

Deep renderer, cache, and memory telemetry belongs in a separate
**Diagnostics** dock opened from View → Panels → Diagnostics. The status bar may
show one compact frame-time or GPU indicator that opens that dock.

## Feature placement algorithm

Every new feature proposal must answer these questions in order:

```text
1. What does it act on?
   Application → Preferences
   Scene       → Scene inspector or command bar
   Selection   → Inspector
   Geometry    → Tool rail
   Process     → Tasks

2. Is it a mode or a command?
   Mode        → persistent selected tool + visible options
   Command     → canonical menu action; optionally duplicate nearby

3. Is its result persistent?
   Yes         → create/update an object in Scene and support Undo
   No          → transient viewport feedback, toast, or status

4. How often is it used?
   Frequent    → visible in its zone
   Occasional  → named section or menu
   Advanced    → Advanced section or dedicated dock

5. Does it require a decision before continuing?
   No          → inline update or task
   Yes         → sheet/dialog with one clear primary action
```

If two answers point to different locations, the target wins. For example,
“export selected layers” is canonically a File command but obtains its scope
from Scene selection; it does not become an Inspector property.

## Feature placement matrix

| Feature type | Primary home | Secondary entrance | Never the only home |
|---|---|---|---|
| Open/Add/Export | File menu | Command bar, drop target | Context menu |
| Global rendering appearance | Command bar or Scene inspector | View menu | Layer Inspector |
| Layer appearance | Inspector → Appearance | Layer context presets | Global toolbar |
| Layer filters | Inspector → Filters | Layer menu | Modal-only flow |
| Visibility and grouping | Scene | Layer menu/context menu | Inspector-only toggle |
| Fit/standard views | View menu | Command bar, layer context menu | Undocumented shortcut |
| Orbit/pan/select/measure/clip | Tool rail | Tools menu, shortcuts | Context menu |
| Long-running import/export/indexing | Tasks | Scene placeholder, status badge | Blocking overlay on an active scene |
| Metadata and CRS | Inspector → Information | Copy command | Status bar |
| Performance/residency telemetry | Diagnostics dock | Compact status indicator | Layer Properties |
| Application defaults and budgets | Preferences | — | Per-scene Inspector |
| Errors tied to a task | Task details | Toast/status badge | Generic message box only |

## Placement examples for likely future features

| Proposed feature | Placement and behavior |
|---|---|
| Point inspection | Inspect tool in rail; picked point values in Inspector; Escape clears |
| Distance/area measurement | Measure tool; each committed measurement becomes a Scene object; properties in Inspector |
| Clipping box | Clip tool creates a Scene object; geometry edited in viewport; numeric values in Inspector |
| Elevation filter | Selected layer Inspector → Filters → Elevation |
| Global elevation normalization | Scene Inspector → Appearance, explicitly labeled scene-wide |
| Layer opacity | Layer Inspector → Appearance, live and undoable |
| Planar vector placement | Vector Layer Inspector → Placement; state “planar overlay, not terrain draping” | Layer context menu → Show anyway for an XY-disjoint layer | Global elevation controls |
| Background color | Scene Inspector → Appearance; frequent preset may also appear in View |
| Orthographic projection | View menu and command bar segmented control |
| Top/front/side views | View menu and command bar view control |
| Export selected points | File → Export Selected; scope comes from current selection/filter state and is confirmed in export sheet |
| Screenshot | File → Export Image and command-bar camera action if frequently used |
| Camera bookmarks | Scene tree collection; Add Bookmark command near viewport |
| Annotations | Annotation tool creates Scene objects; text/style in Inspector |
| CRS reprojection | Scene/layer command that opens a focused task sheet; progress in Tasks |
| Cache management | Preferences → Storage, with current usage and recoverable Clear action |
| GPU/memory budgets | Preferences → Performance; Diagnostics shows actual use, not editable defaults |
| Plugin tools | Tools menu and tool rail only when they create a pointer mode; plugin output becomes a Scene object or Task |

## Terminology

Use one noun for each concept everywhere:

| Concept | Preferred wording | Avoid |
|---|---|---|
| Working collection | Scene | project/document when not actually persisted |
| Loaded source | Layer | cloud/file when referring to the scene object |
| Add without replacing | Add files… | Open when the result is additive |
| Replace current scene | New scene from files… | Open New Scene after a generic Open |
| Make bounds fill view | Fit scene / Fit selection | Zoom to layer, Frame scene, Focus |
| Attribute used for color | Color by | Color |
| Lookup palette | Color map | Palette/ramp interchangeably |
| Temporarily show one item | Isolate | Solo unless adopted everywhere |
| Remove scene object | Remove layer | Delete file |
| Background operation | Task | job/activity/status interchangeably |

Labels use sentence case. Ellipses mean that another step is required, not that
an operation is merely slow. Destructive labels name the object:
**Remove layer**, not **OK**.

## Interaction language

### Selection is the editing scope

- Single click selects one object.
- Command/Ctrl-click toggles objects in the selection.
- Shift-click extends a range.
- Clicking empty viewport space clears geometric selection, not necessarily
  the active layer.
- Scene and geometric selection are visually distinct when both exist.
- The Inspector header always states the active scope.

### Changes are immediate and reversible

Appearance and filter edits update the viewport immediately. They participate
in one Undo stack once an undo model exists. Numeric fields preview while
scrubbing and commit on Enter, focus loss, or a short debounce; Escape restores
the pre-edit value.

Avoid Apply buttons for ordinary properties. Use Apply only for a staged,
expensive operation where users must review parameters before a task begins.

### Modes are explicit

The cursor, selected tool, tool options, and status hint all communicate the
same mode. A keyboard shortcut updates the visible tool selection. A temporary
modifier changes the cursor while held and returns predictably on release.

### Errors stay attached to their cause

- Invalid field → inline message beside the field.
- Failed import/export → failed task row and layer placeholder state.
- Unsupported GPU/backend → persistent banner with actionable recovery.
- Unexpected application failure → dialog with copyable details.

Never rely on color alone. Use an icon, status word, and accessible description.

### Loading does not erase context

The user may navigate and edit loaded layers while another source loads. Only a
scene-replacing operation may ask for a choice before it begins. Cancellation
returns to the previous valid scene state.

## Visual language

### Character

Strata should feel precise, calm, and information-dense—not decorative. It uses
flat neutral surfaces, restrained separators, tabular data, and clear selected
states. Point-cloud color remains the visual focus.

### Spacing and sizing tokens

Use a 4 px logical-pixel base grid. Qt high-DPI scaling remains authoritative.

| Token | Value | Use |
|---|---:|---|
| `space-1` | 4 | icon/text gaps, tight internal spacing |
| `space-2` | 8 | field and row spacing |
| `space-3` | 12 | compact group padding |
| `space-4` | 16 | panel padding and section separation |
| `space-6` | 24 | major empty-state separation |
| compact row | 28 | dense tree and task rows |
| standard row | 32 | fields and labeled buttons |
| command bar | 40 | primary toolbar height |
| tool target | 32 × 32 minimum | pointer tools |
| panel width | 280 default, 220 minimum | Scene |
| inspector width | 320 default, 260 minimum | Inspector |

Offer **Compact** and **Comfortable** density in Preferences. Density changes
row height and padding, not information architecture.

### Typography

- Use the platform UI font for all chrome.
- Use three stable roles: title, body/control, and secondary/caption.
- Use semibold sparingly for panel target names and section titles.
- Use tabular numerals for coordinates, point counts, ranges, memory, and time.
- Use a platform monospace font only for paths, CRS definitions, IDs, and
  diagnostic logs.
- Do not encode hierarchy through many font sizes; spacing and alignment do
  most of the work.

### Semantic color tokens

Colors are semantic roles, not widget-specific constants:

| Token | Purpose |
|---|---|
| `surface-canvas` | viewport-adjacent base |
| `surface-panel` | Scene and Inspector |
| `surface-raised` | popovers, menus, cards |
| `border-subtle` | panel and section separation |
| `text-primary` | normal content |
| `text-secondary` | metadata and hints |
| `accent` | selected control and keyboard focus |
| `selection` | selected rows and objects |
| `success` | completed task |
| `warning` | degraded or attention state |
| `danger` | failure and destructive confirmation |

Support system light and dark appearance. The viewport background is a scene
setting and does not determine the chrome theme. Classification and scalar-map
colors are data; they must not be reused as UI status colors.

Contrast targets:

- normal text: at least 4.5:1;
- large text and graphical controls: at least 3:1;
- keyboard focus: clearly visible on every surface;
- disabled state: still legible, with unavailability explained by tooltip or
  inline text where important.

### Icons

Use one coherent 16 px, monochrome, outline-style icon family with filled
selected variants only where useful. Icons communicate object or action, never
decoration. Pair text with unfamiliar or destructive icons. Do not mix Qt
standard icons, emoji, and unrelated third-party icon styles.

### Surfaces and separators

Prefer spacing and 1 px separators to nested group-box frames. Inspector
sections use a disclosure row and a single content indentation. Avoid multiple
rounded cards inside already bounded panels; reserve raised cards for transient
content such as an empty-state prompt or first-load progress.

## Component rules

### Scene row

One row owns visibility, name, state, and compact progress. Hover reveals
secondary actions only if they are also available in the Layer menu. Do not put
editable property widgets inside the row.

### Inspector field

Labels align consistently above fields in narrow panels and to the left only
when there is enough width. Units remain visible. Long paths and CRS values use
middle elision with Copy and Reveal actions.

For scalar ranges, present a single control group:

```text
Range   [ Auto ✓ ]
        Min [  12.00 ]  Max [ 184.00 ]
```

Disabled manual values remain visible when Auto is active so users can
understand the current range.

### Color-map field

Show a small gradient/categorical preview beside the map name. The menu repeats
the preview. Incompatible maps do not appear. “Color by RGB” disables or hides
the map field with a short explanation.

### Filter summary

Filters remain inline in the Inspector. A summary such as
**Classification · 4 of 7 visible** is the collapsed section label. Opening it
shows searchable checks, Select all, Clear, and optional presets. A separate
modal is justified only when the filter becomes too large for the Inspector.

### Buttons

- Primary button: one per dialog or focused task sheet.
- Secondary button: supporting actions.
- Quiet/icon button: reversible local actions.
- Danger button: destructive action after intent is clear.
- Split button: one default action plus closely related alternatives.

“Apply to all” is not a button category; selection determines scope.

### Tooltips

A command tooltip follows:

```text
Fit scene (F)
Fits all visible layers in the viewport.
```

Disabled controls explain why and, when possible, what resolves the condition.

### Empty states

An empty viewport says what the product accepts and offers the primary action:

```text
Open point-cloud files
LAS, LAZ, COPC, or EPT
[ Open files… ]
You can also drop files here.
```

An empty Scene filter says **No layers match this filter**, not **No point
clouds loaded**.

### Dialogs

Use dialogs for Preferences, export parameters, required scene-replacement
decisions, and irreversible confirmation. Titles name the task. Buttons use
verb phrases. Preserve the user's context behind modeless sheets where the
platform supports them.

## Current-feature migration

| Current feature | Strata destination |
|---|---|
| Open Point Cloud toolbar/menu | Open/Add split command plus File menu |
| Existing-scene Add/Replace dialog | Split command when initiated explicitly; focused replace/add decision for dropped or generic-open files |
| Layers toggle | View → Panels → Scene; optional command-bar layout control |
| Layer visibility | Scene row |
| Layer selection | Scene row with multi-selection |
| Zoom to layer | Layer → Fit Selected and row context menu |
| Isolate / Show all | Layer menu, row context menu, and Scene panel header as appropriate |
| Remove | Layer menu, row context menu, Delete/Backspace shortcut where safe |
| Eye-Dome Lighting | Command bar and View → Appearance |
| Point size | Command bar compact field and Scene Appearance setting |
| Color source/map/range | Inspector → Appearance |
| Apply color to all | Remove; select layers and edit |
| Classification dialog | Inspector → Filters; selection sets scope |
| Points/attributes/bounds/CRS/source | Inspector → Information |
| Source activity list | Scene placeholders plus Tasks drawer |
| Loading overlay | First-empty-scene progress card only |
| Cancel Loading | Tasks badge/drawer; File menu only if a global cancel command remains |
| Residency group | Diagnostics dock |
| Diagnostic status string | Diagnostics dock with a compact optional status summary |
| Undocumented viewport controls | Tool rail, Help → Controls, first-run hint, visible shortcuts |

## Engineering model that supports the language

### Central command registry

Build each user command once as a `QAction` or equivalent command object with:

- stable ID;
- label and menu label;
- icon;
- shortcut;
- tooltip/help text;
- enabled/checked predicate;
- handler;
- analytics/diagnostic name if later required.

Menus, command bar, context menus, command palette, and tests reference the same
command. This extends the existing correct sharing of the Open action.

Suggested command ID shape:

```text
file.open
file.add
view.fit_scene
view.toggle_edl
layer.fit_selected
layer.isolate_selected
layer.remove_selected
tool.navigate
tool.measure
panel.scene.toggle
panel.inspector.toggle
```

### Explicit UI context

Expose one read-only `UiContext` snapshot containing:

- current scene/document;
- selected scene object IDs;
- active tool;
- active tasks;
- viewport capabilities;
- diagnostic-build capabilities.

Commands and Inspector sections derive visibility and enabled state from that
context. Avoid letting individual widgets infer scope independently.

### Separate panels by responsibility

Evolve `PointCloudLayerPanel` into:

- `SceneDock` — object model, visibility, selection, load placeholders;
- `InspectorDock` — editors and information for the selected object(s);
- `TasksDock` or bottom drawer — asynchronous operations;
- `DiagnosticsDock` — optional technical telemetry.

`MainWindow` remains the composition root but should not manually construct
every field. Panel-specific view models translate the document and task state
into display state.

### Design tokens over per-widget styling

Define palette, spacing, icon, typography, and density tokens centrally.
Prefer `QPalette`, a small `QProxyStyle`, and reusable widgets. Keep style
sheets narrow and semantic; the current loading overlay should consume the same
surface, text, border, accent, and progress tokens as the rest of the app.

### Persistent workspace

Use `QSettings` to remember:

- dock positions and visibility;
- panel widths;
- Inspector section expansion;
- density and theme choice;
- last active non-destructive tool.

Provide **View → Workspace Layouts → Reset to Default** so customization is
always recoverable. Do not restore transient selections or stale loading
states.

### Accessibility and automation

Every custom or icon-only control needs an accessible name and tooltip. Preserve
logical tab order by zone. Scene rows expose checked, selected, busy, and failed
states. All commands remain keyboard reachable through menus even without a
shortcut.

Stable object names should describe semantic roles rather than visual
placement, making UI tests resilient when panels move.

## Delivery sequence

### Phase 1: establish the information architecture

No new end-user feature is required.

1. Add canonical File, Edit, View, Layer, Tools, and Help menus.
2. Introduce a central command registry.
3. Split Scene and Inspector into separate docks.
4. Move diagnostics to its own dock.
5. Add a concise status bar and Tasks entry point.
6. Add Help → Controls and a one-time viewport navigation hint.
7. Preserve existing functionality and shortcuts.

This phase solves the largest discoverability and feature-placement problems.

### Phase 2: unify components and visual tokens

1. Introduce semantic palette and spacing tokens.
2. Replace nested group boxes with Inspector sections.
3. Standardize rows, fields, buttons, icons, empty states, and tooltips.
4. Add compact/comfortable density and light/dark validation.
5. Make loading presentation consume the shared system.

### Phase 3: professional productivity

1. Add multi-selection and mixed-value editing.
2. Replace bulk-apply buttons with selection-scoped edits.
3. Add command palette/search if the command set warrants it.
4. Persist workspace layouts and panel state.
5. Add undo for appearance and filter changes.

### Phase 4: grow by the placement rules

Add inspection, measurement, clipping, annotations, export, or other product
features only after each proposal passes the feature placement algorithm and
uses the shared command and component systems.

## Validation plan

### Workflow checks

Test with at least these scenarios:

1. First-time user opens one file, understands navigation, changes color, and
   finds source information.
2. Experienced user opens five files, compares them, isolates two, applies a
   shared filter, and restores all visibility.
3. User adds a large file while continuing to inspect existing layers, then
   cancels the load.
4. User finds Fit Scene, Fit Selected, EDL, point size, and classification
   filtering without right-clicking.
5. User identifies whether an edit affects one layer, several selected layers,
   or the whole scene before activating it.
6. Diagnostic user finds frame, memory, residency, and task details without
   obscuring ordinary status.
7. Keyboard-only user opens files, moves between zones, edits a field, invokes
   a menu command, and returns focus to the viewport.

### Measurable acceptance criteria

- Every non-contextual command is discoverable from a menu.
- No essential feature is context-menu-only or shortcut-only.
- The active target is named in the Inspector at all times.
- A bulk edit never uses an ambiguous hidden scope.
- Adding a layer never blocks interaction with an already valid scene.
- The normal status bar remains readable at 900 logical pixels wide.
- Panels remain usable at their documented minimum widths without clipped
  labels or horizontal scrolling.
- All icon-only controls have accessible names and explanatory tooltips.
- Light, dark, compact, comfortable, 100%, and 200% scale variants pass visual
  regression checks.
- Existing Open, visibility, appearance, filtering, loading, cancellation, and
  renderer contracts remain covered by automated tests.

### Research questions

Observe professional users rather than asking for preference alone:

- Do users think in “scene/layer,” “project/dataset,” or another vocabulary?
- Do they expect layer management on the left or prefer keeping it on the
  right?
- Which appearance controls must remain visible during comparison?
- Is multi-selection enough for bulk edits, or are saved layer groups needed?
- Which navigation convention do users bring from their other 3D/GIS tools?
- Which status data is needed continuously versus only during troubleshooting?
- Does classification filtering fit comfortably inline for real datasets?

The stable-zone architecture should survive different answers. Research should
tune labels, defaults, shortcuts, and density—not send every new feature to a
new location.

## Feature admission checklist

Before implementation, every UI-facing feature should document:

- [ ] target scope: application, scene, selection, geometry, or process;
- [ ] primary zone;
- [ ] canonical command and menu location, if it is a command;
- [ ] visible selected/active state, if it is a mode;
- [ ] empty, loading, success, disabled, and error states;
- [ ] keyboard access and accessible name;
- [ ] undo or cancellation behavior;
- [ ] behavior for one selection, multiple selections, and mixed values;
- [ ] compact and narrow-panel behavior;
- [ ] light/dark and high-DPI behavior;
- [ ] automated UI contract;
- [ ] whether it creates a Scene object, Task, or transient result.

If these answers are unclear, the feature is not yet ready to enter the UI.
