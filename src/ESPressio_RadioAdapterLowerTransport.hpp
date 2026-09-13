#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <ESPressio_AdapterTransport.hpp>
#include <ESPressio_RadioRuntime.hpp>
#include <ESPressio_Synchronization.hpp>
#include <ESPressio_SystemPlatformClock.hpp>

#include "ESPressio_RadioAdapterEnvelope.hpp"
#include "ESPressio_RadioAdapterM1.hpp"
#include "ESPressio_RadioAdapterServiceMapping.hpp"

namespace ESPressio::RadioAdapters {

/// <summary>Fixed resolver from one opaque Adapter route fact to one generation-safe direct Radio peer handle.</summary>
struct RadioAdapterRouteBinding final {
    void* Owner=nullptr;
    bool (*Resolve)(void*,Adapters::AdapterRouteToken,Radio::RadioPeerHandle&) noexcept=nullptr;
    bool (*Validate)(void*) noexcept=nullptr;
    constexpr explicit operator bool() const noexcept { return Owner&&Resolve&&Validate; }
};

/// <summary>Fixed composition policy translating one immutable P2 occurrence into Radio-local finite service/timing requirements.</summary>
struct RadioAdapterTransferPolicyBinding final {
    void* Owner=nullptr;
    bool (*Resolve)(void*,Primitive::PrimitiveFamilyId,Primitive::PrimitiveProtocolVersion,
        const Primitive::PrimitivePolicyDescriptor&,Adapters::AdapterServiceClass,std::uint64_t,
        Radio::RadioServiceProfile&,Radio::RadioTransferTiming&) noexcept=nullptr;
    bool (*Validate)(void*) noexcept=nullptr;
    constexpr explicit operator bool() const noexcept { return Owner&&Resolve&&Validate; }
};

constexpr Adapters::LowerTransportDisposition ToLowerTransportDisposition(Radio::RadioSchedulerStatus status) noexcept {
    switch(status){
        case Radio::RadioSchedulerStatus::Success:return Adapters::LowerTransportDisposition::Accepted;
        case Radio::RadioSchedulerStatus::Busy:
        case Radio::RadioSchedulerStatus::NotInitialized:
        case Radio::RadioSchedulerStatus::ProviderUnavailable:
            return Adapters::LowerTransportDisposition::TemporarilyUnavailable;
        case Radio::RadioSchedulerStatus::ResourceUnavailable:
            return Adapters::LowerTransportDisposition::ResourceUnavailable;
        case Radio::RadioSchedulerStatus::Frozen:
        case Radio::RadioSchedulerStatus::InvalidConfiguration:
        case Radio::RadioSchedulerStatus::PayloadTooLarge:
        case Radio::RadioSchedulerStatus::Expired:
            return Adapters::LowerTransportDisposition::PermanentlyRejected;
    }
    return Adapters::LowerTransportDisposition::PermanentlyRejected;
}

constexpr bool RadioPolicyNeedsDestinationAdmission(
    const Primitive::PrimitivePolicyDescriptor& policy) noexcept {
    return policy.Evidence==1;
}

/// <summary>Direct-Radio A2 lower transport adding the exact four-byte family/version prefix before Radio admission.</summary>
/// <remarks>
/// The fixed workspace exists only for the synchronous call into RadioRuntime. RadioRuntime/scheduler copies accepted
/// logical bytes into Radio-owned capacity before returning. This class retains no payload, retry queue, worker, route,
/// fragment, or family object. When the optional M1 binding is present, only P2 occurrences explicitly requiring
/// destination Primitive admission reserve a generation-safe deferred attempt. Link completion/peer acknowledgement never
/// establishes M1. NoRemoteEvidence keeps the original immediate lower-transport-acceptance path.
/// </remarks>
template<class TRadioRuntime,std::size_t TMaximumLogicalMessageBytes>
class RadioAdapterLowerTransport final {
    static_assert(TMaximumLogicalMessageBytes>DirectRadioPrimitivePrefixBytes,
        "RadioAdapter lower transport requires room for prefix plus family representation");
    TRadioRuntime* _radio=nullptr;
    RadioAdapterRouteBinding _routes{};
    RadioAdapterTransferPolicyBinding _policy{};
    RadioAdapterM1TransportBinding _m1{};
    std::array<std::uint8_t,TMaximumLogicalMessageBytes> _workspace{};
    System::Synchronization::Mutex _workspaceMutex;
    std::atomic<bool> _quiesced{false};

    static Adapters::LowerTransportSubmitResult SubmitThunk(
        void* owner,Adapters::AdapterRecordIdentity record,
        Primitive::PrimitiveFamilyId family,Primitive::PrimitiveProtocolVersion protocol,
        const Primitive::PrimitivePolicyDescriptor& policy,Adapters::AdapterServiceClass service,
        Adapters::AdapterByteView bytes,Adapters::AdapterRouteToken route) noexcept {
        auto& self=*static_cast<RadioAdapterLowerTransport*>(owner);
        if(self._quiesced.load(std::memory_order_acquire)||self._radio==nullptr||!self._routes||!self._policy)
            return {Adapters::LowerTransportDisposition::PermanentlyRejected,0,false};
        if(bytes.Size&&bytes.Data==nullptr)
            return {Adapters::LowerTransportDisposition::PermanentlyRejected,0,false};
        if(bytes.Size>self._workspace.size()-DirectRadioPrimitivePrefixBytes)
            return {Adapters::LowerTransportDisposition::PermanentlyRejected,0,false};

        Radio::RadioPeerHandle peer{};
        if(!route||!self._routes.Resolve(self._routes.Owner,route,peer)||!peer)
            return {Adapters::LowerTransportDisposition::PermanentlyRejected,0,false};

        const auto radioService=ToRadioServiceClass(service);
        if(!Radio::IsValidRadioServiceClass(radioService))
            return {Adapters::LowerTransportDisposition::PermanentlyRejected,0,false};

        const auto now=System::Clock::Monotonic().NowNanoseconds();
        Radio::RadioServiceProfile profile{};
        Radio::RadioTransferTiming timing{};
        if(!self._policy.Resolve(self._policy.Owner,family,protocol,policy,service,now,profile,timing)||
           !profile.IsValid()||profile.Class!=radioService||!timing.IsValidFor(profile))
            return {Adapters::LowerTransportDisposition::PermanentlyRejected,0,false};

        const bool needsDestinationAdmission=RadioPolicyNeedsDestinationAdmission(policy);
        std::uint64_t transportGeneration=0;
        if(needsDestinationAdmission){
            if(!self._m1||!self._m1.Reserve(self._m1.Owner,record,route,transportGeneration)||transportGeneration==0)
                return {Adapters::LowerTransportDisposition::ResourceUnavailable,0,false};
        }

        std::unique_lock<System::Synchronization::Mutex> lock(self._workspaceMutex,std::try_to_lock);
        if(!lock.owns_lock()){
            if(transportGeneration) self._m1.Cancel(self._m1.Owner,transportGeneration);
            return {Adapters::LowerTransportDisposition::TemporarilyUnavailable,0,false};
        }
        if(!EncodeDirectRadioPrimitivePrefix(family,protocol,self._workspace.data(),self._workspace.size())){
            if(transportGeneration) self._m1.Cancel(self._m1.Owner,transportGeneration);
            return {Adapters::LowerTransportDisposition::PermanentlyRejected,0,false};
        }
        if(bytes.Size) std::memcpy(self._workspace.data()+DirectRadioPrimitivePrefixBytes,bytes.Data,bytes.Size);
        const auto logicalBytes=DirectRadioPrimitivePrefixBytes+bytes.Size;
        const auto submitted=self._radio->SubmitPeer(
            peer,profile,timing,self._workspace.data(),logicalBytes,transportGeneration);
        if(!submitted){
            if(transportGeneration) self._m1.Cancel(self._m1.Owner,transportGeneration);
            return {ToLowerTransportDisposition(submitted.Status),0,false};
        }
        if(needsDestinationAdmission)
            return {Adapters::LowerTransportDisposition::Accepted,transportGeneration,true};
        return {Adapters::LowerTransportDisposition::Accepted,0,false};
    }

    static bool ValidateThunk(void* owner) noexcept {
        auto& self=*static_cast<RadioAdapterLowerTransport*>(owner);
        return !self._quiesced.load(std::memory_order_acquire)&&self._radio!=nullptr&&self._radio->IsRunning()&&self._routes&&self._policy&&
               self._routes.Validate(self._routes.Owner)&&self._policy.Validate(self._policy.Owner);
    }

    static void CancelThunk(void* owner,Adapters::AdapterRecordIdentity record) noexcept {
        auto& self=*static_cast<RadioAdapterLowerTransport*>(owner);
        if(self._m1) self._m1.CancelRecord(self._m1.Owner,record);
    }

    static void QuiesceThunk(void* owner) noexcept {
        static_cast<RadioAdapterLowerTransport*>(owner)->_quiesced.store(true,std::memory_order_release);
    }

public:
    RadioAdapterLowerTransport(TRadioRuntime& radio,RadioAdapterRouteBinding routes,
        RadioAdapterTransferPolicyBinding policy,RadioAdapterM1TransportBinding m1={}) noexcept
        :_radio(&radio),_routes(routes),_policy(policy),_m1(m1) {}
    RadioAdapterLowerTransport(const RadioAdapterLowerTransport&)=delete;
    RadioAdapterLowerTransport& operator=(const RadioAdapterLowerTransport&)=delete;

    bool IsValid() const noexcept { return _radio!=nullptr&&_routes&&_policy; }

    Adapters::LowerTransportBinding AdapterBinding() noexcept {
        if(!IsValid()) return {};
        Adapters::LowerTransportBinding result{};
        result.Owner=this;
        result.Submit=&RadioAdapterLowerTransport::SubmitThunk;
        result.Validate=&RadioAdapterLowerTransport::ValidateThunk;
        result.Cancel=&RadioAdapterLowerTransport::CancelThunk;
        result.Quiesce=&RadioAdapterLowerTransport::QuiesceThunk;
        result.ServiceClassMask=static_cast<std::uint8_t>((std::uint8_t{1}<<Adapters::AdapterServiceClassCount)-1U);
        result.ProvidesDestinationPrimitiveAdmission=bool(_m1);
        result.ProvidesValidatedOriginalSource=false;
        return result;
    }
};

} // namespace ESPressio::RadioAdapters
