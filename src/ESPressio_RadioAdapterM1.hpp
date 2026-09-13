#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

#include <ESPressio_AdapterTransport.hpp>
#include <ESPressio_PrimitiveAdmission.hpp>
#include <ESPressio_RadioRuntime.hpp>
#include <ESPressio_Synchronization.hpp>
#include <ESPressio_SystemPlatformClock.hpp>

#include "ESPressio_RadioAdapterEnvelope.hpp"

namespace ESPressio::RadioAdapters {

inline constexpr Primitive::PrimitiveFamilyId DirectRadioM1ControlFamily=0;
inline constexpr Primitive::PrimitiveProtocolVersion DirectRadioM1ControlProtocol=1;
inline constexpr std::size_t DirectRadioM1ReceiptBytes=8;

struct DirectRadioM1Receipt final {
    Radio::RadioTransferId OriginalTransferId{0};
    Primitive::PrimitiveAdmissionDisposition Admission{Primitive::PrimitiveAdmissionDisposition::Rejected};
};

inline bool IsDirectRadioM1Admission(std::uint8_t raw) noexcept {
    return raw<=static_cast<std::uint8_t>(Primitive::PrimitiveAdmissionDisposition::Malformed);
}

inline bool EncodeDirectRadioM1Receipt(
    Radio::RadioTransferId originalTransferId,
    Primitive::PrimitiveAdmissionDisposition admission,
    std::uint8_t* output,
    std::size_t capacity) noexcept {
    if(originalTransferId==0||output==nullptr||capacity<DirectRadioM1ReceiptBytes) return false;
    if(!EncodeDirectRadioPrimitivePrefix(
        DirectRadioM1ControlFamily,DirectRadioM1ControlProtocol,output,capacity)) return false;
    output[4]=static_cast<std::uint8_t>(originalTransferId&0xffu);
    output[5]=static_cast<std::uint8_t>((originalTransferId>>8u)&0xffu);
    output[6]=static_cast<std::uint8_t>(admission);
    output[7]=0;
    return true;
}

inline bool DecodeDirectRadioM1Receipt(
    Adapters::AdapterByteView bytes,
    DirectRadioM1Receipt& receipt) noexcept {
    receipt={};
    if(bytes.Data==nullptr||bytes.Size!=DirectRadioM1ReceiptBytes) return false;
    DirectRadioPrimitivePrefix prefix{};
    if(!DecodeDirectRadioPrimitivePrefix(bytes.Data,bytes.Size,prefix)||
       prefix.Family!=DirectRadioM1ControlFamily||prefix.Protocol!=DirectRadioM1ControlProtocol||
       bytes.Data[7]!=0||!IsDirectRadioM1Admission(bytes.Data[6])) return false;
    const auto id=static_cast<Radio::RadioTransferId>(
        static_cast<std::uint16_t>(bytes.Data[4])|
        (static_cast<std::uint16_t>(bytes.Data[5])<<8u));
    if(id==0) return false;
    receipt.OriginalTransferId=id;
    receipt.Admission=static_cast<Primitive::PrimitiveAdmissionDisposition>(bytes.Data[6]);
    return true;
}

struct RadioAdapterM1WakeTarget final {
    void* Owner=nullptr;
    void (*Wake)(void*) noexcept=nullptr;
    constexpr explicit operator bool()const noexcept{return Owner&&Wake;}
};

/// <summary>Composition policy for one exact-M1 control receipt; no retry ownership is implied.</summary>
struct RadioAdapterM1ReceiptPolicyBinding final {
    void* Owner=nullptr;
    bool (*Resolve)(void*,Radio::RadioServiceClass,std::uint64_t,
        Radio::RadioServiceProfile&,Radio::RadioTransferTiming&) noexcept=nullptr;
    bool (*Validate)(void*) noexcept=nullptr;
    constexpr explicit operator bool()const noexcept{return Owner&&Resolve&&Validate;}
};

/// <summary>Optional R9 lower-transport extension used only when a P2 occurrence requires destination M1.</summary>
struct RadioAdapterM1TransportBinding final {
    void* Owner=nullptr;
    bool (*Reserve)(void*,Adapters::AdapterRecordIdentity,Adapters::AdapterRouteToken,std::uint64_t&) noexcept=nullptr;
    void (*Cancel)(void*,std::uint64_t) noexcept=nullptr;
    void (*CancelRecord)(void*,Adapters::AdapterRecordIdentity) noexcept=nullptr;
    constexpr explicit operator bool()const noexcept{return Owner&&Reserve&&Cancel&&CancelRecord;}
};

/// <summary>Fixed RadioAdapters receipt seam used by ingress without exposing controller storage.</summary>
struct RadioAdapterM1ReceiptBinding final {
    void* Owner=nullptr;
    bool (*Handle)(void*,Adapters::AdapterRouteToken,Radio::RadioContentionDomainId,
        Radio::RadioTransferId,Primitive::PrimitiveAdmissionDisposition) noexcept=nullptr;
    bool (*Send)(void*,Radio::IRadio&,const Radio::RadioAddress&,Radio::RadioTransferId,
        Radio::RadioServiceClass,Primitive::PrimitiveAdmissionDisposition) noexcept=nullptr;
    constexpr explicit operator bool()const noexcept{return Owner&&Handle&&Send;}
};

/// <summary>
/// Fixed-capacity exact-M1 correlation owner for direct Radio transport attempts.
/// </summary>
/// <remarks>
/// Radio terminal/link evidence and destination Primitive admission remain independent. The controller leases scheduler
/// transfer identifiers while destination evidence is outstanding, stores at most one pending A2 completion per attempt,
/// and never invokes A2 from a Radio callback. Radio/result callbacks only publish bounded slot state and a wake; the
/// composition later calls ServiceOne() after the A2 outbound quantum has had an opportunity to retain its record.
/// No retry policy, payload bytes, Primitive object, fragmentation state, task or unbounded allocation is owned here.
/// </remarks>
template<class TRadioRuntime,std::size_t TMaximumAttempts>
class RadioAdapterM1Controller final : public Radio::IRadioRuntimeTransferResultSink {
    static_assert(TMaximumAttempts>0&&TMaximumAttempts<std::numeric_limits<std::uint16_t>::max());
    struct Attempt final {
        bool Occupied{false};
        std::uint64_t Generation{0};
        Adapters::AdapterRecordIdentity Record{};
        Adapters::AdapterRouteToken Route{};
        Radio::RadioContentionDomainId Domain{};
        Radio::RadioTransferId TransferId{0};
        bool TransferAssigned{false};
        bool PendingCompletion{false};
        Adapters::LowerTransportDisposition PendingDisposition{Adapters::LowerTransportDisposition::ResourceUnavailable};
        Primitive::PrimitiveAdmissionDisposition PendingAdmission{Primitive::PrimitiveAdmissionDisposition::Unsupported};
        bool PendingHasAdmission{false};
    };

    TRadioRuntime* _radio=nullptr;
    std::array<Attempt,TMaximumAttempts> _attempts{};
    RadioAdapterM1ReceiptPolicyBinding _receiptPolicy{};
    RadioAdapterM1WakeTarget _wake{};
    Radio::IRadioRuntimeTransferResultSink* _downstream=nullptr;
    void* _completionOwner=nullptr;
    Adapters::AdapterSubmissionDisposition (*_complete)(void*,const Adapters::LowerTransportCompletion&) noexcept=nullptr;
    mutable System::Synchronization::Mutex _mutex;

    static constexpr std::uint64_t SlotMask=0xffffULL;
    static constexpr std::uint64_t MaximumGeneration=(std::numeric_limits<std::uint64_t>::max()>>16u);

    static std::uint64_t Token(std::size_t slot,std::uint64_t generation) noexcept {
        return (generation<<16u)|static_cast<std::uint64_t>(slot+1u);
    }
    static bool DecodeToken(std::uint64_t token,std::size_t& slot,std::uint64_t& generation) noexcept {
        const auto raw=token&SlotMask;
        if(raw==0||raw>TMaximumAttempts) return false;
        slot=static_cast<std::size_t>(raw-1u);generation=token>>16u;return generation!=0;
    }
    static void ClearAttempt(Attempt& attempt) noexcept {
        const auto generation=attempt.Generation;
        attempt={};attempt.Generation=generation;
    }
    void SignalWake() noexcept {if(_wake) _wake.Wake(_wake.Owner);}

    bool QueueCompletionLocked(
        Attempt& attempt,Adapters::LowerTransportDisposition disposition,
        Primitive::PrimitiveAdmissionDisposition admission,bool hasAdmission) noexcept {
        if(attempt.PendingCompletion) return false;
        attempt.PendingCompletion=true;
        attempt.PendingDisposition=disposition;
        attempt.PendingAdmission=admission;
        attempt.PendingHasAdmission=hasAdmission;
        return true;
    }

    static bool IsReservedThunk(void* owner,Radio::RadioContentionDomainId domain,Radio::RadioTransferId id) noexcept {
        auto& self=*static_cast<RadioAdapterM1Controller*>(owner);
        std::unique_lock<System::Synchronization::Mutex> lock(self._mutex,std::try_to_lock);
        if(!lock.owns_lock()) return true;
        for(const auto& attempt:self._attempts)
            if(attempt.Occupied&&attempt.TransferAssigned&&attempt.Domain==domain&&attempt.TransferId==id) return true;
        return false;
    }
    static bool ReserveIssuedThunk(void* owner,Radio::RadioContentionDomainId domain,
        std::uint64_t correlation,Radio::RadioTransferId id) noexcept {
        auto& self=*static_cast<RadioAdapterM1Controller*>(owner);
        std::unique_lock<System::Synchronization::Mutex> lock(self._mutex,std::try_to_lock);
        if(!lock.owns_lock()||!domain||id==0) return false;
        std::size_t slot=0;std::uint64_t generation=0;
        if(!DecodeToken(correlation,slot,generation)) return false;
        auto& attempt=self._attempts[slot];
        if(!attempt.Occupied||attempt.Generation!=generation||attempt.TransferAssigned) return false;
        for(std::size_t i=0;i<self._attempts.size();++i){
            if(i==slot) continue;
            const auto& other=self._attempts[i];
            if(other.Occupied&&other.TransferAssigned&&other.Domain==domain&&other.TransferId==id) return false;
        }
        attempt.Domain=domain;attempt.TransferId=id;attempt.TransferAssigned=true;return true;
    }
    static void ReleaseIssuedThunk(void* owner,Radio::RadioContentionDomainId domain,
        std::uint64_t correlation,Radio::RadioTransferId id) noexcept {
        auto& self=*static_cast<RadioAdapterM1Controller*>(owner);
        std::unique_lock<System::Synchronization::Mutex> lock(self._mutex,std::try_to_lock);
        if(!lock.owns_lock()) return;
        std::size_t slot=0;std::uint64_t generation=0;
        if(!DecodeToken(correlation,slot,generation)) return;
        auto& attempt=self._attempts[slot];
        if(attempt.Occupied&&attempt.Generation==generation&&attempt.TransferAssigned&&
           attempt.Domain==domain&&attempt.TransferId==id){
            attempt.Domain={};attempt.TransferId=0;attempt.TransferAssigned=false;
        }
    }
    static bool ReserveThunk(void* owner,Adapters::AdapterRecordIdentity record,
        Adapters::AdapterRouteToken route,std::uint64_t& token) noexcept {
        return static_cast<RadioAdapterM1Controller*>(owner)->Reserve(record,route,token);
    }
    static void CancelThunk(void* owner,std::uint64_t token) noexcept {
        static_cast<RadioAdapterM1Controller*>(owner)->Cancel(token);
    }
    static void CancelRecordThunk(void* owner,Adapters::AdapterRecordIdentity record) noexcept {
        static_cast<RadioAdapterM1Controller*>(owner)->CancelRecord(record);
    }
    static bool HandleReceiptThunk(void* owner,Adapters::AdapterRouteToken route,
        Radio::RadioContentionDomainId domain,Radio::RadioTransferId transferId,
        Primitive::PrimitiveAdmissionDisposition admission) noexcept {
        return static_cast<RadioAdapterM1Controller*>(owner)->HandleReceipt(route,domain,transferId,admission);
    }
    static bool SendReceiptThunk(void* owner,Radio::IRadio& provider,const Radio::RadioAddress& source,
        Radio::RadioTransferId transferId,Radio::RadioServiceClass service,
        Primitive::PrimitiveAdmissionDisposition admission) noexcept {
        return static_cast<RadioAdapterM1Controller*>(owner)->SendReceipt(provider,source,transferId,service,admission);
    }

public:
    RadioAdapterM1Controller(TRadioRuntime& radio,RadioAdapterM1ReceiptPolicyBinding receiptPolicy={},
        RadioAdapterM1WakeTarget wake={},Radio::IRadioRuntimeTransferResultSink* downstream=nullptr) noexcept
        :_radio(&radio),_receiptPolicy(receiptPolicy),_wake(wake),_downstream(downstream) {}
    RadioAdapterM1Controller(const RadioAdapterM1Controller&)=delete;
    RadioAdapterM1Controller& operator=(const RadioAdapterM1Controller&)=delete;

    template<class TAdapterRuntime>
    void BindAdapterRuntime(TAdapterRuntime& runtime) noexcept {
        _completionOwner=&runtime;
        _complete=[](void* owner,const Adapters::LowerTransportCompletion& completion) noexcept {
            return static_cast<TAdapterRuntime*>(owner)->CompleteTransport(completion);
        };
    }

    bool IsValid() const noexcept {
        return _radio!=nullptr&&_completionOwner!=nullptr&&_complete!=nullptr&&
            (!_receiptPolicy||_receiptPolicy.Validate(_receiptPolicy.Owner));
    }

    Radio::RadioTransferIdLeaseTarget TransferIdLeaseTarget() noexcept {
        return {this,&RadioAdapterM1Controller::IsReservedThunk,
            &RadioAdapterM1Controller::ReserveIssuedThunk,&RadioAdapterM1Controller::ReleaseIssuedThunk};
    }
    RadioAdapterM1TransportBinding TransportBinding() noexcept {
        return {this,&RadioAdapterM1Controller::ReserveThunk,&RadioAdapterM1Controller::CancelThunk,
            &RadioAdapterM1Controller::CancelRecordThunk};
    }
    RadioAdapterM1ReceiptBinding ReceiptBinding() noexcept {
        return {this,&RadioAdapterM1Controller::HandleReceiptThunk,&RadioAdapterM1Controller::SendReceiptThunk};
    }

    bool Reserve(Adapters::AdapterRecordIdentity record,Adapters::AdapterRouteToken route,std::uint64_t& token) noexcept {
        token=0;if(!record||!route) return false;
        std::unique_lock<System::Synchronization::Mutex> lock(_mutex,std::try_to_lock);
        if(!lock.owns_lock()) return false;
        for(std::size_t i=0;i<_attempts.size();++i){
            auto& attempt=_attempts[i];
            if(attempt.Occupied) continue;
            if(attempt.Generation==MaximumGeneration) return false;
            ++attempt.Generation;if(attempt.Generation==0) return false;
            attempt.Occupied=true;attempt.Record=record;attempt.Route=route;
            token=Token(i,attempt.Generation);return true;
        }
        return false;
    }

    void Cancel(std::uint64_t token) noexcept {
        std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
        std::size_t slot=0;std::uint64_t generation=0;
        if(!DecodeToken(token,slot,generation)) return;
        auto& attempt=_attempts[slot];
        if(attempt.Occupied&&attempt.Generation==generation) ClearAttempt(attempt);
    }

    void CancelRecord(Adapters::AdapterRecordIdentity record) noexcept {
        if(!record) return;
        std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
        for(auto& attempt:_attempts){
            if(attempt.Occupied&&attempt.Record==record){
                ClearAttempt(attempt);
                return;
            }
        }
    }

    void RadioLogicalTransferResolved(const Radio::RadioRuntimeTransferResult& result) noexcept override {
        bool wake=false;
        {
            std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
            for(auto& attempt:_attempts){
                if(!attempt.Occupied||!attempt.TransferAssigned||attempt.Domain!=result.Domain||
                   attempt.TransferId!=result.Result.TransferId) continue;
                if(result.Result.Status!=Radio::RadioTransferTerminalStatus::Completed)
                    wake=QueueCompletionLocked(attempt,Adapters::LowerTransportDisposition::ResourceUnavailable,
                        Primitive::PrimitiveAdmissionDisposition::Unsupported,false)||wake;
                break;
            }
        }
        if(_downstream) _downstream->RadioLogicalTransferResolved(result);
        if(wake) SignalWake();
    }

    bool HandleReceipt(Adapters::AdapterRouteToken route,Radio::RadioContentionDomainId domain,
        Radio::RadioTransferId transferId,Primitive::PrimitiveAdmissionDisposition admission) noexcept {
        bool wake=false;
        {
            std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
            for(auto& attempt:_attempts){
                if(!attempt.Occupied||!attempt.TransferAssigned||attempt.Domain!=domain||attempt.TransferId!=transferId) continue;
                if(attempt.Route.Value!=route.Value) return false;
                wake=QueueCompletionLocked(attempt,Adapters::LowerTransportDisposition::Accepted,admission,true);
                break;
            }
        }
        if(wake) SignalWake();
        return wake;
    }

    bool SendReceipt(Radio::IRadio& provider,const Radio::RadioAddress& source,
        Radio::RadioTransferId transferId,Radio::RadioServiceClass inboundService,
        Primitive::PrimitiveAdmissionDisposition admission) noexcept {
        if(_radio==nullptr||!_receiptPolicy||!source.IsValid()||source.IsBroadcast()||transferId==0) return false;
        Radio::RadioServiceProfile profile{};Radio::RadioTransferTiming timing{};
        const auto now=System::Clock::Monotonic().NowNanoseconds();
        if(!_receiptPolicy.Resolve(_receiptPolicy.Owner,inboundService,now,profile,timing)||
           !profile.IsValid()||!timing.IsValidFor(profile)) return false;
        std::array<std::uint8_t,DirectRadioM1ReceiptBytes> bytes{};
        if(!EncodeDirectRadioM1Receipt(transferId,admission,bytes.data(),bytes.size())) return false;
        Radio::RadioPeerHandle peer{};
        const auto observed=_radio->ObservePeer(provider,source,peer);
        if(observed==Radio::RadioPeerObserveResult::Invalid||observed==Radio::RadioPeerObserveResult::ResourceUnavailable||!peer)
            return false;
        const auto submitted=_radio->SubmitPeer(peer,profile,timing,bytes.data(),bytes.size(),0);
        return bool(submitted);
    }

    /// <summary>Attempts one pending deferred completion; no polling or retry loop is owned by the controller.</summary>
    Adapters::AdapterSubmissionDisposition ServiceOne() noexcept {
        if(_complete==nullptr||_completionOwner==nullptr) return Adapters::AdapterSubmissionDisposition::InvalidConfiguration;
        std::size_t selected=_attempts.size();
        Adapters::LowerTransportCompletion completion{};
        {
            std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
            for(std::size_t i=0;i<_attempts.size();++i){
                const auto& attempt=_attempts[i];
                if(!attempt.Occupied||!attempt.PendingCompletion) continue;
                selected=i;
                completion.Record=attempt.Record;
                completion.TransportGeneration=Token(i,attempt.Generation);
                completion.Disposition=attempt.PendingDisposition;
                completion.DestinationAdmission=attempt.PendingAdmission;
                completion.HasDestinationAdmission=attempt.PendingHasAdmission;
                break;
            }
        }
        if(selected==_attempts.size()) return Adapters::AdapterSubmissionDisposition::Rejected;
        const auto result=_complete(_completionOwner,completion);
        if(result==Adapters::AdapterSubmissionDisposition::Busy){SignalWake();return result;}
        {
            std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
            auto& attempt=_attempts[selected];
            if(attempt.Occupied&&attempt.Generation==(completion.TransportGeneration>>16u)&&attempt.PendingCompletion)
                ClearAttempt(attempt);
        }
        return result;
    }

    std::size_t OutstandingAttempts() const noexcept {
        std::lock_guard<System::Synchronization::Mutex> lock(_mutex);
        std::size_t count=0;for(const auto& attempt:_attempts)if(attempt.Occupied)++count;return count;
    }
};

} // namespace ESPressio::RadioAdapters
