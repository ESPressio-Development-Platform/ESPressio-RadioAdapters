#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <type_traits>
#include <utility>

#include <ESPressio_AdapterBinding.hpp>
#include <ESPressio_CommandRuntime.hpp>
#include <ESPressio_CommandWireV1.hpp>

#include "ESPressio_RadioAdapterBinding.hpp"

namespace ESPressio::RadioAdapters {

enum class CommandRadioAdapterBindingStatus : std::uint8_t {
    Success=0,
    Frozen,
    DuplicateType,
    ResourceUnavailable,
    InvalidBinding,
    InvalidService,
    RouteUnavailable
};

namespace CommandRadioDetail {

constexpr Primitive::PrimitiveAdmissionDisposition MapRemoteAdmission(
    Command::CommandRemoteAdmissionStatus status) noexcept {
    using S=Command::CommandRemoteAdmissionStatus;
    using P=Primitive::PrimitiveAdmissionDisposition;
    switch(status) {
        case S::Admitted: return P::Accepted;
        case S::DuplicateTerminal:
        case S::StaleOriginRuntime:
        case S::ExecutionHistoryExpired: return P::AlreadyAccepted;
        case S::InProgress:
        case S::TemporarilyUnavailable: return P::TemporarilyUnavailable;
        case S::LedgerCapacityUnavailable: return P::ResourceUnavailable;
        case S::UnknownType:
        case S::UnsupportedProtocol: return P::Unsupported;
        case S::NoActiveRequester: return P::Rejected;
        case S::SchemaOrDecodeFailure:
        case S::Invalid: return P::Malformed;
    }
    return P::Rejected;
}

constexpr bool RetainsResponseDestination(Command::CommandRemoteAdmissionStatus status) noexcept {
    using S=Command::CommandRemoteAdmissionStatus;
    return status==S::Admitted || status==S::DuplicateTerminal ||
           status==S::StaleOriginRuntime || status==S::ExecutionHistoryExpired;
}

constexpr Adapters::AdapterResourceStatus MapWireEncode(Command::CommandWireStatus status) noexcept {
    using S=Command::CommandWireStatus;
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

constexpr Command::CommandOutboundAdmission MapOutboundSubmission(
    Adapters::AdapterSubmissionDisposition disposition) noexcept {
    using A=Adapters::AdapterSubmissionDisposition;
    using C=Command::CommandOutboundAdmissionStatus;
    switch(disposition) {
        case A::Accepted: return {C::Accepted};
        case A::Busy:
        case A::ResourceUnavailable: return {C::CapacityUnavailable};
        case A::NotRunning: return {C::Quiesced};
        case A::RepresentationTooLarge:
        case A::Unsupported:
        case A::InvalidConfiguration:
        case A::Rejected:
        case A::Malformed: return {C::InvalidTarget};
    }
    return {C::InvalidTarget};
}

constexpr bool SamePolicy(
    const Primitive::PrimitivePolicyDescriptor& left,
    const Primitive::PrimitivePolicyDescriptor& right) noexcept {
    return left.Evidence==right.Evidence &&
           left.Terminal==right.Terminal &&
           left.MaximumResidenceNanoseconds==right.MaximumResidenceNanoseconds &&
           left.MaximumAttempts==right.MaximumAttempts &&
           left.MaximumAdapterAdmissionWaitNanoseconds==right.MaximumAdapterAdmissionWaitNanoseconds &&
           left.MinimumRetrySpacingNanoseconds==right.MinimumRetrySpacingNanoseconds &&
           left.MaximumRetrySpacingNanoseconds==right.MaximumRetrySpacingNanoseconds;
}

template<class TFormat>
constexpr Command::CommandPayloadFormat PayloadFormatFor() noexcept {
    if constexpr(std::is_same_v<TFormat,Serializable::DirectBinary>) return Command::CommandPayloadFormat::DirectBinary;
    else if constexpr(std::is_same_v<TFormat,Serializable::CBOR>) return Command::CommandPayloadFormat::CBOR;
    else {
        static_assert(std::is_same_v<TFormat,Serializable::JSON>,"Unsupported Command RadioAdapter format");
        return Command::CommandPayloadFormat::JSON;
    }
}

constexpr bool IsService(Adapters::AdapterServiceClass service) noexcept {
    return static_cast<std::uint8_t>(service)<Adapters::AdapterServiceClassCount;
}

constexpr std::uint8_t ServiceBit(Adapters::AdapterServiceClass service) noexcept {
    return static_cast<std::uint8_t>(std::uint8_t{1}<<static_cast<std::uint8_t>(service));
}

} // namespace CommandRadioDetail

/// <summary>
/// Frozen Command family binding between Command-owned semantics and the generic A2/direct-Radio transport composition.
/// </summary>
/// <remarks>
/// Type wire/policy metadata is configured before Command::Runtime initialization so Command can validate its transport and
/// stage recovered responses. The initialized Runtime inbound binding is attached afterwards and before Freeze(). Locally
/// originated requests and executor responses are encoded synchronously into A2-owned bytes. Response-bearing local requests
/// retain only the trivially-copyable CommandRequestDeliveryToken in a finite campaign slot until A2 terminal feedback.
/// Response destinations retain only a generation, Type slot and opaque route token. No request/response payload, retry
/// schedule, execution state, durable ledger, fragmentation state or Radio transfer state is duplicated here.
/// </remarks>
template<class TAdapterRuntime,std::size_t TMaximumTypes,std::size_t TMaximumResponseDestinations,
         std::size_t TMaximumRequestCampaigns=TMaximumResponseDestinations>
class CommandRadioAdapterFamilyBinding final {
    static_assert(TMaximumTypes>0,"Command RadioAdapter Type capacity must be non-zero");
    static_assert(TMaximumResponseDestinations>0,"Command RadioAdapter response destination capacity must be non-zero");
    static_assert(TMaximumRequestCampaigns>0,"Command RadioAdapter request campaign capacity must be non-zero");
    static_assert(TMaximumResponseDestinations<=UINT16_MAX,"Command response destination capacity must fit index");
    static_assert(TMaximumRequestCampaigns<=UINT16_MAX,"Command request campaign capacity must fit correlation index");

    enum class OutboundKind : std::uint8_t { Request=1,Response=2 };
    struct OutboundSource final {
        OutboundKind Kind{OutboundKind::Request};
        std::size_t EntryIndex{0};
        const void* Request{nullptr};
        Command::CommandExecutionKey Key{};
        System::DeviceRuntimeIdentity Executor{};
        Command::CommandResponseDisposition Disposition{Command::CommandResponseDisposition::Succeeded};
        const void* Payload{nullptr};
    };

    using EncodeRequestThunk=Command::CommandWireResult(*)(const void*,std::uint8_t*,std::size_t);
    using EncodeResponseThunk=Command::CommandWireResult(*)(const OutboundSource&,std::uint8_t*,std::size_t);

    struct Entry final {
        Command::CommandTypeId TypeId{};
        Command::CommandPayloadFormat Format{Command::CommandPayloadFormat::DirectBinary};
        Command::Runtime* Runtime{nullptr};
        Command::CommandInboundBinding Inbound{};
        Primitive::PrimitivePolicyDescriptor RequestPolicy{};
        Primitive::PrimitivePolicyDescriptor ResponsePolicy{};
        Adapters::AdapterServiceClass RequestService{Adapters::AdapterServiceClass::BestEffort};
        Adapters::AdapterServiceClass ResponseService{Adapters::AdapterServiceClass::Responsive};
        std::size_t MaximumRequestWireBytes{0};
        std::size_t MaximumResponseWireBytes{0};
        EncodeRequestThunk EncodeRequest{nullptr};
        EncodeResponseThunk EncodeResponse{nullptr};
        bool ResponseBearing{false};
        bool Used{false};
    };

    enum class DestinationState : std::uint8_t { Free=0,Reserved };
    struct ResponseDestination final {
        DestinationState State{DestinationState::Free};
        std::uint64_t Generation{0};
        std::size_t EntryIndex{0};
        Adapters::AdapterRouteToken Route{};
    };

    enum class CampaignState : std::uint8_t { Free=0,Reserved };
    struct RequestCampaign final {
        CampaignState State{CampaignState::Free};
        std::uint64_t Generation{0};
        Command::CommandRequestDeliveryToken Token{};
        bool RequiresDestinationAdmission{false};
    };

    static constexpr std::uint64_t RequestCorrelationFlag=std::uint64_t{1}<<63U;
    static constexpr std::uint64_t MaximumPackedGeneration=(std::uint64_t{1}<<47U)-1U;

    TAdapterRuntime* _adapter{nullptr};
    RadioAdapterSemanticRouteBinding _routes{};
    std::array<Entry,TMaximumTypes> _entries{};
    std::array<ResponseDestination,TMaximumResponseDestinations> _destinations{};
    std::array<RequestCampaign,TMaximumRequestCampaigns> _campaigns{};
    std::size_t _count{0};
    std::size_t _maximumInboundBytes{0};
    std::size_t _maximumOutboundBytes{0};
    std::uint8_t _serviceMask{0};
    bool _requiresDestinationEvidence{false};
    bool _frozen{false};
    std::mutex _destinationMutex{};
    std::mutex _campaignMutex{};

    const Entry* Find(Command::CommandTypeId type) const noexcept {
        for(std::size_t i=0;i<_count;++i) if(_entries[i].Used&&_entries[i].TypeId==type) return &_entries[i];
        return nullptr;
    }
    Entry* Find(Command::CommandTypeId type) noexcept {
        for(std::size_t i=0;i<_count;++i) if(_entries[i].Used&&_entries[i].TypeId==type) return &_entries[i];
        return nullptr;
    }
    std::size_t IndexOf(const Entry* entry) const noexcept {
        return entry?static_cast<std::size_t>(entry-_entries.data()):TMaximumTypes;
    }

    static std::uint64_t ResponseCorrelation(std::size_t slot,std::uint64_t generation) noexcept {
        return ((generation&MaximumPackedGeneration)<<16U)|static_cast<std::uint64_t>(slot+1U);
    }
    static std::uint64_t RequestCorrelation(std::size_t slot,std::uint64_t generation) noexcept {
        return RequestCorrelationFlag|ResponseCorrelation(slot,generation);
    }

    Command::CommandRemoteResponseDestination ReserveResponseDestination(
        std::size_t entryIndex,Adapters::AdapterRouteToken route) noexcept {
        if(entryIndex>=_count||!route) return {};
        std::unique_lock<std::mutex> lock(_destinationMutex,std::try_to_lock);
        if(!lock.owns_lock()) return {};
        for(std::size_t i=0;i<_destinations.size();++i) {
            auto& slot=_destinations[i];
            if(slot.State!=DestinationState::Free||slot.Generation>=MaximumPackedGeneration) continue;
            ++slot.Generation;
            slot.State=DestinationState::Reserved;
            slot.EntryIndex=entryIndex;
            slot.Route=route;
            return {this,static_cast<std::uint16_t>(i),slot.Generation,&CommandRadioAdapterFamilyBinding::AcceptResponseThunk};
        }
        return {};
    }

    void ReleaseResponseDestination(std::uint16_t index,std::uint64_t generation) noexcept {
        std::lock_guard<std::mutex> lock(_destinationMutex);
        if(index>=_destinations.size()) return;
        auto& slot=_destinations[index];
        if(slot.State!=DestinationState::Reserved||slot.Generation!=generation) return;
        slot.State=DestinationState::Free;
        slot.EntryIndex=0;
        slot.Route={};
    }

    bool ReserveRequestCampaign(Command::CommandRequestDeliveryToken token,bool requiresDestinationAdmission,
                                std::uint64_t& correlation) noexcept {
        correlation=0;
        if(!token) return true;
        std::unique_lock<std::mutex> lock(_campaignMutex,std::try_to_lock);
        if(!lock.owns_lock()) return false;
        for(std::size_t i=0;i<_campaigns.size();++i) {
            auto& slot=_campaigns[i];
            if(slot.State!=CampaignState::Free||slot.Generation>=MaximumPackedGeneration) continue;
            ++slot.Generation;
            slot.State=CampaignState::Reserved;
            slot.Token=token;
            slot.RequiresDestinationAdmission=requiresDestinationAdmission;
            correlation=RequestCorrelation(i,slot.Generation);
            return true;
        }
        return false;
    }

    void ReleaseRequestCampaign(std::uint64_t correlation,bool terminalFeedback,
                                const Adapters::AdapterFamilyFeedback* feedback=nullptr) noexcept {
        if((correlation&RequestCorrelationFlag)==0) return;
        const auto rawSlot=static_cast<std::uint16_t>(correlation&0xffffU);
        if(rawSlot==0) return;
        const auto index=static_cast<std::size_t>(rawSlot-1U);
        const auto generation=(correlation>>16U)&MaximumPackedGeneration;
        Command::CommandRequestDeliveryToken token{};
        bool requiresDestinationAdmission=false;
        {
            std::lock_guard<std::mutex> lock(_campaignMutex);
            if(index>=_campaigns.size()) return;
            auto& slot=_campaigns[index];
            if(slot.State!=CampaignState::Reserved||slot.Generation!=generation) return;
            token=slot.Token;
            requiresDestinationAdmission=slot.RequiresDestinationAdmission;
            slot.State=CampaignState::Free;
            slot.Token={};
            slot.RequiresDestinationAdmission=false;
        }
        if(!terminalFeedback||!feedback||!token) return;
        const bool success=requiresDestinationAdmission
            ? feedback->Evidence==Adapters::AdapterEvidence::DestinationPrimitiveAdmission &&
              Primitive::EstablishesDestinationAdmission(feedback->Admission)
            : static_cast<std::uint8_t>(feedback->Evidence)>=
              static_cast<std::uint8_t>(Adapters::AdapterEvidence::LowerTransportAccepted);
        if(!success) (void)token.PublishFailure();
    }

    static void FeedbackOutbound(void* owner,const Adapters::AdapterFamilyFeedback& feedback) noexcept {
        static_cast<CommandRadioAdapterFamilyBinding*>(owner)->ReleaseRequestCampaign(
            feedback.Correlation,true,&feedback);
    }

    static bool AcceptResponseThunk(
        void* owner,std::uint16_t index,std::uint64_t generation,
        const Command::CommandExecutionKey& key,
        const System::DeviceRuntimeIdentity& executor,
        Command::CommandResponseDisposition disposition,
        Command::CommandResponsePayloadLease&& payload) noexcept {
        return static_cast<CommandRadioAdapterFamilyBinding*>(owner)->AcceptResponse(
            index,generation,key,executor,disposition,std::move(payload));
    }

    bool AcceptResponse(
        std::uint16_t index,std::uint64_t generation,
        const Command::CommandExecutionKey& key,
        const System::DeviceRuntimeIdentity& executor,
        Command::CommandResponseDisposition disposition,
        Command::CommandResponsePayloadLease&& payload) noexcept {
        std::size_t entryIndex=0;
        Adapters::AdapterRouteToken route{};
        {
            std::unique_lock<std::mutex> lock(_destinationMutex,std::try_to_lock);
            if(!lock.owns_lock()||index>=_destinations.size()) return false;
            auto& slot=_destinations[index];
            if(slot.State!=DestinationState::Reserved||slot.Generation!=generation||slot.EntryIndex>=_count) return false;
            entryIndex=slot.EntryIndex;
            route=slot.Route;
        }
        const auto& entry=_entries[entryIndex];
        if(!entry.Used||!entry.ResponseBearing||entry.TypeId!=key.TypeId||!entry.EncodeResponse||!route) {
            ReleaseResponseDestination(index,generation);
            return false;
        }
        if(disposition==Command::CommandResponseDisposition::Succeeded&&!payload.Payload()) {
            ReleaseResponseDestination(index,generation);
            return false;
        }

        const OutboundSource source{OutboundKind::Response,entryIndex,nullptr,key,executor,disposition,payload.Payload()};
        const auto correlation=ResponseCorrelation(index,generation);
        const auto submitted=_adapter->SubmitOutbound(
            Primitive::FamilyIds::Command,entry.ResponseService,Command::CommandProtocolVersion,
            &source,route,entry.ResponsePolicy,correlation);
        ReleaseResponseDestination(index,generation);
        return submitted==Adapters::AdapterSubmissionDisposition::Accepted;
    }

    static Adapters::AdapterEncodeResult EncodeOutbound(
        void* owner,Primitive::PrimitiveProtocolVersion protocol,std::uint64_t,
        const void* source,Adapters::AdapterMutableByteView output) noexcept {
        auto& self=*static_cast<CommandRadioAdapterFamilyBinding*>(owner);
        if(!self._frozen||protocol!=Command::CommandProtocolVersion||!source||!output.Data)
            return {Adapters::AdapterResourceStatus::InvalidConfiguration,0};
        const auto& outbound=*static_cast<const OutboundSource*>(source);
        if(outbound.EntryIndex>=self._count) return {Adapters::AdapterResourceStatus::InvalidConfiguration,0};
        const auto& entry=self._entries[outbound.EntryIndex];
        Command::CommandWireResult encoded{};
        if(outbound.Kind==OutboundKind::Request) {
            if(!entry.Used||!entry.EncodeRequest||!outbound.Request)
                return {Adapters::AdapterResourceStatus::InvalidConfiguration,0};
            encoded=entry.EncodeRequest(outbound.Request,output.Data,output.Capacity);
        } else if(outbound.Kind==OutboundKind::Response) {
            if(!entry.Used||!entry.ResponseBearing||!entry.EncodeResponse)
                return {Adapters::AdapterResourceStatus::InvalidConfiguration,0};
            encoded=entry.EncodeResponse(outbound,output.Data,output.Capacity);
        } else return {Adapters::AdapterResourceStatus::InvalidConfiguration,0};
        return {CommandRadioDetail::MapWireEncode(encoded.Status),encoded.Bytes};
    }

    static Primitive::PrimitiveAdmissionDisposition AdmitInbound(
        void* owner,Primitive::PrimitiveProtocolVersion protocol,Adapters::AdapterByteView bytes,
        const Adapters::AdapterSemanticProvenance& provenance) noexcept {
        auto& self=*static_cast<CommandRadioAdapterFamilyBinding*>(owner);
        using P=Primitive::PrimitiveAdmissionDisposition;
        if(!self._frozen||protocol!=Command::CommandProtocolVersion||!bytes.Data||bytes.Size<5)
            return P::Malformed;

        const auto kind=static_cast<Command::CommandMessageKind>(bytes.Data[4]);
        if(kind==Command::CommandMessageKind::Request) {
            Command::CommandRequestWireHeader header{};
            const auto decoded=Command::DecodeCommandRequestHeader(bytes.Data,bytes.Size,header);
            if(!decoded) return decoded.Status==Command::CommandWireStatus::UnsupportedProtocol?P::Unsupported:P::Malformed;
            auto* entry=self.Find(header.Key.TypeId);
            if(!entry||!entry->Runtime||!entry->Inbound) return P::Unsupported;
            const System::DeviceRuntimeIdentity origin{header.Key.OriginDevice,header.Key.OriginRuntime};
            if(!provenance.OriginalSource||provenance.OriginalSource.Identity!=origin) return P::Rejected;

            Command::CommandRemoteResponseDestination destination{};
            if(entry->ResponseBearing) {
                Adapters::AdapterRouteToken route{};
                if(!self._routes.TryResolve(origin.Device,route)) return P::Rejected;
                destination=self.ReserveResponseDestination(self.IndexOf(entry),route);
                if(!destination) return P::ResourceUnavailable;
            }
            const auto admitted=entry->Runtime->TryAdmitRemoteRequest(entry->Inbound,bytes.Data,bytes.Size,destination);
            if(entry->ResponseBearing&&!CommandRadioDetail::RetainsResponseDestination(admitted.Status))
                self.ReleaseResponseDestination(destination.Index,destination.Generation);
            return CommandRadioDetail::MapRemoteAdmission(admitted.Status);
        }

        if(kind==Command::CommandMessageKind::Response) {
            Command::CommandResponseWireHeader header{};
            const auto decoded=Command::DecodeCommandResponseHeader(bytes.Data,bytes.Size,header);
            if(!decoded) return decoded.Status==Command::CommandWireStatus::UnsupportedProtocol?P::Unsupported:P::Malformed;
            auto* entry=self.Find(header.Key.TypeId);
            if(!entry||!entry->Runtime||!entry->Inbound||!entry->ResponseBearing) return P::Unsupported;
            if(!provenance.OriginalSource||provenance.OriginalSource.Identity!=header.Executor) return P::Rejected;
            return CommandRadioDetail::MapRemoteAdmission(
                entry->Runtime->TryAdmitRemoteResponse(entry->Inbound,bytes.Data,bytes.Size).Status);
        }
        return P::Malformed;
    }

    static RadioAdapterBindingResolutionStatus ResolveRadioPolicy(
        void* owner,Primitive::PrimitiveProtocolVersion protocol,Adapters::AdapterServiceClass service,
        Adapters::AdapterByteView bytes,Primitive::PrimitivePolicyDescriptor& policy) noexcept {
        auto& self=*static_cast<CommandRadioAdapterFamilyBinding*>(owner);
        if(!self._frozen) return RadioAdapterBindingResolutionStatus::Rejected;
        if(protocol!=Command::CommandProtocolVersion) return RadioAdapterBindingResolutionStatus::Unsupported;
        if(!bytes.Data||bytes.Size<5) return RadioAdapterBindingResolutionStatus::Malformed;

        const auto kind=static_cast<Command::CommandMessageKind>(bytes.Data[4]);
        if(kind==Command::CommandMessageKind::Request) {
            Command::CommandRequestWireHeader header{};
            const auto decoded=Command::DecodeCommandRequestHeader(bytes.Data,bytes.Size,header);
            if(!decoded) return decoded.Status==Command::CommandWireStatus::UnsupportedProtocol
                ?RadioAdapterBindingResolutionStatus::Unsupported:RadioAdapterBindingResolutionStatus::Malformed;
            const auto* entry=self.Find(header.Key.TypeId);
            if(!entry) return RadioAdapterBindingResolutionStatus::Unsupported;
            if(service!=entry->RequestService) return RadioAdapterBindingResolutionStatus::Rejected;
            if(bytes.Size>entry->MaximumRequestWireBytes) return RadioAdapterBindingResolutionStatus::Malformed;
            policy=entry->RequestPolicy;
            return RadioAdapterBindingResolutionStatus::Success;
        }

        if(kind==Command::CommandMessageKind::Response) {
            Command::CommandResponseWireHeader header{};
            const auto decoded=Command::DecodeCommandResponseHeader(bytes.Data,bytes.Size,header);
            if(!decoded) return decoded.Status==Command::CommandWireStatus::UnsupportedProtocol
                ?RadioAdapterBindingResolutionStatus::Unsupported:RadioAdapterBindingResolutionStatus::Malformed;
            const auto* entry=self.Find(header.Key.TypeId);
            if(!entry||!entry->ResponseBearing) return RadioAdapterBindingResolutionStatus::Unsupported;
            if(service!=entry->ResponseService) return RadioAdapterBindingResolutionStatus::Rejected;
            if(bytes.Size>entry->MaximumResponseWireBytes) return RadioAdapterBindingResolutionStatus::Malformed;
            policy=entry->ResponsePolicy;
            return RadioAdapterBindingResolutionStatus::Success;
        }
        return RadioAdapterBindingResolutionStatus::Malformed;
    }

public:
    CommandRadioAdapterFamilyBinding(TAdapterRuntime& adapter,RadioAdapterSemanticRouteBinding routes) noexcept
        :_adapter(&adapter),_routes(routes) {}
    CommandRadioAdapterFamilyBinding(const CommandRadioAdapterFamilyBinding&)=delete;
    CommandRadioAdapterFamilyBinding& operator=(const CommandRadioAdapterFamilyBinding&)=delete;

    template<class TCommand,class TFormat>
    CommandRadioAdapterBindingStatus ConfigureType(
        Adapters::AdapterServiceClass requestService,
        Adapters::AdapterServiceClass responseService=Adapters::AdapterServiceClass::Responsive) noexcept {
        static_assert(TCommand::IsTransmissibleCommand&&TCommand::ValidateTier(),
                      "Radio Command binding requires a TransmissibleCommand");
        static_assert(std::is_same_v<TFormat,Serializable::DirectBinary>||
                      std::is_same_v<TFormat,Serializable::CBOR>||
                      std::is_same_v<TFormat,Serializable::JSON>,"Unsupported Command RadioAdapter format");
        if(_frozen) return CommandRadioAdapterBindingStatus::Frozen;
        if(!CommandRadioDetail::IsService(requestService)||!CommandRadioDetail::IsService(responseService))
            return CommandRadioAdapterBindingStatus::InvalidService;
        if(Find(TCommand::TypeId)) return CommandRadioAdapterBindingStatus::DuplicateType;
        if(_count==_entries.size()) return CommandRadioAdapterBindingStatus::ResourceUnavailable;

        auto& entry=_entries[_count++];
        entry.TypeId=TCommand::TypeId;
        entry.Format=CommandRadioDetail::PayloadFormatFor<TFormat>();
        entry.RequestPolicy=Primitive::PrimitivePolicyContract<typename TCommand::RequestDeliveryPolicy>::Descriptor();
        entry.RequestService=requestService;
        entry.MaximumRequestWireBytes=Command::MaximumCompleteRequestWireBytes<TCommand,TFormat>;
        entry.EncodeRequest=[](const void* request,std::uint8_t* output,std::size_t capacity) {
            if(!request) return Command::CommandWireResult{};
            return Command::EncodeCommandRequest<TCommand,TFormat>(*static_cast<const TCommand*>(request),output,capacity);
        };
        entry.ResponseBearing=!std::is_same_v<typename TCommand::ResponseType,Command::NoCommandResponse>;
        if constexpr(!std::is_same_v<typename TCommand::ResponseType,Command::NoCommandResponse>) {
            entry.ResponsePolicy=Primitive::PrimitivePolicyContract<typename TCommand::ResponseDeliveryPolicy>::Descriptor();
            entry.ResponseService=responseService;
            entry.MaximumResponseWireBytes=Command::MaximumCompleteResponseWireBytes<TCommand,TFormat>;
            entry.EncodeResponse=[](const OutboundSource& source,std::uint8_t* output,std::size_t capacity) {
                return Command::EncodeCommandResponse<TCommand,TFormat>(
                    source.Key,source.Executor,source.Disposition,
                    static_cast<const typename TCommand::ResponseType*>(source.Payload),output,capacity);
            };
            if(entry.MaximumResponseWireBytes>_maximumOutboundBytes) _maximumOutboundBytes=entry.MaximumResponseWireBytes;
            _serviceMask=static_cast<std::uint8_t>(_serviceMask|CommandRadioDetail::ServiceBit(responseService));
            _requiresDestinationEvidence=_requiresDestinationEvidence||entry.ResponsePolicy.Evidence!=0U;
        }
        entry.Used=true;
        if(entry.MaximumRequestWireBytes>_maximumInboundBytes) _maximumInboundBytes=entry.MaximumRequestWireBytes;
        if(entry.MaximumRequestWireBytes>_maximumOutboundBytes) _maximumOutboundBytes=entry.MaximumRequestWireBytes;
        if(entry.MaximumResponseWireBytes>_maximumInboundBytes) _maximumInboundBytes=entry.MaximumResponseWireBytes;
        _serviceMask=static_cast<std::uint8_t>(_serviceMask|CommandRadioDetail::ServiceBit(requestService));
        _requiresDestinationEvidence=_requiresDestinationEvidence||entry.RequestPolicy.Evidence!=0U;
        return CommandRadioAdapterBindingStatus::Success;
    }

    template<class TCommand,class TFormat>
    CommandRadioAdapterBindingStatus AttachRuntime(Command::Runtime& runtime) noexcept {
        if(_frozen) return CommandRadioAdapterBindingStatus::Frozen;
        auto* entry=Find(TCommand::TypeId);
        if(!entry||entry->Format!=CommandRadioDetail::PayloadFormatFor<TFormat>()||entry->Runtime||entry->Inbound)
            return CommandRadioAdapterBindingStatus::InvalidBinding;
        const auto inbound=runtime.template BindInbound<TCommand,TFormat>();
        if(!inbound) return CommandRadioAdapterBindingStatus::InvalidBinding;
        entry->Runtime=&runtime;
        entry->Inbound=inbound;
        return CommandRadioAdapterBindingStatus::Success;
    }

    template<class TCommand,class TFormat>
    CommandRadioAdapterBindingStatus BindType(
        Command::Runtime& runtime,Adapters::AdapterServiceClass requestService,
        Adapters::AdapterServiceClass responseService=Adapters::AdapterServiceClass::Responsive) noexcept {
        auto* entry=Find(TCommand::TypeId);
        if(!entry) {
            const auto configured=ConfigureType<TCommand,TFormat>(requestService,responseService);
            if(configured!=CommandRadioAdapterBindingStatus::Success) return configured;
            entry=Find(TCommand::TypeId);
        } else if(entry->RequestService!=requestService||
                  (entry->ResponseBearing&&entry->ResponseService!=responseService)) {
            return CommandRadioAdapterBindingStatus::InvalidBinding;
        }
        return AttachRuntime<TCommand,TFormat>(runtime);
    }

    template<class TCommand,class TFormat>
    bool ValidateOutboundContract(const Command::CommandOutboundContract& contract) noexcept {
        const auto* entry=Find(TCommand::TypeId);
        if(!entry||!entry->Used||!entry->EncodeRequest||!_routes.IsValid()) return false;
        if(contract.TypeId!=entry->TypeId||contract.Format!=CommandRadioDetail::PayloadFormatFor<TFormat>()||
           contract.MaximumRequestWireBytes!=entry->MaximumRequestWireBytes||!contract.RequestDeliveryPolicy||
           !CommandRadioDetail::SamePolicy(*contract.RequestDeliveryPolicy,entry->RequestPolicy)) return false;
        if(entry->ResponseBearing) {
            return contract.MaximumResponseWireBytes==entry->MaximumResponseWireBytes&&
                   contract.ResponseDeliveryPolicy&&
                   CommandRadioDetail::SamePolicy(*contract.ResponseDeliveryPolicy,entry->ResponsePolicy);
        }
        return contract.MaximumResponseWireBytes==0&&contract.ResponseDeliveryPolicy==nullptr;
    }

    template<class TCommand>
    Command::CommandOutboundAdmission SubmitRequest(
        System::DeviceIdentifier target,const Command::CommandRequestLease<TCommand>& request,
        Command::CommandRequestDeliveryToken token) noexcept {
        if(!_frozen||!_adapter||!target||!request) return {Command::CommandOutboundAdmissionStatus::Quiesced};
        auto* entry=Find(TCommand::TypeId);
        if(!entry||!entry->EncodeRequest||request.Facts().Key.TypeId!=entry->TypeId)
            return {Command::CommandOutboundAdmissionStatus::InvalidTarget};
        Adapters::AdapterRouteToken route{};
        if(!_routes.TryResolve(target,route)) return {Command::CommandOutboundAdmissionStatus::InvalidTarget};

        std::uint64_t correlation=request.Facts().Key.Id.Value();
        if(token&&!ReserveRequestCampaign(token,entry->RequestPolicy.Evidence!=0U,correlation))
            return {Command::CommandOutboundAdmissionStatus::CapacityUnavailable};
        const OutboundSource source{OutboundKind::Request,IndexOf(entry),&request.Request(),{}, {},
                                    Command::CommandResponseDisposition::Succeeded,nullptr};
        const auto submitted=_adapter->SubmitOutbound(
            Primitive::FamilyIds::Command,entry->RequestService,Command::CommandProtocolVersion,
            &source,route,entry->RequestPolicy,correlation);
        if(submitted!=Adapters::AdapterSubmissionDisposition::Accepted&&token)
            ReleaseRequestCampaign(correlation,false,nullptr);
        return CommandRadioDetail::MapOutboundSubmission(submitted);
    }

    template<class TCommand>
    Command::CommandRemoteResponseDestination ReserveRecoveredResponse(
        const Command::CommandExecutionKey& key) noexcept {
        auto* entry=Find(TCommand::TypeId);
        if(!entry||!entry->ResponseBearing||!entry->EncodeResponse||key.TypeId!=entry->TypeId||!key.IsValid()) return {};
        Adapters::AdapterRouteToken route{};
        if(!_routes.TryResolve(key.OriginDevice,route)) return {};
        return ReserveResponseDestination(IndexOf(entry),route);
    }

    void ReleaseRecoveredResponse(Command::CommandRemoteResponseDestination destination) noexcept {
        if(!destination||destination.Context!=this||destination.Accept!=&CommandRadioAdapterFamilyBinding::AcceptResponseThunk) return;
        ReleaseResponseDestination(destination.Index,destination.Generation);
    }

    CommandRadioAdapterBindingStatus Freeze() noexcept {
        if(_frozen) return CommandRadioAdapterBindingStatus::Frozen;
        if(_count==0||_maximumInboundBytes==0||_maximumOutboundBytes==0||_serviceMask==0||_adapter==nullptr)
            return CommandRadioAdapterBindingStatus::InvalidBinding;
        if(!_routes.IsValid()) return CommandRadioAdapterBindingStatus::RouteUnavailable;
        for(std::size_t i=0;i<_count;++i)
            if(!_entries[i].Used||!_entries[i].Runtime||!_entries[i].Inbound||!_entries[i].EncodeRequest)
                return CommandRadioAdapterBindingStatus::InvalidBinding;
        _frozen=true;
        return CommandRadioAdapterBindingStatus::Success;
    }

    bool IsFrozen() const noexcept { return _frozen; }
    std::size_t TypeCount() const noexcept { return _count; }

    Adapters::AdapterBindingDescriptor AdapterBinding() noexcept {
        if(!_frozen) return {};
        Adapters::AdapterBindingDescriptor binding{};
        binding.Family=Primitive::FamilyIds::Command;
        binding.Protocols={Command::CommandProtocolVersion,Command::CommandProtocolVersion};
        binding.MaximumInboundBytes=_maximumInboundBytes;
        binding.MaximumOutboundBytes=_maximumOutboundBytes;
        binding.ServiceClassMask=_serviceMask;
        binding.RequiresDestinationAdmissionEvidence=_requiresDestinationEvidence;
        binding.RequiresValidatedOriginalSource=true;
        binding.Owner=this;
        binding.AdmitInbound=&CommandRadioAdapterFamilyBinding::AdmitInbound;
        binding.EncodeOutbound=&CommandRadioAdapterFamilyBinding::EncodeOutbound;
        binding.Feedback=&CommandRadioAdapterFamilyBinding::FeedbackOutbound;
        return binding;
    }

    RadioAdapterBindingDescriptor RadioBinding() noexcept {
        if(!_frozen) return {};
        return {Primitive::FamilyIds::Command,{Command::CommandProtocolVersion,Command::CommandProtocolVersion},
                this,&CommandRadioAdapterFamilyBinding::ResolveRadioPolicy};
    }
};

} // namespace ESPressio::RadioAdapters
