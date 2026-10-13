// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/ui/browse_session.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace infiltrator::files;

namespace {

void require(const bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Location location(std::string uri)
{
    return Location{std::move(uri), {}, LocationKind::Directory};
}

} // namespace

int main()
{
    try {
        BrowseSessionState session(location("file:///home"));
        require(session.tabs().size() == 1 && session.active_index() == 0,
                "A session must start with one active tab");

        session.tabs()[0].current_location();
        auto documents = session.open(location("file:///documents"));
        require(documents == 1 && session.active_index() == 1,
                "Opening a tab must activate the new location");

        auto duplicate = session.duplicate(1);
        require(duplicate == 2 && session.tabs().size() == 3,
                "Duplicate must insert beside the source tab");
        require(session.tabs()[2].current_location().uri == "file:///documents",
                "Duplicate must preserve location");

        require(session.reorder(2, 0) && session.active_index() == 0,
                "Reordering must preserve active-tab identity");
        BrowseTabState *active = session.tab(0);
        require(active != nullptr, "Active tab state must exist");
        active->navigate_to(location("file:///documents/work"));
        active->selected_names = {"report.odt"};
        active->primary_selection = "report.odt";
        active->scroll_position = 72.0;

        auto moved = session.take(0);
        require(moved.has_value() && session.tabs().size() == 2,
                "Taking a tab must transfer its state without closing history");
        BrowseSessionState other(location("file:///tmp"));
        auto inserted = other.insert(std::move(*moved), 1);
        require(inserted == 1 && other.active_index() == 1,
                "Moved tab must have exactly one new owner");
        require(other.tabs()[1].current_location().uri == "file:///documents/work" &&
                    other.tabs()[1].primary_selection == "report.odt" &&
                    other.tabs()[1].scroll_position == 72.0,
                "Moving between windows must preserve browsing context");

        require(other.close(1) && other.closed_count() == 1,
                "Closing must record one restorable snapshot");
        auto reopened = other.reopen_closed();
        require(reopened == 1 && other.tabs()[1].current_location().uri == "file:///documents/work",
                "Reopen must restore the most recently closed context");
        require(other.tabs()[1].primary_selection == "report.odt",
                "Reopen must restore selection context");

        BrowseSessionState bounded(location("file:///one"), 2);
        bounded.open(location("file:///two"));
        bounded.open(location("file:///three"));
        require(bounded.close(2) && bounded.close(1) && bounded.close(0),
                "Every tab, including the final tab, must be closable by the model");
        require(bounded.tabs().empty() && !bounded.active_index().has_value(),
                "Closing the final tab must leave window-close policy to the UI");
        require(bounded.closed_count() == 2,
                "Recently closed history must remain bounded");
        require(bounded.reopen_closed().has_value() &&
                    bounded.tabs()[0].current_location().uri == "file:///one",
                "Reopen order must be most-recent-first");

        BrowseTabState history(location("file:///a"));
        history.navigate_to(location("file:///b"));
        history.navigate_to(location("file:///c"));
        require(history.go_back() && history.current_location().uri == "file:///b",
                "Back must restore the previous location");
        history.navigate_to(location("file:///d"));
        require(!history.can_go_forward() && history.history.size() == 3,
                "New navigation must discard forward history");

        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
