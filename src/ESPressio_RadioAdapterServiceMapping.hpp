#pragma once

#include <ESPressio_AdapterTypes.hpp>
#include <ESPressio_RadioServiceProfile.hpp>

namespace ESPressio::RadioAdapters {

/// <summary>Maps one runtime-neutral A2 service class to its wire-stable Radio service class explicitly.</summary>
constexpr Radio::RadioServiceClass ToRadioServiceClass(Adapters::AdapterServiceClass service) noexcept {
    switch(service) {
        case Adapters::AdapterServiceClass::Infrastructure:return Radio::RadioServiceClass::Infrastructure;
        case Adapters::AdapterServiceClass::Clock:return Radio::RadioServiceClass::Clock;
        case Adapters::AdapterServiceClass::Critical:return Radio::RadioServiceClass::Critical;
        case Adapters::AdapterServiceClass::Responsive:return Radio::RadioServiceClass::Responsive;
        case Adapters::AdapterServiceClass::Convergent:return Radio::RadioServiceClass::Convergent;
        case Adapters::AdapterServiceClass::BestEffort:return Radio::RadioServiceClass::BestEffort;
    }
    return Radio::RadioServiceClass::Invalid;
}

/// <summary>Maps one wire-stable Radio service class back to runtime-neutral A2 service semantics.</summary>
constexpr bool TryToAdapterServiceClass(
    Radio::RadioServiceClass service,
    Adapters::AdapterServiceClass& mapped) noexcept {
    switch(service) {
        case Radio::RadioServiceClass::Infrastructure:mapped=Adapters::AdapterServiceClass::Infrastructure;return true;
        case Radio::RadioServiceClass::Clock:mapped=Adapters::AdapterServiceClass::Clock;return true;
        case Radio::RadioServiceClass::Critical:mapped=Adapters::AdapterServiceClass::Critical;return true;
        case Radio::RadioServiceClass::Responsive:mapped=Adapters::AdapterServiceClass::Responsive;return true;
        case Radio::RadioServiceClass::Convergent:mapped=Adapters::AdapterServiceClass::Convergent;return true;
        case Radio::RadioServiceClass::BestEffort:mapped=Adapters::AdapterServiceClass::BestEffort;return true;
        case Radio::RadioServiceClass::Invalid:break;
    }
    return false;
}

} // namespace ESPressio::RadioAdapters
