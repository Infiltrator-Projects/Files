// SPDX-License-Identifier: GPL-3.0-or-later
#include "browse_session.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace infiltrator::files {

BrowseTabState::BrowseTabState(Location initial)
    : history{std::move(initial)}
{
}

const Location &BrowseTabState::current_location() const
{
    if (history.empty() || history_index >= history.size()) {
        throw std::logic_error("Browse tab has no current location");
    }
    return history[history_index];
}

bool BrowseTabState::can_go_back() const noexcept
{
    return history_index > 0 && history_index < history.size();
}

bool BrowseTabState::can_go_forward() const noexcept
{
    return !history.empty() && history_index + 1 < history.size();
}

void BrowseTabState::navigate_to(Location location)
{
    if (history_index + 1 < history.size()) {
        history.erase(history.begin() + static_cast<std::ptrdiff_t>(history_index + 1),
                      history.end());
    }
    history.push_back(std::move(location));
    history_index = history.size() - 1;
    selected_names.clear();
    primary_selection.reset();
    scroll_position = 0.0;
}

bool BrowseTabState::go_back() noexcept
{
    if (!can_go_back()) {
        return false;
    }
    --history_index;
    selected_names.clear();
    primary_selection.reset();
    scroll_position = 0.0;
    return true;
}

bool BrowseTabState::go_forward() noexcept
{
    if (!can_go_forward()) {
        return false;
    }
    ++history_index;
    selected_names.clear();
    primary_selection.reset();
    scroll_position = 0.0;
    return true;
}

BrowseSessionState::BrowseSessionState(Location initial, const std::size_t closed_limit)
    : closed_limit_(closed_limit)
{
    open(std::move(initial));
}

std::optional<std::size_t> BrowseSessionState::active_index() const noexcept
{
    return active_index_;
}

bool BrowseSessionState::activate(const std::size_t index) noexcept
{
    if (index >= tabs_.size()) {
        return false;
    }
    active_index_ = index;
    return true;
}

std::size_t BrowseSessionState::open(Location location, const bool make_active)
{
    const std::size_t index = tabs_.size();
    tabs_.emplace_back(std::move(location));
    if (make_active || !active_index_.has_value()) {
        active_index_ = index;
    }
    return index;
}

std::optional<std::size_t> BrowseSessionState::duplicate(const std::size_t index,
                                                         const bool make_active)
{
    if (index >= tabs_.size()) {
        return std::nullopt;
    }
    BrowseTabState copy = tabs_[index];
    return insert(std::move(copy), index + 1, make_active);
}

bool BrowseSessionState::close(const std::size_t index, const bool remember)
{
    if (index >= tabs_.size()) {
        return false;
    }
    BrowseTabState removed = std::move(tabs_[index]);
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
    normalize_active_after_removal(index);

    if (remember && closed_limit_ > 0) {
        closed_.push_front(std::move(removed));
        while (closed_.size() > closed_limit_) {
            closed_.pop_back();
        }
    }
    return true;
}

std::optional<std::size_t> BrowseSessionState::reopen_closed(const bool make_active)
{
    if (closed_.empty()) {
        return std::nullopt;
    }
    BrowseTabState state = std::move(closed_.front());
    closed_.pop_front();
    const std::size_t index =
        active_index_.has_value() ? std::min(*active_index_ + 1, tabs_.size()) : tabs_.size();
    return insert(std::move(state), index, make_active);
}

bool BrowseSessionState::reorder(const std::size_t from, const std::size_t to)
{
    if (from >= tabs_.size() || to >= tabs_.size()) {
        return false;
    }
    if (from == to) {
        return true;
    }

    BrowseTabState moved = std::move(tabs_[from]);
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(from));
    tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));

    if (active_index_.has_value()) {
        if (*active_index_ == from) {
            active_index_ = to;
        } else if (from < *active_index_ && *active_index_ <= to) {
            --*active_index_;
        } else if (to <= *active_index_ && *active_index_ < from) {
            ++*active_index_;
        }
    }
    return true;
}

std::optional<BrowseTabState> BrowseSessionState::take(const std::size_t index)
{
    if (index >= tabs_.size()) {
        return std::nullopt;
    }
    BrowseTabState state = std::move(tabs_[index]);
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
    normalize_active_after_removal(index);
    return state;
}

std::optional<std::size_t> BrowseSessionState::insert(BrowseTabState state,
                                                      const std::size_t index,
                                                      const bool make_active)
{
    if (index > tabs_.size()) {
        return std::nullopt;
    }
    tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(index), std::move(state));
    if (make_active || !active_index_.has_value()) {
        active_index_ = index;
    } else if (*active_index_ >= index) {
        ++*active_index_;
    }
    return index;
}

void BrowseSessionState::normalize_active_after_removal(const std::size_t removed) noexcept
{
    if (tabs_.empty()) {
        active_index_.reset();
        return;
    }
    if (!active_index_.has_value()) {
        active_index_ = 0;
        return;
    }
    if (*active_index_ > removed) {
        --*active_index_;
    } else if (*active_index_ == removed) {
        active_index_ = std::min(removed, tabs_.size() - 1);
    }
}

} // namespace infiltrator::files
