#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ESPressio_AdapterBinding.hpp>
#include <ESPressio_EventRuntime.hpp>
#include <ESPressio_EventWire.hpp>

#include "ESPressio_RadioAdapterBinding.hpp"

namespace ESPressio::RadioAdapters {

enum class EventRadioAdapterBindingStatus : std::uint8_t {
    Success=0,
    Frozen,
    DuplicateType,
    ResourceUnavailable,
    InvalidBinding,
    InvalidService
};

struct EventRadioAdapterOutboundSource final {
    Event::EventTypeId TypeId{};
    const Event::EventLease* Occurrence{nullptr};
};

namespace EventRadioDetail {
constexpr Adapters::AdapterResourceStatus MapWireEncode(Event::EventWireStatus status) noexcept {
    using S=Event::EventWireStatus;
    switch(status) {
        case S::Success: return Adapters::AdapterResourceStatus::Success;
        case S::InsufficientOutput:
        case S::PayloadTooLarge: return Adapters::AdapterResourceStatus::TooLarge;
        case S::InvalidHeader:
        case S::UnsupportedProtocol:
        case S::UnknownType:
        case S::InvalidLength:
        case S::SchemaOrDecodeFailure: return Adapters::AdapterResourceStatus::InvalidConfiguration;
    }
    return Adapters::AdapterResourceStatus::InvalidConfiguration;
}
}

/// <summary>Frozen Event family binding shared by A2 execution and direct-Radio ingress policy validation.</summary>
/// <remarks>Event owns wire/admission/idempotency/source-loop semantics; this class owns only fixed transport composition facts.</remarks>
template<std::size_t TMaximumTypes>
class EventRadioAdapterFamilyBinding final {
    static_assert(TMaximumTypes>0,"Event RadioAdapter Type capacity must be non-zero");
    using EncodeThunk=Event::EventWireResult(*)(const Event::EventLease&,std::uint8_t*,std::size_t);

    struct Entry final {
        Event::EventTypeId TypeId{};
        Event::Runtime* Runtime{nullptr};
        Event::EventInboundBinding Inbound{};
        Primitive::PrimitivePolicyDescriptor Policy{};
        Adapters::AdapterServiceClass Service{Adapters::AdapterServiceClass::BestEffort};
        std::size_t MaximumWireBytes{0};
        EncodeThunk Encode{nullptr};
        bool Used{false};
    };

    std::array<Entry,TMaximumTypes> _entries{};
    std::size_t _count{0};
    std::size_t _maximumInboundBytes{0};
    std::size_t _maximumOutboundBytes{0};
    std::uint8_t _serviceMask{0};
    bool _requiresDestinationEvidence{false};
    bool _frozen{false};

    static constexpr bool IsService(Adapters::AdapterServiceClass service) noexcept {
        return static_cast<std::uint8_t>(service)<Adapters::AdapterServiceClassCount;
    }
    static constexpr std::uint8_t ServiceBit(Adapters::AdapterServiceClass service) noexcept {
        return static_cast<std::uint8_t>(std::uint8_t{1}<<static_cast<std::uint8_t>(service));
    }
    const Entry* Find(Event::EventTypeId type) const noexcept {
        for(std::size_t i=0;i<_count;++i) if(_entries[i].Used&&_entries[i].TypeId==type) return &_entries[i];
        return nullptr;
    }

    static Primitive::PrimitiveAdmissionDisposition AdmitInbound(
        void* owner,Primitive::PrimitiveProtocolVersion protocol,Adapters::AdapterByteView bytes,
        const Adapters::AdapterSemanticProvenance&) noexcept {
        auto& self=*static_cast<EventRadioAdapterFamilyBinding*>(owner);
        if(!self._frozen||protocol!=Event::EventProtocolVersion||!bytes.Data||bytes.Size<Event::EventWireHeaderSize)
            return Primitive::PrimitiveAdmissionDisposition::Malformed;
        Event::EventWireHeader header{};
        const auto parsed=Event::DecodeEventWireHeader(bytes.Data,bytes.Size,header);
        if(!parsed) return parsed.Status==Event::EventWireStatus::UnsupportedProtocol
            ?Primitive::PrimitiveAdmissionDisposition::Unsupported
            :Primitive::PrimitiveAdmissionDisposition::Malformed;
        const auto* entry=self.Find(header.Key.TypeId);
        if(!entry||!entry->Runtime||!entry->Inbound) return Primitive::PrimitiveAdmissionDisposition::Unsupported;
        if(bytes.Size>entry->MaximumWireBytes) return Primitive::PrimitiveAdmissionDisposition::Malformed;
        return Event::ToPrimitiveAdmissionDisposition(
            entry->Runtime->TryAdmitRemote(entry->Inbound,bytes.Data,bytes.Size).Status);
    }

    static Adapters::AdapterEncodeResult EncodeOutbound(
        void* owner,Primitive::PrimitiveProtocolVersion protocol,std::uint64_t,
        const void* source,Adapters::AdapterMutableByteView output) noexcept {
        auto& self=*static_cast<EventRadioAdapterFamilyBinding*>(owner);
        if(!self._frozen||protocol!=Event::EventProtocolVersion||!source||!output.Data)
            return {Adapters::AdapterResourceStatus::InvalidConfiguration,0};
        const auto& outbound=*static_cast<const EventRadioAdapterOutboundSource*>(source);
        const auto* entry=self.Find(outbound.TypeId);
        if(!entry||!entry->Encode||!outbound.Occurrence||!static_cast<bool>(*outbound.Occurrence)||
           outbound.Occurrence->Facts().TypeId!=entry->TypeId)
            return {Adapters::AdapterResourceStatus::InvalidConfiguration,0};
        const auto encoded=entry->Encode(*outbound.Occurrence,output.Data,output.Capacity);
        return {EventRadioDetail::MapWireEncode(encoded.Status),encoded.Bytes};
    }

    static RadioAdapterBindingResolutionStatus ResolveRadioPolicy(
        void* owner,Primitive::PrimitiveProtocolVersion protocol,Adapters::AdapterServiceClass service,
        Adapters::AdapterByteView bytes,Primitive::PrimitivePolicyDescriptor& policy) noexcept {
        auto& self=*static_cast<EventRadioAdapterFamilyBinding*>(owner);
        if(!self._frozen) return RadioAdapterBindingResolutionStatus::Rejected;
        if(protocol!=Event::EventProtocolVersion) return RadioAdapterBindingResolutionStatus::Unsupported;
        if(!bytes.Data||bytes.Size<Event::EventWireHeaderSize) return RadioAdapterBindingResolutionStatus::Malformed;
        Event::EventWireHeader header{};
        const auto parsed=Event::DecodeEventWireHeader(bytes.Data,bytes.Size,header);
        if(!parsed) return parsed.Status==Event::EventWireStatus::UnsupportedProtocol
            ?RadioAdapterBindingResolutionStatus::Unsupported:RadioAdapterBindingResolutionStatus::Malformed;
        const auto* entry=self.Find(header.Key.TypeId);
        if(!entry) return RadioAdapterBindingResolutionStatus::Unsupported;
        if(service!=entry->Service) return RadioAdapterBindingResolutionStatus::Rejected;
        if(bytes.Size>entry->MaximumWireBytes) return RadioAdapterBindingResolutionStatus::Malformed;
        policy=entry->Policy;
        return RadioAdapterBindingResolutionStatus::Success;
    }

public:
    template<class TEvent,class TFormat>
    EventRadioAdapterBindingStatus BindType(Event::Runtime& runtime,Adapters::AdapterServiceClass service) noexcept {
        static_assert(TEvent::IsTransmissibleEvent&&TEvent::ValidateTier(),"Radio Event binding requires a Transmissible Event");
        static_assert(std::is_same_v<TFormat,Serializable::DirectBinary>||
                      std::is_same_v<TFormat,Serializable::CBOR>||
                      std::is_same_v<TFormat,Serializable::JSON>,"Unsupported Event RadioAdapter format");
        if(_frozen) return EventRadioAdapterBindingStatus::Frozen;
        if(!IsService(service)) return EventRadioAdapterBindingStatus::InvalidService;
        if(Find(TEvent::TypeId)) return EventRadioAdapterBindingStatus::DuplicateType;
        if(_count==_entries.size()) return EventRadioAdapterBindingStatus::ResourceUnavailable;
        const auto inbound=runtime.template BindInbound<TEvent,TFormat>();
        if(!inbound) return EventRadioAdapterBindingStatus::InvalidBinding;
        constexpr auto maximum=Event::MaximumCompletePrimitiveWireBytes<TEvent,TFormat>;
        auto& entry=_entries[_count++];
        entry.TypeId=TEvent::TypeId;
        entry.Runtime=&runtime;
        entry.Inbound=inbound;
        entry.Policy=Primitive::PrimitivePolicyContract<typename TEvent::DeliveryPolicy>::Descriptor();
        entry.Service=service;
        entry.MaximumWireBytes=maximum;
        entry.Encode=[](const Event::EventLease& occurrence,std::uint8_t* output,std::size_t capacity) {
            if(!occurrence||occurrence.Facts().TypeId!=TEvent::TypeId) return Event::EventWireResult{};
            return Event::EncodeEventWire<TEvent,TFormat>(occurrence.template Get<TEvent>(),output,capacity);
        };
        entry.Used=true;
        if(maximum>_maximumInboundBytes) _maximumInboundBytes=maximum;
        if(maximum>_maximumOutboundBytes) _maximumOutboundBytes=maximum;
        _serviceMask=static_cast<std::uint8_t>(_serviceMask|ServiceBit(service));
        _requiresDestinationEvidence=_requiresDestinationEvidence||entry.Policy.Evidence!=0U;
        return EventRadioAdapterBindingStatus::Success;
    }

    EventRadioAdapterBindingStatus Freeze() noexcept {
        if(_frozen) return EventRadioAdapterBindingStatus::Frozen;
        if(_count==0||_maximumInboundBytes==0||_maximumOutboundBytes==0||_serviceMask==0)
            return EventRadioAdapterBindingStatus::InvalidBinding;
        _frozen=true;
        return EventRadioAdapterBindingStatus::Success;
    }

    bool IsFrozen() const noexcept { return _frozen; }
    std::size_t TypeCount() const noexcept { return _count; }

    bool TryGetOutboundContract(Event::EventTypeId type,Adapters::AdapterServiceClass& service,
        Primitive::PrimitivePolicyDescriptor& policy) const noexcept {
        if(!_frozen) return false;
        const auto* entry=Find(type);
        if(!entry||!entry->Encode) return false;
        service=entry->Service;policy=entry->Policy;return true;
    }

    Adapters::AdapterBindingDescriptor AdapterBinding() noexcept {
        if(!_frozen) return {};
        Adapters::AdapterBindingDescriptor binding{};
        binding.Family=Primitive::FamilyIds::Event;
        binding.Protocols={Event::EventProtocolVersion,Event::EventProtocolVersion};
        binding.MaximumInboundBytes=_maximumInboundBytes;
        binding.MaximumOutboundBytes=_maximumOutboundBytes;
        binding.ServiceClassMask=_serviceMask;
        binding.RequiresDestinationAdmissionEvidence=_requiresDestinationEvidence;
        binding.Owner=this;
        binding.AdmitInbound=&EventRadioAdapterFamilyBinding::AdmitInbound;
        binding.EncodeOutbound=&EventRadioAdapterFamilyBinding::EncodeOutbound;
        return binding;
    }

    RadioAdapterBindingDescriptor RadioBinding() noexcept {
        if(!_frozen) return {};
        return {Primitive::FamilyIds::Event,{Event::EventProtocolVersion,Event::EventProtocolVersion},
                this,&EventRadioAdapterFamilyBinding::ResolveRadioPolicy};
    }
};

} // namespace ESPressio::RadioAdapters
