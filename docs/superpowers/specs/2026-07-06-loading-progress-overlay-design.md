# Loading progress overlay design

Date: 2026-07-06

## Goal

Add clear, centered loading feedback while importing point cloud files. The first pass should make long-running loads visibly active, show progress when the loader can report it, and let the user cancel from the same overlay.

This is a proof-of-concept UI layer over the existing import pipeline. It should not change point cloud loading semantics, renderer behavior, or the existing status bar feedback.

## User-facing behavior

When the user opens a point cloud file, the current point cloud remains visible in the viewport. A dimmed overlay appears above it with a centered panel containing:

- the loaded file name,
- a progress bar,
- progress details,
- a Cancel button.

If the import reports a known total point count, the progress bar is determinate and shows percentage plus `processed / total` points. If the total is unknown or zero, the progress bar uses an indeterminate busy state with text such as “Reading point cloud…”.

Clicking Cancel disables the Cancel button, updates the overlay text to indicate cancellation is pending, and forwards cancellation through the existing load controller. The overlay remains visible until the controller emits success, failure, or cancellation.

On success, failure, or cancellation, the overlay hides. Existing status bar messages remain as secondary feedback.

## Architecture

Introduce a small `LoadingOverlay` widget under `src/app`.

`LoadingOverlay` is a pure UI component. It owns the centered panel, progress bar, labels, and Cancel button. It exposes a minimal API:

- start or show loading for a source path/name,
- update progress from `processed` and `total`,
- enter a cancelling state,
- hide/reset,
- emit `cancelRequested()`.

`MainWindow` owns and wires the overlay. Instead of setting the renderer widget directly as the central widget, `MainWindow` creates a central container using a stacked overlay layout:

1. bottom layer: existing viewport widget,
2. top layer: `LoadingOverlay`.

The stack uses overlay behavior so the existing point cloud remains visible behind the dimmed loading overlay.

## Data flow

The overlay is driven by the existing `PointCloudLoadController` signals:

1. `MainWindow::loadPointCloud(...)` starts loading and shows the overlay immediately.
2. `progressChanged(processed, total)` updates the progress bar and detail text.
3. `loaded(...)` hides the overlay after updating the viewport.
4. `failed(...)` hides the overlay before/while showing the failure feedback.
5. `cancelled()` hides the overlay after cancellation is acknowledged.
6. `LoadingOverlay::cancelRequested()` calls the existing controller cancel path.

The overlay should not own worker threads, import logic, point cloud data, or renderer state.

## Error handling and edge cases

- If loading fails, the overlay hides and the existing failure message path remains responsible for communicating the error.
- If cancellation is requested multiple times, only the first click should matter because the Cancel button is disabled immediately.
- If progress arrives after cancellation was requested, the overlay may still update numeric progress, but the cancel-pending state should remain clear until the controller finishes.
- If total progress is missing or zero, the UI should remain useful by showing an indeterminate progress bar.

## Testing

Add UI-level tests around `MainWindow` and/or `LoadingOverlay`:

- overlay is hidden before loading,
- starting a load shows the overlay above the viewport,
- progress updates the progress bar and detail label,
- the Cancel button triggers the existing cancellation path and disables itself,
- success hides the overlay,
- failure hides the overlay,
- cancellation hides the overlay.

Tests should use object names for stable lookup, for example:

- `loadingOverlay`,
- `loadingTitleLabel`,
- `loadingProgressBar`,
- `loadingDetailsLabel`,
- `loadingCancelButton`.

Rendering and Metal behavior do not need new tests for this change because the overlay is a Qt widget layer over the existing viewport.

## Out of scope

- Background import architecture changes.
- Renderer changes.
- Import performance changes.
- Per-file-format progress estimation beyond what the current loader reports.
- A global task manager or queue.
- Windows/Linux-specific UI adjustments.
