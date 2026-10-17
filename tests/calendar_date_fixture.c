// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Synthetic Calendar Resources module for the Files timestamp integration
 * test. Only provider ABI and language handoff are under test here; no
 * chronology algorithm is implemented by Files.
 */
#include <infiltratr/temporal_calendar.h>
#include <stdlib.h>
#include <string.h>

static const int64_t test_jdn = INT64_C(2461324);

static bool fixture_fields(
    const InfiltratrTemporalCalendarProvider *provider,
    int64_t jdn, InfiltratrTemporalCalendarFields *fields)
{
    (void)provider;
    if (jdn != test_jdn || fields == NULL) return false;
    *fields = (InfiltratrTemporalCalendarFields){ 2026, 10, 10, 0, false };
    return true;
}

static bool fixture_format(
    const InfiltratrTemporalCalendarProvider *provider,
    int64_t jdn, InfiltratrTemporalCalendarDatePart part,
    char *buffer, size_t capacity, size_t *length)
{
    const char *language = getenv("LANGUAGE");
    const char *text = language != NULL && strncmp(language, "zh", 2) == 0
        ? "中文日期" : "Chinese lunar date";
    const size_t size = strlen(text);
    (void)provider;
    if (jdn != test_jdn ||
        part != INFILTRATR_TEMPORAL_CALENDAR_DATE_PART_SHORT ||
        buffer == NULL || capacity <= size) return false;
    memcpy(buffer, text, size + 1U);
    if (length != NULL) *length = size;
    return true;
}

static bool fixture_period_start(
    const InfiltratrTemporalCalendarProvider *provider,
    int64_t jdn, int64_t *result)
{
    (void)provider;
    if (result == NULL || jdn != test_jdn) return false;
    *result = test_jdn;
    return true;
}

static bool fixture_navigate(
    const InfiltratrTemporalCalendarProvider *provider,
    int64_t jdn, int32_t amount, int64_t *result)
{
    (void)provider;
    if (result == NULL || jdn != test_jdn || amount != 0) return false;
    *result = jdn;
    return true;
}

static const InfiltratrTemporalResourceMetadata metadata = {
    sizeof(InfiltratrTemporalResourceMetadata),
    INFILTRATR_TEMPORAL_RESOURCE_METADATA_ABI,
    INFILTRATR_TEMPORAL_RESOURCE_CALENDAR,
    "chinese", "Fixture Chinese Calendar", false, false, false
};

static const InfiltratrTemporalCalendarProvider provider = {
    sizeof(InfiltratrTemporalCalendarProvider),
    INFILTRATR_TEMPORAL_CALENDAR_PROVIDER_ABI,
    "chinese",
    fixture_fields, fixture_format, fixture_period_start,
    fixture_navigate, fixture_navigate
};

#ifdef _WIN32
#define FIXTURE_EXPORT __declspec(dllexport)
#else
#define FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

FIXTURE_EXPORT const InfiltratrTemporalResourceMetadata *
infiltratr_temporal_resource_metadata_v1(void);
FIXTURE_EXPORT const InfiltratrTemporalCalendarProvider *
infiltratr_temporal_calendar_provider_v1(void);

FIXTURE_EXPORT const InfiltratrTemporalResourceMetadata *
infiltratr_temporal_resource_metadata_v1(void) { return &metadata; }
FIXTURE_EXPORT const InfiltratrTemporalCalendarProvider *
infiltratr_temporal_calendar_provider_v1(void) { return &provider; }
