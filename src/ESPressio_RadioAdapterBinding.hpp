#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <ESPressio_AdapterProvenance.hpp>
#include <ESPressio_AdapterTransport.hpp>
#include <ESPressio_AdapterTypes.hpp>
#include <ESPressio_PrimitivePolicy.hpp>
#include <ESPressio_PrimitiveTypes.hpp>
#include <ESPressio_IRadio.hpp>

namespace ESPressio::RadioAdapters {

/// <summary>Result of resolving transport-specific immutable metadata before A2 owns direct-Radio ingress.</summary>
enum class RadioAdapterBindingResolutionStatus : std::uint8_t {
    Success=0,
    Unsupported,
    Malformed,
    Rejected,
    TemporarilyUnavailable
};

/// <summary>Fixed per-family policy resolver. It may inspect only the immutable family representation and neutral service supplied for this call.</summary>
using RadioAdapterPolicyResolverThunk=RadioAdapterBindingResolutionStatus(*)(
    void*,
    Primitive::PrimitiveProtocolVersion,
    Adapters::AdapterServiceClass,
    Adapters::AdapterByteView,
    Primitive::PrimitivePolicyDescriptor&) noexcept;

/// <summary>
/// One Radio-specific family demultiplexing binding. Family execution itself remains owned by A2's AdapterBindingTable.
/// </summary>
struct RadioAdapterBindingDescriptor final {
    Primitive::PrimitiveFamilyId Family=0;
    Primitive::PrimitiveProtocolVersionRange Protocols{};
    void* Owner=nullptr;
    RadioAdapterPolicyResolverThunk ResolvePolicy=nullptr;

    constexpr bool IsValid() const noexcept {
        return Family!=0 && Protocols.IsValid() && Protocols.Maximum!=0 && Owner!=nullptr && ResolvePolicy!=nullptr;
    }
};

/// <summary>Fixed-capacity Radio-specific family demultiplexing table, mutable only before Freeze().</summary>
template<std::size_t TMaximumBindings>
class RadioAdapterBindingRegistry final {
    static_assert(TMaximumBindings>0,"RadioAdapterBindingRegistry requires finite non-zero capacity");
    std::array<RadioAdapterBindingDescriptor,TMaximumBindings> _entries{};
    std::size_t _count=0;
    bool _frozen=false;
public:
    Adapters::AdapterRuntimeStatus Bind(const RadioAdapterBindingDescriptor& binding) noexcept {
        if(_frozen) return Adapters::AdapterRuntimeStatus::Frozen;
        if(!binding.IsValid()) return Adapters::AdapterRuntimeStatus::InvalidConfiguration;
        for(std::size_t i=0;i<_count;++i)
            if(_entries[i].Family==binding.Family) return Adapters::AdapterRuntimeStatus::DuplicateFamily;
        if(_count==_entries.size()) return Adapters::AdapterRuntimeStatus::ResourceUnavailable;
        _entries[_count++]=binding;
        return Adapters::AdapterRuntimeStatus::Success;
    }

    Adapters::AdapterRuntimeStatus Freeze() noexcept {
        if(_frozen) return Adapters::AdapterRuntimeStatus::Frozen;
        if(_count==0) return Adapters::AdapterRuntimeStatus::InvalidConfiguration;
        _frozen=true;
        return Adapters::AdapterRuntimeStatus::Success;
    }

    bool IsFrozen() const noexcept { return _frozen; }
    std::size_t Size() const noexcept { return _count; }

    const RadioAdapterBindingDescriptor* Find(
        Primitive::PrimitiveFamilyId family,
        Primitive::PrimitiveProtocolVersion protocol) const noexcept {
        if(!_frozen) return nullptr;
        for(std::size_t i=0;i<_count;++i)
            if(_entries[i].Family==family && _entries[i].Protocols.Contains(protocol)) return &_entries[i];
        return nullptr;
    }
};

/// <summary>
/// Frozen transport/security provenance resolver. Immediate peer and semantic origin are independent outputs.
/// </summary>
using RadioAdapterProvenanceResolverThunk=RadioAdapterBindingResolutionStatus(*)(
    void*,
    Radio::IRadio&,
    const Radio::RadioAddress&,
    Adapters::AdapterSemanticProvenance&,
    Adapters::AdapterRouteToken&) noexcept;

struct RadioAdapterProvenanceBinding final {
    void* Owner=nullptr;
    RadioAdapterProvenanceResolverThunk Resolve=nullptr;

    constexpr explicit operator bool() const noexcept { return Owner!=nullptr && Resolve!=nullptr; }
};

/// <summary>
/// Fixed composition-owned resolver from a semantic DeviceIdentifier to an opaque Adapter route token.
/// </summary>
/// <remarks>
/// This is intentionally distinct from the lower-transport route binding. Family APIs address semantic devices while the
/// lower Radio transport addresses provider/peer handles. A composition may map both to the same finite peer table, but
/// generic code must never pack, hash, truncate, or otherwise derive an AdapterRouteToken from DeviceIdentifier bytes.
/// </remarks>
struct RadioAdapterSemanticRouteBinding final {
    void* Owner=nullptr;
    bool (*Resolve)(void*,System::DeviceIdentifier,Adapters::AdapterRouteToken&) noexcept=nullptr;
    bool (*Validate)(void*) noexcept=nullptr;

    constexpr explicit operator bool() const noexcept { return Owner&&Resolve&&Validate; }

    bool TryResolve(System::DeviceIdentifier device,Adapters::AdapterRouteToken& route) const noexcept {
        route={};
        return *this && bool(device) && Resolve(Owner,device,route) && bool(route);
    }

    bool IsValid() const noexcept { return *this && Validate(Owner); }
};

} // namespace ESPressio::RadioAdapters
