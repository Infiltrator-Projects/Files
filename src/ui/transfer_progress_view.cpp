// SPDX-License-Identifier: GPL-3.0-or-later
#include "transfer_progress_view.hpp"
#include <infiltratr/format.h>
#include <algorithm>
#include <string>

namespace infiltrator::files {
namespace {
std::string capacity(std::uintmax_t bytes)
{
    char buffer[64]{};
    const char *formatted = infiltratr_format_disk_capacity(bytes, buffer, sizeof buffer);
    return formatted != nullptr ? formatted : "—";
}
}
TransferProgressView::TransferProgressView(GtkWindow *window, DirectoryMutationGuard &lifetime,
                                           GtkWidget *status, const char *description)
    : lifetime_(lifetime), status_(status)
{
    if (window == nullptr) return;
    GtkWidget *header = gtk_window_get_titlebar(window);
    if (!GTK_IS_HEADER_BAR(header)) return;
    box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    bar_ = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(bar_), TRUE);
    gtk_widget_set_size_request(bar_, 170, -1);
    gtk_widget_set_tooltip_text(bar_, description);
    cancel_ = gtk_button_new_from_icon_name("process-stop-symbolic");
    gtk_widget_set_tooltip_text(cancel_, "Cancel at the next safe boundary");
    gtk_box_append(GTK_BOX(box_), bar_);
    gtk_box_append(GTK_BOX(box_), cancel_);
    gtk_widget_set_visible(box_, FALSE);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), box_);
    g_signal_connect(cancel_, "clicked", G_CALLBACK(cancel), this);
}
TransferProgressView::~TransferProgressView() { finish(); }

void TransferProgressView::start(std::shared_ptr<TransferControl> control)
{
    finish();
    if (!lifetime_.alive() || control == nullptr || box_ == nullptr) return;
    control_ = std::move(control);
    gtk_widget_set_visible(box_, TRUE);
    gtk_widget_set_sensitive(cancel_, TRUE);
    refresh();
    timer_ = g_timeout_add(100, tick, this);
}
void TransferProgressView::finish()
{
    if (timer_ != 0U) { g_source_remove(timer_); timer_ = 0U; }
    control_.reset();
    if (lifetime_.alive() && box_ != nullptr) gtk_widget_set_visible(box_, FALSE);
}
gboolean TransferProgressView::tick(gpointer data)
{
    auto *self = static_cast<TransferProgressView *>(data);
    if (!self->lifetime_.alive() || self->control_ == nullptr) {
        self->timer_ = 0U;
        return G_SOURCE_REMOVE;
    }
    self->refresh();
    return G_SOURCE_CONTINUE;
}
void TransferProgressView::cancel(GtkButton *, gpointer data)
{
    auto *self = static_cast<TransferProgressView *>(data);
    if (!self->lifetime_.alive() || self->control_ == nullptr) return;
    self->control_->request_cancel();
    gtk_widget_set_sensitive(self->cancel_, FALSE);
    if (self->status_ != nullptr)
        gtk_label_set_text(GTK_LABEL(self->status_), "Cancellation requested; stopping at the next safe boundary…");
    self->refresh();
}
void TransferProgressView::refresh()
{
    if (!lifetime_.alive() || control_ == nullptr || bar_ == nullptr) return;
    const TransferProgress p = control_->progress();
    double fraction = 0.0;
    std::string text = "Preparing…";
    const bool determinate = p.bytes_total != 0U || p.items_total != 0U;
    if (p.bytes_total != 0U) {
        fraction = static_cast<double>(p.bytes_done) / static_cast<double>(p.bytes_total);
        text = capacity(p.bytes_done) + " / " + capacity(p.bytes_total);
    } else if (p.items_total != 0U) {
        fraction = static_cast<double>(p.items_done) / static_cast<double>(p.items_total);
        text = std::to_string(p.items_done) + " / " + std::to_string(p.items_total) + " items";
    }
    if (determinate) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar_), std::clamp(fraction, 0.0, 1.0));
    else gtk_progress_bar_pulse(GTK_PROGRESS_BAR(bar_));
    if (control_->cancelled()) text = "Cancelling…";
    else if (determinate && fraction >= 1.0) text = "Finishing…";
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(bar_), text.c_str());
}
} // namespace infiltrator::files
