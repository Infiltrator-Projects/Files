// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../core/location.hpp"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace infiltrator::files {

struct BrowseTabState {
    std::vector<Location> history;
    std::size_t history_index{0};
    std::vector<std::string> selected_names;
    std::optional<std::string> primary_selection;
    double scroll_position{0.0};

    explicit BrowseTabState(Location initial);

    [[nodiscard]] const Location &current_location() const;
    [[nodiscard]] bool can_go_back() const noexcept;
    [[nodiscard]] bool can_go_forward() const noexcept;
    void navigate_to(Location location);
    bool go_back() noexcept;
    bool go_forward() noexcept;
};

class BrowseSessionState final {
public:
    explicit BrowseSessionState(Location initial, std::size_t closed_limit = 12);

    [[nodiscard]] const std::vector<BrowseTabState> &tabs() const noexcept { return tabs_; }
    [[nodiscard]] std::optional<std::size_t> active_index() const noexcept;
    [[nodiscard]] std::size_t closed_count() const noexcept { return closed_.size(); }

    bool activate(std::size_t index) noexcept;
    std::size_t open(Location location, bool activate = true);
    std::optional<std::size_t> duplicate(std::size_t index, bool activate = true);
    bool close(std::size_t index, bool remember = true);
    std::optional<std::size_t> reopen_closed(bool activate = true);
    bool reorder(std::size_t from, std::size_t to);
    std::optional<BrowseTabState> take(std::size_t index);
    std::optional<std::size_t> insert(BrowseTabState state,
                                      std::size_t index,
                                      bool activate = true);

private:
    void normalize_active_after_removal(std::size_t removed) noexcept;

    std::vector<BrowseTabState> tabs_;
    std::deque<BrowseTabState> closed_;
    std::optional<std::size_t> active_index_;
    std::size_t closed_limit_{12};
};

} // namespace infiltrator::files
