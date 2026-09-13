#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

#include <ESPressio_RadioIngressRouter.hpp>
#include <ESPressio_RadioReassembly.hpp>
#include <ESPressio_Synchronization.hpp>

#include "ESPressio_RadioAdapterBinding.hpp"
#include "ESPressio_RadioAdapterEnvelope.hpp"
#include "ESPressio_RadioAdapterM1.hpp"
#include "ESPressio_RadioAdapterServiceMapping.hpp"

namespace ESPressio::RadioAdapters {

constexpr Adapters::AdapterSubmissionDisposition ToAdapterSubmissionDisposition(
    RadioAdapterBindingResolutionStatus status) noexcept {
    switch(status){
        case RadioAdapterBindingResolutionStatus::Success:return Adapters::AdapterSubmissionDisposition::Accepted;
        case RadioAdapterBindingResolutionStatus::Unsupported:return Adapters::AdapterSubmissionDisposition::Unsupported;
        case RadioAdapterBindingResolutionStatus::Malformed:return Adapters::AdapterSubmissionDisposition::Malformed;
        case RadioAdapterBindingResolutionStatus::Rejected:return Adapters::AdapterSubmissionDisposition::Rejected;
        case RadioAdapterBindingResolutionStatus::TemporarilyUnavailable:return Adapters::AdapterSubmissionDisposition::Busy;
    }
    return Adapters::AdapterSubmissionDisposition::Rejected;
}

constexpr Primitive::PrimitiveAdmissionDisposition ToPrimitiveAdmissionDisposition(
    Adapters::AdapterSubmissionDisposition disposition) noexcept {
    switch(disposition){
        case Adapters::AdapterSubmissionDisposition::Accepted:
            return Primitive::PrimitiveAdmissionDisposition::Accepted;
        case Adapters::AdapterSubmissionDisposition::Busy:
        case Adapters::AdapterSubmissionDisposition::NotRunning:
            return Primitive::PrimitiveAdmissionDisposition::TemporarilyUnavailable;
        case Adapters::AdapterSubmissionDisposition::ResourceUnavailable:
        case Adapters::AdapterSubmissionDisposition::RepresentationTooLarge:
            return Primitive::PrimitiveAdmissionDisposition::ResourceUnavailable;
        case Adapters::AdapterSubmissionDisposition::Unsupported:
            return Primitive::PrimitiveAdmissionDisposition::Unsupported;
        case Adapters::AdapterSubmissionDisposition::Malformed:
            return Primitive::PrimitiveAdmissionDisposition::Malformed;
        case Adapters::AdapterSubmissionDisposition::InvalidConfiguration:
        case Adapters::AdapterSubmissionDisposition::Rejected:
            return Primitive::PrimitiveAdmissionDisposition::Rejected;
    }
    return Primitive::PrimitiveAdmissionDisposition::Rejected;
}

/// <summary>Validates/demultiplexes one trusted direct-Radio logical message and transfers only family bytes into A2.</summary>
/// <remarks>The borrowed Radio bytes are valid only for this call. A2 copies accepted bytes before returning.</remarks>
template<class TAdapterRuntime,std::size_t TMaximumBindings>
Adapters::AdapterSubmissionDisposition AdmitDirectRadioLogicalMessage(
    TAdapterRuntime& runtime,
    const RadioAdapterBindingRegistry<TMaximumBindings>& bindings,
    const RadioAdapterProvenanceBinding& provenanceBinding,
    Radio::IRadio& provider,
    const Radio::RadioAddress& source,
    Radio::RadioServiceClass radioService,
    Adapters::AdapterByteView logicalMessage,
    std::uint64_t correlation=0,
    Adapters::AdapterInboundCompletionTarget completion={}) noexcept {
    if(!bindings.IsFrozen()||!provenanceBinding)
        return Adapters::AdapterSubmissionDisposition::InvalidConfiguration;
    if(logicalMessage.Data==nullptr||logicalMessage.Size<DirectRadioPrimitivePrefixBytes)
        return Adapters::AdapterSubmissionDisposition::Malformed;

    DirectRadioPrimitivePrefix prefix{};
    if(!DecodeDirectRadioPrimitivePrefix(logicalMessage.Data,logicalMessage.Size,prefix))
        return Adapters::AdapterSubmissionDisposition::Malformed;
    const auto* binding=bindings.Find(prefix.Family,prefix.Protocol);
    if(binding==nullptr) return Adapters::AdapterSubmissionDisposition::Unsupported;

    Adapters::AdapterServiceClass adapterService{};
    if(!TryToAdapterServiceClass(radioService,adapterService))
        return Adapters::AdapterSubmissionDisposition::InvalidConfiguration;

    const Adapters::AdapterByteView familyBytes{
        logicalMessage.Data+DirectRadioPrimitivePrefixBytes,
        logicalMessage.Size-DirectRadioPrimitivePrefixBytes};

    Primitive::PrimitivePolicyDescriptor policy{};
    const auto policyStatus=binding->ResolvePolicy(binding->Owner,prefix.Protocol,familyBytes,policy);
    if(policyStatus!=RadioAdapterBindingResolutionStatus::Success)
        return ToAdapterSubmissionDisposition(policyStatus);

    Adapters::AdapterSemanticProvenance provenance{};
    Adapters::AdapterRouteToken route{};
    const auto provenanceStatus=provenanceBinding.Resolve(
        provenanceBinding.Owner,provider,source,provenance,route);
    if(provenanceStatus!=RadioAdapterBindingResolutionStatus::Success)
        return ToAdapterSubmissionDisposition(provenanceStatus);

    return runtime.AdmitTrustedInbound(
        prefix.Family,adapterService,prefix.Protocol,familyBytes,provenance,route,policy,correlation,completion);
}

/// <summary>Offers one Radio-owned completed trusted reassembly to A2 without retaining its lease or byte pointer.</summary>
template<class TAdapterRuntime,std::size_t TMaximumBindings>
Adapters::AdapterSubmissionDisposition AdmitCompletedRadioReassembly(
    TAdapterRuntime& runtime,
    const RadioAdapterBindingRegistry<TMaximumBindings>& bindings,
    const RadioAdapterProvenanceBinding& provenanceBinding,
    Radio::RadioCompletedReassembly& completed,
    std::uint64_t correlation=0,
    Adapters::AdapterInboundCompletionTarget completionTarget={}) noexcept {
    if(!completed) return Adapters::AdapterSubmissionDisposition::Malformed;
    const auto& record=completed.Record();
    if(record.Provider==nullptr||!record.Source.IsValid()||!Radio::IsValidRadioServiceClass(record.Service))
        return Adapters::AdapterSubmissionDisposition::Malformed;
    const auto payload=completed.Payload();
    return AdmitDirectRadioLogicalMessage(
        runtime,bindings,provenanceBinding,*record.Provider,record.Source,record.Service,
        {payload.Data,payload.Size},correlation,completionTarget);
}

/// <summary>
/// Fixed Radio reassembly-ready sink; one bounded take+handoff quantum, exact-M1 receipt lifecycle and no local retry loop.
/// </summary>
/// <remarks>
/// Ordinary Primitive messages reserve at most one fixed receipt context until A2 publishes exact family admission. The
/// context owns only link metadata required to emit the separate control receipt; A2 owns all family bytes after accepted
/// handoff. `{Family=0,Protocol=1}` control receipts are consumed before family demux and never generate another receipt.
/// Receipt transmission itself is one bounded Radio submission; if it cannot be admitted, sender-side P2 retry of the
/// original Primitive occurrence provides the next opportunity for an idempotent `AlreadyAccepted` receipt.
/// </remarks>
template<class TReassemblyTable,class TAdapterRuntime,std::size_t TMaximumBindings,
         std::size_t TMaximumPendingReceipts=TMaximumBindings>
class RadioAdapterReassemblyIngress final : public Radio::IRadioReassemblyReadySink {
    static_assert(TMaximumPendingReceipts>0&&TMaximumPendingReceipts<std::numeric_limits<std::uint16_t>::max());
    struct PendingReceipt final {
        bool Occupied{false};
        std::uint64_t Generation{0};
        Radio::IRadio* Provider=nullptr;
        Radio::RadioAddress Source{};
        Radio::RadioTransferId TransferId{0};
        Radio::RadioServiceClass Service{Radio::RadioServiceClass::Invalid};
    };

    TReassemblyTable* _reassembly=nullptr;
    TAdapterRuntime* _runtime=nullptr;
    const RadioAdapterBindingRegistry<TMaximumBindings>* _bindings=nullptr;
    RadioAdapterProvenanceBinding _provenance{};
    RadioAdapterM1ReceiptBinding _m1{};
    std::array<PendingReceipt,TMaximumPendingReceipts> _pendingReceipts{};
    System::Synchronization::Mutex _receiptMutex;
    std::atomic<std::uint64_t> _accepted{0};
    std::atomic<std::uint64_t> _rejected{0};
    std::atomic<std::uint64_t> _receiptsConsumed{0};
    std::atomic<std::uint64_t> _receiptsSent{0};

    static constexpr std::uint64_t ReceiptSlotMask=0xffffULL;
    static constexpr std::uint64_t MaximumReceiptGeneration=(std::numeric_limits<std::uint64_t>::max()>>16u);
    static std::uint64_t ReceiptToken(std::size_t slot,std::uint64_t generation) noexcept {
        return (generation<<16u)|static_cast<std::uint64_t>(slot+1u);
    }
    static bool DecodeReceiptToken(std::uint64_t token,std::size_t& slot,std::uint64_t& generation) noexcept {
        const auto raw=token&ReceiptSlotMask;
        if(raw==0||raw>TMaximumPendingReceipts) return false;
        slot=static_cast<std::size_t>(raw-1u);generation=token>>16u;return generation!=0;
    }
    static void ClearPending(PendingReceipt& pending) noexcept {
        const auto generation=pending.Generation;pending={};pending.Generation=generation;
    }

    bool ReservePending(const Radio::RadioReassemblyRecord& record,std::uint64_t& token) noexcept {
        token=0;
        std::unique_lock<System::Synchronization::Mutex> lock(_receiptMutex,std::try_to_lock);
        if(!lock.owns_lock()) return false;
        for(std::size_t i=0;i<_pendingReceipts.size();++i){
            auto& pending=_pendingReceipts[i];if(pending.Occupied) continue;
            if(pending.Generation==MaximumReceiptGeneration) return false;
            ++pending.Generation;if(pending.Generation==0) return false;
            pending.Occupied=true;pending.Provider=record.Provider;pending.Source=record.Source;
            pending.TransferId=record.TransferId;pending.Service=record.Service;
            token=ReceiptToken(i,pending.Generation);return true;
        }
        return false;
    }

    bool ReleasePending(std::uint64_t token,PendingReceipt& output) noexcept {
        std::lock_guard<System::Synchronization::Mutex> lock(_receiptMutex);
        std::size_t slot=0;std::uint64_t generation=0;
        if(!DecodeReceiptToken(token,slot,generation)) return false;
        auto& pending=_pendingReceipts[slot];
        if(!pending.Occupied||pending.Generation!=generation) return false;
        output=pending;ClearPending(pending);return true;
    }

    static void CompleteInboundThunk(void* owner,const Adapters::AdapterInboundCompletion& completion) noexcept {
        static_cast<RadioAdapterReassemblyIngress*>(owner)->CompleteInbound(completion);
    }

    void CompleteInbound(const Adapters::AdapterInboundCompletion& completion) noexcept {
        PendingReceipt pending{};
        if(!ReleasePending(completion.Correlation,pending)||!_m1||pending.Provider==nullptr) return;
        if(_m1.Send(_m1.Owner,*pending.Provider,pending.Source,pending.TransferId,pending.Service,completion.Admission))
            _receiptsSent.fetch_add(1,std::memory_order_relaxed);
    }

    bool ConsumeControlReceipt(const Radio::RadioReassemblyRecord& record,Adapters::AdapterByteView payload) noexcept {
        if(!_m1) return false;
        DirectRadioM1Receipt receipt{};
        if(!DecodeDirectRadioM1Receipt(payload,receipt)) return false;
        Adapters::AdapterSemanticProvenance provenance{};Adapters::AdapterRouteToken route{};
        const auto status=_provenance.Resolve(_provenance.Owner,*record.Provider,record.Source,provenance,route);
        if(status!=RadioAdapterBindingResolutionStatus::Success||!route) return false;
        if(!_m1.Handle(_m1.Owner,route,record.Provider->ContentionDomain(),receipt.OriginalTransferId,receipt.Admission))
            return false;
        _receiptsConsumed.fetch_add(1,std::memory_order_relaxed);return true;
    }

public:
    RadioAdapterReassemblyIngress(TReassemblyTable& reassembly,TAdapterRuntime& runtime,
        const RadioAdapterBindingRegistry<TMaximumBindings>& bindings,
        RadioAdapterProvenanceBinding provenance,RadioAdapterM1ReceiptBinding m1={}) noexcept
        :_reassembly(&reassembly),_runtime(&runtime),_bindings(&bindings),_provenance(provenance),_m1(m1) {}

    void RadioReassemblyReady(Radio::IRadio& provider,const Radio::RadioAddress& source,
        Radio::RadioTransferId transferId,Radio::RadioServiceClass service) noexcept override {
        Radio::RadioCompletedReassembly completed{};
        const auto taken=_reassembly->TakeCompleteTrusted(provider,source,transferId,completed);
        if(taken!=Radio::RadioReassemblyStatus::Complete||!completed){_rejected.fetch_add(1,std::memory_order_relaxed);return;}
        if(completed.Record().Service!=service){completed.Reset();_rejected.fetch_add(1,std::memory_order_relaxed);return;}

        const auto payload=completed.Payload();
        DirectRadioPrimitivePrefix prefix{};
        if(payload.Data&&payload.Size>=DirectRadioPrimitivePrefixBytes&&
           DecodeDirectRadioPrimitivePrefix(payload.Data,payload.Size,prefix)&&
           prefix.Family==DirectRadioM1ControlFamily&&prefix.Protocol==DirectRadioM1ControlProtocol){
            const bool consumed=ConsumeControlReceipt(completed.Record(),{payload.Data,payload.Size});
            completed.Reset();
            if(consumed)_accepted.fetch_add(1,std::memory_order_relaxed);
            else _rejected.fetch_add(1,std::memory_order_relaxed);
            return;
        }

        std::uint64_t receiptToken=0;
        if(_m1&&!ReservePending(completed.Record(),receiptToken)){
            const auto& record=completed.Record();
            (void)_m1.Send(_m1.Owner,*record.Provider,record.Source,record.TransferId,record.Service,
                Primitive::PrimitiveAdmissionDisposition::ResourceUnavailable);
            completed.Reset();_rejected.fetch_add(1,std::memory_order_relaxed);return;
        }

        const auto disposition=AdmitCompletedRadioReassembly(
            *_runtime,*_bindings,_provenance,completed,receiptToken,
            receiptToken?Adapters::AdapterInboundCompletionTarget{this,&RadioAdapterReassemblyIngress::CompleteInboundThunk}
                        :Adapters::AdapterInboundCompletionTarget{});

        if(disposition!=Adapters::AdapterSubmissionDisposition::Accepted&&receiptToken){
            PendingReceipt pending{};
            if(ReleasePending(receiptToken,pending)&&pending.Provider&&_m1.Send(
                _m1.Owner,*pending.Provider,pending.Source,pending.TransferId,pending.Service,
                ToPrimitiveAdmissionDisposition(disposition)))
                _receiptsSent.fetch_add(1,std::memory_order_relaxed);
        }
        completed.Reset();
        if(disposition==Adapters::AdapterSubmissionDisposition::Accepted)_accepted.fetch_add(1,std::memory_order_relaxed);
        else _rejected.fetch_add(1,std::memory_order_relaxed);
    }

    std::uint64_t AcceptedCount()const noexcept{return _accepted.load(std::memory_order_relaxed);}
    std::uint64_t RejectedCount()const noexcept{return _rejected.load(std::memory_order_relaxed);}
    std::uint64_t ReceiptsConsumedCount()const noexcept{return _receiptsConsumed.load(std::memory_order_relaxed);}
    std::uint64_t ReceiptsSentCount()const noexcept{return _receiptsSent.load(std::memory_order_relaxed);}
};

} // namespace ESPressio::RadioAdapters
