/*
 * LiteKVM — zero-config keyboard/mouse sharing
 * SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
 * SPDX-License-Identifier: GPL-2.0
 */

#pragma once

#include <QString>

namespace deskflow::gui {

/**
 * @brief Applies the LiteKVM dark theme to the running QApplication.
 *
 * Three layers, in this order:
 *  1. application font  — CJK-first family stack (Microsoft YaHei UI → … → Segoe UI)
 *  2. dark QPalette     — only for glyphs drawn by QStyle that QSS cannot recolour
 *                         (combo/spin arrows, …); visible surfaces come from QSS
 *  3. stylesheet        — litekvm-theme-dark.qss (Qt resource, disk fallback)
 *
 * Must be called *after* the QApplication is constructed and *after*
 * `updateIconTheme()` (it re-pins the icon theme to the dark variant), but
 * before MainWindow is created. Calling it twice is harmless.
 *
 * Environment:
 *  - `LITEKVM_DISABLE_THEME=1`  → no-op, keeps the stock platform look
 *  - `LITEKVM_THEME_QSS=<path>` → load that stylesheet file instead of the
 *                                 packaged resource (stylesheet hot-tuning)
 *
 * @return true when a stylesheet was found and installed.
 */
bool applyLiteKvmTheme();

/**
 * @brief Location the stylesheet was loaded from ("" if none was found).
 * Useful for logs / diagnostics / tests.
 */
QString liteKvmThemeSource();

} // namespace deskflow::gui
