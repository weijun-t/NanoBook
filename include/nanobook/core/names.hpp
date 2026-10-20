/*
 * names.hpp: readable names for the engine's enums ("Trade", "SelfMatch", "Buy", ...).
 *
 * What it does: One name() function per enum, returning a fixed piece of text. Used by the test
 *   helpers (to print events when a test fails) and by the differential fuzzer (to print a
 *   mismatch).
 *
 * Why it exists: The tests and the fuzzer both need the same names. Keeping them in one place
 *   means a new enum value can't be forgotten in one of them: -Wswitch warns about any value a
 *   switch doesn't handle, and -Werror makes that a build error.
 *
 * How it works: A switch per enum, returning a std::string_view that points at text stored in the
 *   program, so nothing is allocated. constexpr, so it can even run at compile time.
 *
 * Trade-offs: Only for cold paths (tests, tools, error messages). The engine itself never prints.
 */

#pragma once

#include <string_view>

#include "nanobook/core/messages.hpp"
#include "nanobook/core/types.hpp"

namespace nanobook {

constexpr std::string_view name(Side s) {
    switch (s) {
    case Side::Buy:
        return "Buy";
    case Side::Sell:
        return "Sell";
    }
    return "?";
}

constexpr std::string_view name(OrderType t) {
    switch (t) {
    case OrderType::Limit:
        return "Limit";
    case OrderType::Market:
        return "Market";
    case OrderType::IOC:
        return "IOC";
    case OrderType::FOK:
        return "FOK";
    }
    return "?";
}

constexpr std::string_view name(EventType t) {
    switch (t) {
    case EventType::New:
        return "New";
    case EventType::Trade:
        return "Trade";
    case EventType::Canceled:
        return "Canceled";
    case EventType::Replaced:
        return "Replaced";
    case EventType::Rejected:
        return "Rejected";
    }
    return "?";
}

constexpr std::string_view name(CancelReason r) {
    switch (r) {
    case CancelReason::None:
        return "None";
    case CancelReason::Requested:
        return "Requested";
    case CancelReason::SelfMatch:
        return "SelfMatch";
    case CancelReason::Unfilled:
        return "Unfilled";
    case CancelReason::FokKill:
        return "FokKill";
    }
    return "?";
}

constexpr std::string_view name(FeedResult r) {
    switch (r) {
    case FeedResult::Ok:
        return "Ok";
    case FeedResult::UnknownOrder:
        return "UnknownOrder";
    case FeedResult::DuplicateOrder:
        return "DuplicateOrder";
    case FeedResult::InvalidQty:
        return "InvalidQty";
    case FeedResult::InvalidPrice:
        return "InvalidPrice";
    case FeedResult::ReduceTooLarge:
        return "ReduceTooLarge";
    case FeedResult::BookFull:
        return "BookFull";
    }
    return "?";
}

constexpr std::string_view name(RejectReason r) {
    switch (r) {
    case RejectReason::None:
        return "None";
    case RejectReason::InvalidQty:
        return "InvalidQty";
    case RejectReason::InvalidPrice:
        return "InvalidPrice";
    case RejectReason::UnknownSymbol:
        return "UnknownSymbol";
    case RejectReason::IdsExhausted:
        return "IdsExhausted";
    case RejectReason::BookFull:
        return "BookFull";
    case RejectReason::NoLiquidity:
        return "NoLiquidity";
    case RejectReason::UnknownOrder:
        return "UnknownOrder";
    case RejectReason::TooLateToCancel:
        return "TooLateToCancel";
    case RejectReason::UseCancel:
        return "UseCancel";
    case RejectReason::NoChange:
        return "NoChange";
    }
    return "?";
}

} // namespace nanobook
