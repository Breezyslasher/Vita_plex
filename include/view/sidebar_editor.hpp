/**
 * VitaPlex - Sidebar Editor (Direction D, "inline edit mode")
 * A controller-driven editor for the navigation sidebar: reorder and
 * show/hide items in a widened sidebar panel while the app dims behind it.
 * Commits the order + hidden set to settings.cfg and rebuilds the live sidebar.
 */

#pragma once

namespace vitaplex {

class SidebarEditor {
public:
    // Push the inline sidebar editor over the current screen.
    static void open();
};

} // namespace vitaplex
