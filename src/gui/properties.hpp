// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: the Properties dialog (general information, permissions, checksums).
#pragma once

#include "util.hpp"

#include <gtk/gtk.h>

#include <vector>

namespace beluga {

void show_properties_dialog(GtkWidget *parent, const std::vector<Ref<GFile>> &files);

} // namespace beluga
