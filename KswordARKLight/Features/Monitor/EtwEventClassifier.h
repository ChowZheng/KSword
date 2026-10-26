#pragma once

#include "EtwEventModel.h"

#include <cstdint>

namespace Ksword::Features::Monitor {

// EtwCategory groups events by the subsystem they touch, so a live capture can
// be narrowed to one kind of activity without inventing a query string.
//
// The classification is keyword based and deliberately conservative: an event
// lands in Other unless the provider name or the summary clearly identifies a
// subsystem. For an investigation tool a wrong category is worse than an
// unspecific one, because the user filters by it and stops looking elsewhere.
enum class EtwCategory : int {
    All = 0,
    Process = 1,
    Thread = 2,
    Image = 3,
    File = 4,
    Registry = 5,
    Network = 6,
    Security = 7,
    Other = 8
};

// ClassifyEtwEvent derives the category of one event. Input is a captured event
// row; processing inspects the provider name first (the most reliable signal)
// and the summary second; output is never All.
EtwCategory ClassifyEtwEvent(const EtwEvent& eventRow);

// EtwCategoryText returns the Chinese caption for a category. Input is one
// category; output is a static string, and All maps to "全部".
const wchar_t* EtwCategoryText(EtwCategory category);

// IsCategorySelectable reports whether a value can be used as a filter choice.
// Input is one category; output is false only for All, which is the "no filter"
// state rather than a classification an event can have.
bool IsCategorySelectable(EtwCategory category);

// EtwLevelText returns the standard ETW level caption for one level number.
// Inputs are the raw level byte; output is a static Chinese string, with
// anything outside 1..5 labelled as the numeric value's fallback "其他".
const wchar_t* EtwLevelText(std::uint8_t level);

// EtwLevelFilterMatches decides whether one event passes a level filter.
// Inputs are the combo selection (0 means every level) and the event level;
// output is true when the event should stay visible.
bool EtwLevelFilterMatches(int levelFilterIndex, std::uint8_t level);

// EtwCategoryFilterMatches decides whether one event passes a category filter.
// Inputs are the selected category and the event; output is true when the event
// should stay visible.
bool EtwCategoryFilterMatches(EtwCategory filter, const EtwEvent& eventRow);

} // namespace Ksword::Features::Monitor
