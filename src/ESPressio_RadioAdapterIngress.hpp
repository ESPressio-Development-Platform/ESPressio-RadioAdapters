#pragma once

#include <cstddef>
#include <cstdint>

#include <ESPressio_AdapterRuntime.hpp>
#include <ESPressio_RadioIngressRouter.hpp>
#include <ESPressio_RadioReassembly.hpp>

#include "ESPressio_RadioAdapterBinding.hpp"
#include "ESPressio_RadioAdapterEnvelope.hpp"
#include "ESPressio_RadioAdapterServiceMapping.hpp"

namespace ESPressio::RadioAdapters {

/// <summary>Maps pre-A2 RadioAdapter metadata resolution failure onto the neutral Adapter submission surface.</summary>
constexpr Adapters::AdapterSubmissionDisposition ToAdapterSubmissionDisposition(
    RadioAdapterBindingResolutionStatus status) noexcept {
    switch(status){
        case RadioAdapterBindingResolutionStatus::Success:
            return Adapters::AdapterSubmissionDisposition::Accepted;
        case RadioAdapterBindingResolutionStatus::Unsupported:
            return Adapters::AdapterSubmissionDisposition::Unsupported;
        case RadioAdapterBindingResolutionStatus::Malformed:
            return Adapters::AdapterSubmissionDisposition::Malformed;
        case RadioAdapterBindingResolutionStatus::Rejected:
            return Adapters::AdapterSubmissionDisposition::Rejected;
        case RadioAdapterBindingResolutionStatus::TemporarilyUnavailable:
            return Adapters::AdapterSubmissionDisposition::Busy;
    }
    return Adapters::AdapterSubmissionDisposition::Rejected;
}

/// <summary>
/// Validates/demultiplexes one complete trusted direct-Radio logical message and transfers only the family bytes into A2.
/// </summary>
/// <remarks>
/// The borrowed logical-message buffer remains Radio-owned for the duration of this call only. A successful A2
/// admission synchronously copies it into Adapter-owned capacity before returning. The four-byte RadioAdapter prefix is
/// consumed here and is never exposed to Event/Command/State family decoders. No Radio lease, source pointer, family
/// object, retry record or worker is retained by this layer.
/// </remarks>
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
    if(!bindings.IsFrozen() || !provenanceBinding)
        return Adapters::AdapterSubmissionDisposition::InvalidConfiguration;
    if(logicalMessage.Data==nullptr || logicalMessage.Size<DirectRadioPrimitivePrefixBytes)
        return Adapters::AdapterSubmissionDisposition::Malformed;

    DirectRadioPrimitivePrefix prefix{};
    if(!DecodeDirectRadioPrimitivePrefix(logicalMessage.Data,logicalMessage.Size,prefix))
        return Adapters::AdapterSubmissionDisposition::Malformed;

    const auto* binding=bindings.Find(prefix.Family,prefix.Protocol);
    if(binding==nullptr) return Adapters::AdapterSubmissionDisposition::Unsupported;

    Adapters::AdapterServiceClass adapterService{};
    if(!TryMapRadioServiceClass(radioService,adapterService))
        return Adapters::AdapterSubmissionDisposition::InvalidConfiguration;

    const Adapters::AdapterByteView familyBytes{
        logicalMessage.Data+DirectRadioPrimitivePrefixBytes,
        logicalMessage.Size-DirectRadioPrimitivePrefixBytes};

    Primitive::PrimitivePolicyDescriptor policy{};
    const auto policyStatus=binding->ResolvePolicy(
        binding->Owner,prefix.Protocol,familyBytes,policy);
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

/// <summary>
/// Consumes one Radio-owned completed trusted reassembly only for the duration of the synchronous A2 ownership handoff.
/// </summary>
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
    if(record.Provider==nullptr || !record.Source.IsValid() || !Radio::IsValidRadioServiceClass(record.Service))
        return Adapters::AdapterSubmissionDisposition::Malformed;
    const auto payload=completed.Payload();
    return AdmitDirectRadioLogicalMessage(
        runtime,bindings,provenanceBinding,*record.Provider,record.Source,record.Service,
        {payload.Data,payload.Size},correlation,completionTarget);
}

/// <summary>
/// Fixed Radio reassembly-ready sink. It performs one bounded TakeCompleteTrusted + A2 handoff and never retries locally.
/// </summary>
template<class TReassemblyTable,class TAdapterRuntime,std::size_t TMaximumBindings>
class RadioAdapterReassemblyIngress final : public Radio::IRadioReassemblyReadySink {
    TReassemblyTable* _reassembly=nullptr;
    TAdapterRuntime* _runtime=nullptr;
    const RadioAdapterBindingRegistry<TMaximumBindings>* _bindings=nullptr;
    RadioAdapterProvenanceBinding _provenance{};
    std::uint64_t _accepted=0;
    std::uint64_t _rejected=0;
public:
    RadioAdapterReassemblyIngress(
        TReassemblyTable& reassembly,
        TAdapterRuntime& runtime,
        const RadioAdapterBindingRegistry<TMaximumBindings>& bindings,
        RadioAdapterProvenanceBinding provenance) noexcept
        :_reassembly(&reassembly),_runtime(&runtime),_bindings(&bindings),_provenance(provenance) {}

    void RadioReassemblyReady(
        Radio::IRadio& provider,
        const Radio::RadioAddress& source,
        Radio::RadioTransferId transferId,
        Radio::RadioServiceClass service) noexcept override {
        Radio::RadioCompletedReassembly completed{};
        const auto taken=_reassembly->TakeCompleteTrusted(provider,source,transferId,completed);
        if(taken!=Radio::RadioReassemblyStatus::Complete || !completed){++_rejected;return;}
        if(completed.Record().Service!=service){completed.Reset();++_rejected;return;}
        const auto disposition=AdmitCompletedRadioReassembly(
            *_runtime,*_bindings,_provenance,completed,static_cast<std::uint64_t>(transferId));
        completed.Reset();
        if(disposition==Adapters::AdapterSubmissionDisposition::Accepted) ++_accepted;
        else ++_rejected;
    }

    std::uint64_t AcceptedCount() const noexcept { return _accepted; }
    std::uint64_t RejectedCount() const noexcept { return _rejected; }
};

} // namespace ESPressio::RadioAdapters
