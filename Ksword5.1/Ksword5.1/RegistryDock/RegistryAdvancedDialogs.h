#pragma once

#include <QString>

class QWidget;

// Win32 permissions on the specified view; no owner takeover or SACL access.
void ShowRegistryKeyPermissions(QWidget* parent, const QString& path, int viewBits);

// An application hive loaded exclusively from a temporary working copy.
// All edits stay in that copy until the user explicitly saves another file.
void ShowRegistryOfflineHive(QWidget* parent);
