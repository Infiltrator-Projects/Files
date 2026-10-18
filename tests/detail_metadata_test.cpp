// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/ui/detail_metadata.hpp"

#include <infiltratr/temporal.h>
#include <infiltratr/temporal_clock_resource.h>
#include <infiltratr/temporal_posix.h>

#include <cstdio>
#include <cstring>
#include <string>

using infiltrator::files::detail_compare_modified;
using infiltrator::files::detail_compare_name;
using infiltrator::files::detail_compare_size;
using infiltrator::files::detail_compare_type;
using infiltrator::files::detail_locale_time_format_without_seconds;
using infiltrator::files::detail_modified_text;
using infiltrator::files::detail_size_text;
using infiltrator::files::detail_type_text;

// Preserve named distinct failure codes and print one actionable diagnostic
// when a temporal resource regression occurs on a headless CI runner.
static int detail_metadata_failure(int code)
{
    std::fprintf(stderr, "detail-metadata assertion failed (code %d)\n", code);
    return code;
}

static bool matches_clock(const std::string &visible,const char *date,
    const char *id,const char *expected)
{
    InfiltratrTemporalClockModule *module=infiltratr_temporal_clock_module_open_id(id);
    const bool installed=module != nullptr;
    infiltratr_temporal_clock_module_free(module);
    return visible == std::string(date)+" "+
        (installed ? expected : "clock resource unavailable");
}

int main()
{
    const std::string invalid_utf8("%H:%M:%S\xff");
    if (!detail_locale_time_format_without_seconds(invalid_utf8, "").empty() ||
        !detail_locale_time_format_without_seconds("%r", invalid_utf8).empty()) {
        return detail_metadata_failure(18);
    }
    // Exact civil-time/branch checks must not depend on the runner's timezone.
    g_setenv("TZ", "UTC", TRUE);
    g_setenv("LANGUAGE", "en_AU", TRUE);
    GFileInfo *directory = g_file_info_new();
    g_file_info_set_file_type(directory, G_FILE_TYPE_DIRECTORY);
    const bool directory_ok = detail_type_text(directory) == "Folder" &&
                              detail_size_text(directory) == "—" &&
                              detail_modified_text(directory) == "—";
    g_object_unref(directory);
    if (!directory_ok) {
        return detail_metadata_failure(1);
    }

    GFileInfo *file = g_file_info_new();
    g_file_info_set_file_type(file, G_FILE_TYPE_REGULAR);
    g_file_info_set_name(file, "small.txt");
    g_file_info_set_display_name(file, "small.txt");
    if (detail_type_text(file) != "File") {
        g_object_unref(file);
        return detail_metadata_failure(2);
    }

    g_file_info_set_size(file, 1536);
    if (detail_size_text(file) != "1.5 KB") {
        g_object_unref(file);
        return detail_metadata_failure(3);
    }

    // Put the shared temporal policy in an isolated test config before the
    // first timestamp formatting call. System Settings and Files consume this
    // same Common-owned policy document.
    GError *temp_error = nullptr;
    gchar *config_home = g_dir_make_tmp("infiltrator-files-temporal-XXXXXX", &temp_error);
    if (config_home == nullptr) {
        if (temp_error != nullptr) {
            g_error_free(temp_error);
        }
        g_object_unref(file);
        return detail_metadata_failure(4);
    }
    g_setenv("XDG_CONFIG_HOME", config_home, TRUE);

    InfiltratrTemporalPolicyV3 policy{};
    if (!infiltratr_temporal_policy_v3_default(&policy)) {
        g_free(config_home);
        g_object_unref(file);
        return detail_metadata_failure(5);
    }
    std::strcpy(policy.clock_mode, "standard-12");
    policy.show_seconds = false;
    if (infiltratr_temporal_posix_policy_save(&policy) != 0) {
        g_free(config_home);
        g_object_unref(file);
        return detail_metadata_failure(6);
    }

    g_file_info_set_attribute_uint64(file, G_FILE_ATTRIBUTE_TIME_MODIFIED, 46800U);
    const std::string modified = detail_modified_text(file);
    InfiltratrTemporalClockModule *civil_clock =
        infiltratr_temporal_clock_module_open_id("standard-12");
    const bool civil_ok = civil_clock != nullptr
        ? (modified.find("AM") != std::string::npos ||
           modified.find("PM") != std::string::npos)
        : modified.find("clock resource unavailable") != std::string::npos;
    infiltratr_temporal_clock_module_free(civil_clock);
    if (modified.empty() || modified == "—" || !civil_ok) {
        g_free(config_home);
        g_object_unref(file);
        return detail_metadata_failure(7);
    }

    GFileInfo *larger = g_file_info_new();
    g_file_info_set_file_type(larger, G_FILE_TYPE_REGULAR);
    g_file_info_set_name(larger, "large.txt");
    g_file_info_set_display_name(larger, "large.txt");
    g_file_info_set_size(larger, 4096);
    g_file_info_set_attribute_uint64(larger, G_FILE_ATTRIBUTE_TIME_MODIFIED, 86400U);

    if (detail_compare_size(file, larger) >= 0 ||
        detail_compare_size(larger, file) <= 0 ||
        detail_compare_modified(file, larger) >= 0 ||
        detail_compare_modified(larger, file) <= 0 ||
        detail_compare_name(larger, file) >= 0) {
        g_object_unref(larger);
        g_free(config_home);
        g_object_unref(file);
        return detail_metadata_failure(8);
    }


    GFileInfo *number2 = g_file_info_new();
    GFileInfo *number10 = g_file_info_new();
    g_file_info_set_name(number2, "file2.txt");
    g_file_info_set_display_name(number2, "file2.txt");
    g_file_info_set_name(number10, "file10.txt");
    g_file_info_set_display_name(number10, "file10.txt");
    if (detail_compare_name(number2, number10) >= 0 ||
        detail_compare_name(number10, number2) <= 0) {
        g_object_unref(number2);
        g_object_unref(number10);
        g_free(config_home);
        g_object_unref(file);
        return detail_metadata_failure(35);
    }
    g_object_unref(number2);
    g_object_unref(number10);

    // Equal primary values must remain equal so GtkColumnViewSorter can consult
    // a secondary sort column. Modified-time ordering also includes subsecond
    // metadata when the filesystem exposes it.
    g_file_info_set_size(larger, 1536);
    g_file_info_set_attribute_uint64(
        larger, G_FILE_ATTRIBUTE_TIME_MODIFIED, 46800U);
    g_file_info_set_attribute_uint32(file, "time::modified-nsec", 100U);
    g_file_info_set_attribute_uint32(larger, "time::modified-nsec", 900U);
    if (detail_compare_type(file, larger) != 0 ||
        detail_compare_size(file, larger) != 0 ||
        detail_compare_modified(file, larger) >= 0) {
        g_object_unref(larger);
        g_free(config_home);
        g_object_unref(file);
        return detail_metadata_failure(14);
    }
    g_file_info_set_attribute_uint32(larger, "time::modified-nsec", 100U);
    if (detail_compare_modified(file, larger) != 0) {
        g_object_unref(larger);
        g_free(config_home);
        g_object_unref(file);
        return detail_metadata_failure(15);
    }
    g_object_unref(larger);

    // A followed symlink may report the target's regular/directory type. The
    // explicit is-symlink attribute must win in the analytical Type column.
    g_file_info_set_is_symlink(file, TRUE);
    if (detail_type_text(file) != "Symbolic Link") {
        g_free(config_home);
        g_object_unref(file);
        return detail_metadata_failure(9);
    }

    // The locale-owned standard clock must still obey System Settings'
    // seconds toggle. If this locale's %X includes seconds, the rendered
    // standard timestamp must no longer end in that unmodified %X value.
    std::strcpy(policy.clock_mode, "standard");
    policy.show_seconds = false;
    if (infiltratr_temporal_posix_policy_save(&policy) != 0) {
        g_free(config_home);
        g_object_unref(file);
        return detail_metadata_failure(10);
    }
    g_file_info_set_attribute_uint64(file, G_FILE_ATTRIBUTE_TIME_MODIFIED, 46837U);
    const std::string standard_without_seconds = detail_modified_text(file);
    GDateTime *standard_local = g_date_time_new_from_unix_local(46837);
    if (standard_local == nullptr) {
        g_free(config_home);
        g_object_unref(file);
        return detail_metadata_failure(11);
    }
    char *locale_clock = g_date_time_format(standard_local, "%X");
    char *locale_second = g_date_time_format(standard_local, "%S");
    const bool locale_shows_seconds =
        locale_clock != nullptr && locale_second != nullptr &&
        locale_second[0] != '\0' && std::strstr(locale_clock, locale_second) != nullptr;
    const std::string locale_clock_text = locale_clock != nullptr ? locale_clock : "";
    const bool still_uses_unmodified_locale_clock = locale_shows_seconds &&
        standard_without_seconds.size() >= locale_clock_text.size() &&
        standard_without_seconds.compare(
            standard_without_seconds.size() - locale_clock_text.size(),
            locale_clock_text.size(), locale_clock_text) == 0;
    g_free(locale_clock);
    g_free(locale_second);
    g_date_time_unref(standard_local);
    if (standard_without_seconds.empty() || standard_without_seconds == "—" ||
        still_uses_unmodified_locale_clock) {
        g_free(config_home);
        g_object_unref(file);
        return detail_metadata_failure(12);
    }

    // Exercise locale pattern reduction directly so qualification does not
    // depend on optional locales being installed on the CI host. Field order,
    // AM/PM placement, separators and attached localized units must survive.
    const bool locale_patterns_ok =
        detail_locale_time_format_without_seconds("%H:%M:%S", "") ==
            "%H:%M" &&
        detail_locale_time_format_without_seconds("%I:%M:%S %p", "") ==
            "%I:%M %p" &&
        detail_locale_time_format_without_seconds("%p %I:%M:%S", "") ==
            "%p %I:%M" &&
        detail_locale_time_format_without_seconds("%H時%M分%S秒", "") ==
            "%H時%M分" &&
        detail_locale_time_format_without_seconds("%p %I時%M分%S秒", "") ==
            "%p %I時%M分" &&
        detail_locale_time_format_without_seconds("%r", "%I:%M:%S %p") ==
            "%I:%M %p" &&
        detail_locale_time_format_without_seconds("%T", "") == "%H:%M" &&
        detail_locale_time_format_without_seconds("%H:%M:%OS", "") ==
            "%H:%M" &&
        detail_locale_time_format_without_seconds("%H:%M:%S Uhr", "") ==
            "%H:%M Uhr";
    if (!locale_patterns_ok) {
        g_free(config_home);
        g_object_unref(file);
        return detail_metadata_failure(13);
    }

    // The shared calendar bridge receives the file's local civil date, while
    // Common renders its clock. A rejected chronology must not silently become
    // Gregorian, and changing presentation never changes raw timestamp sort.
    InfiltratrTemporalDateProvider *date_provider =
        infiltratr_temporal_posix_date_provider_new_from(CALENDAR_DATE_FIXTURE_PATH);
    if (date_provider == nullptr) {
        g_free(config_home);
        g_object_unref(file);
        return detail_metadata_failure(16);
    }
    std::strcpy(policy.calendar, "chinese");
    std::strcpy(policy.clock_mode, "chinese-time");
    policy.show_seconds = false;
    g_file_info_set_attribute_uint64(file, G_FILE_ATTRIBUTE_TIME_MODIFIED, 1791646200U);
    const bool saved_coarse = infiltratr_temporal_posix_policy_save(&policy) == 0;
    const std::string coarse_text = detail_modified_text(file, date_provider);
    const bool coarse_ok = saved_coarse &&
        matches_clock(coarse_text,"Chinese lunar date","chinese-time","Shēn hour");
    policy.show_seconds = true;
    const bool saved_fine = infiltratr_temporal_posix_policy_save(&policy) == 0;
    const std::string fine_text = detail_modified_text(file, date_provider);
    const bool fine_ok = saved_fine &&
        matches_clock(fine_text,"Chinese lunar date","chinese-time","Shēn, first half");
    g_setenv("LANGUAGE", "zh_TW", TRUE);
    const std::string chinese_text = detail_modified_text(file, date_provider);
    const bool chinese_language_ok =
        matches_clock(chinese_text,"中文日期","chinese-time","申初");
    std::strcpy(policy.clock_mode, "standard-12");
    policy.show_seconds = false;
    const bool saved_twelve = infiltratr_temporal_posix_policy_save(&policy) == 0;
    InfiltratrTemporalClockModule *twelve_clock =
        infiltratr_temporal_clock_module_open_id("standard-12");
    const std::string twelve_text = detail_modified_text(file, date_provider);
    bool twelve_language_ok = false;
    if (saved_twelve && twelve_clock != nullptr) {
        char native[128]{};
        twelve_language_ok = infiltratr_temporal_clock_format(
            twelve_clock, "zh_TW", INT64_C(1791646200000000), 0,
            false, false, 0.0, 0.0, native, sizeof(native), nullptr) &&
            twelve_text == std::string("中文日期 ") + native;
    } else if (saved_twelve) {
        twelve_language_ok = twelve_text.find("clock resource unavailable") !=
            std::string::npos;
    }
    infiltratr_temporal_clock_module_free(twelve_clock);
    g_setenv("LANGUAGE", "en_AU", TRUE);
    std::strcpy(policy.calendar, "roman");
    std::strcpy(policy.clock_mode, "chinese-time");
    policy.show_seconds = true;
    const bool saved_unavailable = infiltratr_temporal_posix_policy_save(&policy) == 0;
    const std::string unavailable_text = detail_modified_text(file, date_provider);
    const bool unavailable_ok = saved_unavailable &&
        matches_clock(unavailable_text,"calendar resource unavailable","chinese-time","Shēn, first half");
    bool edo_ok = true;
    policy.location_configured = true;
    policy.latitude = 0.0;
    policy.longitude = 0.0;
    policy.show_seconds = false;
    g_file_info_set_attribute_uint64(file, G_FILE_ATTRIBUTE_TIME_MODIFIED, 946728000U);
    for (const char *mode : {"japanese-temporal", "japanese-temporal-early"}) {
        std::strcpy(policy.clock_mode, mode);
        edo_ok = edo_ok && infiltratr_temporal_posix_policy_save(&policy) == 0 &&
            matches_clock(detail_modified_text(file,date_provider),"calendar resource unavailable",mode,"Mi · 四 bells");
    }
    infiltratr_temporal_posix_date_provider_free(date_provider);
    if (!coarse_ok || !fine_ok || !chinese_language_ok || !twelve_language_ok ||
        !unavailable_ok || !edo_ok) {
        g_free(config_home);
        g_object_unref(file);
        std::fprintf(stderr,
            "clock fixtures coarse=%d (%s), fine=%d (%s), zh=%d (%s), "
            "12h=%d (%s), invalid=%d (%s), edo=%d\n",
            (int)coarse_ok, coarse_text.c_str(), (int)fine_ok,
            fine_text.c_str(), (int)chinese_language_ok, chinese_text.c_str(),
            (int)twelve_language_ok, twelve_text.c_str(), (int)unavailable_ok,
            unavailable_text.c_str(), (int)edo_ok);
        return detail_metadata_failure(17);
    }

    // All explicit clocks require their installed modules. No
    // Julian/MJD or other former compatibility formatter may take over.
    std::strcpy(policy.calendar, "gregorian");
    policy.show_seconds = true;
    policy.location_configured = true;
    policy.latitude = 0.0;
    policy.longitude = 0.0;
    g_file_info_set_attribute_uint64(file, G_FILE_ATTRIBUTE_TIME_MODIFIED, 43200U);
    for (const char *mode : {"decimal", "internet", "unix", "hexadecimal",
                             "binary", "julian", "modified-julian",
                             "standard-12", "standard-24",
                             "chinese-ke", "mean-solar", "sidereal", "solar", "chinese-time",
                             "roman-temporal", "japanese-temporal", "japanese-temporal-early",
                             "italian-hours", "babylonian-hours", "babylonian-ancient",
                             "indian-ghati", "nuremberg-hours", "nuremberg-solar"}) {
        std::strcpy(policy.clock_mode, mode);
        if (infiltratr_temporal_posix_policy_save(&policy) != 0) {
            g_free(config_home);
            g_object_unref(file);
            return detail_metadata_failure(19);
        }
        const std::string visible = detail_modified_text(file);
        InfiltratrTemporalClockModule *resource =
            infiltratr_temporal_clock_module_open_id(mode);
        bool valid = false;
        if (resource != nullptr) {
            char expected[128]{};
            std::size_t length = 0U;
            valid = infiltratr_temporal_clock_format(
                        resource, "en_AU", INT64_C(43200000000), 0,
                        true, false, 0.0, 0.0, expected, sizeof(expected),
                        &length) &&
                length != 0U && visible.size() >= length &&
                visible.compare(visible.size() - length, length, expected) == 0;
        } else {
            valid = visible.find("clock resource unavailable") !=
                std::string::npos;
        }
        infiltratr_temporal_clock_module_free(resource);
        if (!valid) {
            g_free(config_home);
            g_object_unref(file);
            return detail_metadata_failure(19);
        }
    }

    g_free(config_home);
    g_object_unref(file);
    return 0;
}
