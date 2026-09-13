#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include <ESPressio_RadioAdapters.hpp>

using namespace ESPressio;

namespace {

class FakeRadio final : public Radio::IRadio {
public:
    bool Start() override { return true; }
    void Stop() noexcept override {}
    bool IsStarted() const noexcept override { return true; }
    Radio::RadioCapabilities Capabilities() const noexcept override { return {}; }
    Radio::RadioAddress LocalAddress() const noexcept override {
        const std::uint8_t bytes[]{0x10,0x11};
        return Radio::RadioAddress::FromBytes(bytes,2);
    }
    Radio::RadioContentionDomainId ContentionDomain() const noexcept override { return {1}; }
    Radio::RadioProviderResourceProfile ProviderResources() const noexcept override { return {}; }
    bool IsTransmitReady() const noexcept override { return true; }
    Radio::RadioTransmissionCost EstimateTransmissionCost(
        const Radio::RadioAddress&,std::size_t,const Radio::RadioServiceProfile&) const noexcept override {
        return {1,1,Radio::RadioCostEstimateQuality::ConservativeAirtime};
    }
    Radio::RadioSendResult Send(const Radio::RadioAddress&,const std::uint8_t*,std::size_t) noexcept override {
        return Radio::RadioSendResult::Accepted(Radio::RadioDirectLinkEvidence::CompletedWithoutPeerAcknowledgement());
    }
    void SetReceiver(Radio::IRadioReceiver*) noexcept override {}
    void SetRuntimeSink(Radio::IRadioRuntimeSink*) noexcept override {}
    Radio::ManagedRadioIngressServiceResult ServiceInbound(std::size_t) noexcept override { return {}; }
};

struct PolicyContext { bool malformed=false; };

RadioAdapters::RadioAdapterBindingResolutionStatus ResolvePolicy(
    void* owner,Primitive::PrimitiveProtocolVersion protocol,Adapters::AdapterByteView bytes,
    Primitive::PrimitivePolicyDescriptor& policy) noexcept {
    auto& context=*static_cast<PolicyContext*>(owner);
    if(context.malformed || bytes.Data==nullptr || bytes.Size==0)
        return RadioAdapters::RadioAdapterBindingResolutionStatus::Malformed;
    if(protocol!=1 || bytes.Data[0]==0xFE)
        return RadioAdapters::RadioAdapterBindingResolutionStatus::Unsupported;
    policy={};
    policy.Category=1;
    policy.Evidence=0;
    policy.MaximumAttempts=1;
    return RadioAdapters::RadioAdapterBindingResolutionStatus::Success;
}

struct ProvenanceContext { bool reject=false; };

RadioAdapters::RadioAdapterBindingResolutionStatus ResolveProvenance(
    void* owner,Radio::IRadio&,const Radio::RadioAddress& source,
    Adapters::AdapterSemanticProvenance& provenance,Adapters::AdapterRouteToken& route) noexcept {
    auto& context=*static_cast<ProvenanceContext*>(owner);
    if(context.reject) return RadioAdapters::RadioAdapterBindingResolutionStatus::Rejected;
    if(!source.IsValid()) return RadioAdapters::RadioAdapterBindingResolutionStatus::Malformed;
    provenance={};
    provenance.ImmediatePeer.Token=0x11223344ULL;
    route.Value=0x55667788ULL;
    return RadioAdapters::RadioAdapterBindingResolutionStatus::Success;
}

struct FakeRuntime {
    bool Called=false;
    Primitive::PrimitiveFamilyId Family=0;
    Primitive::PrimitiveProtocolVersion Protocol=0;
    Adapters::AdapterServiceClass Service=Adapters::AdapterServiceClass::BestEffort;
    Adapters::AdapterSemanticProvenance Provenance{};
    Adapters::AdapterRouteToken Route{};
    Primitive::PrimitivePolicyDescriptor Policy{};
    std::array<std::uint8_t,8> Bytes{};
    std::size_t Size=0;

    Adapters::AdapterSubmissionDisposition AdmitTrustedInbound(
        Primitive::PrimitiveFamilyId family,Adapters::AdapterServiceClass service,
        Primitive::PrimitiveProtocolVersion protocol,Adapters::AdapterByteView borrowed,
        const Adapters::AdapterSemanticProvenance& provenance,Adapters::AdapterRouteToken route,
        Primitive::PrimitivePolicyDescriptor policy,std::uint64_t,
        Adapters::AdapterInboundCompletionTarget) noexcept {
        Called=true;Family=family;Service=service;Protocol=protocol;Provenance=provenance;Route=route;Policy=policy;
        Size=borrowed.Size;
        assert(Size<=Bytes.size());
        for(std::size_t i=0;i<Size;++i) Bytes[i]=borrowed.Data[i];
        return Adapters::AdapterSubmissionDisposition::Accepted;
    }
};

} // namespace

int main(){
    PolicyContext policyContext{};
    RadioAdapters::RadioAdapterBindingRegistry<2> registry;
    RadioAdapters::RadioAdapterBindingDescriptor binding{};
    binding.Family=0x1234;
    binding.Protocols={1,1};
    binding.Owner=&policyContext;
    binding.ResolvePolicy=&ResolvePolicy;
    assert(registry.Bind(binding)==Adapters::AdapterRuntimeStatus::Success);
    assert(registry.Bind(binding)==Adapters::AdapterRuntimeStatus::DuplicateFamily);
    assert(registry.Find(0x1234,1)==nullptr); // not visible until frozen
    assert(registry.Freeze()==Adapters::AdapterRuntimeStatus::Success);
    assert(registry.Find(0x1234,1)!=nullptr);
    assert(registry.Find(0x1234,2)==nullptr);
    assert(registry.Bind(binding)==Adapters::AdapterRuntimeStatus::Frozen);

    ProvenanceContext provenanceContext{};
    const RadioAdapters::RadioAdapterProvenanceBinding provenance{&provenanceContext,&ResolveProvenance};
    FakeRadio radio;
    const std::uint8_t sourceBytes[]{0x21,0x22};
    const auto source=Radio::RadioAddress::FromBytes(sourceBytes,2);

    std::array<std::uint8_t,6> message{};
    assert(RadioAdapters::EncodeDirectRadioPrimitivePrefix(0x1234,1,message.data(),message.size()));
    message[4]=0xAA;message[5]=0xBB;

    FakeRuntime runtime;
    auto disposition=RadioAdapters::AdmitDirectRadioLogicalMessage(
        runtime,registry,provenance,radio,source,Radio::RadioServiceClass::Critical,
        {message.data(),message.size()},77);
    assert(disposition==Adapters::AdapterSubmissionDisposition::Accepted);
    assert(runtime.Called);
    assert(runtime.Family==0x1234 && runtime.Protocol==1);
    assert(runtime.Service==Adapters::AdapterServiceClass::Critical);
    assert(runtime.Size==2 && runtime.Bytes[0]==0xAA && runtime.Bytes[1]==0xBB);
    assert(runtime.Provenance.ImmediatePeer.Token==0x11223344ULL);
    assert(!runtime.Provenance.OriginalSource); // trusted physical peer is not semantic-source proof
    assert(runtime.Route.Value==0x55667788ULL);
    assert(runtime.Policy.Category==1 && runtime.Policy.MaximumAttempts==1);

    runtime.Called=false;
    disposition=RadioAdapters::AdmitDirectRadioLogicalMessage(
        runtime,registry,provenance,radio,source,Radio::RadioServiceClass::Critical,
        {message.data(),3});
    assert(disposition==Adapters::AdapterSubmissionDisposition::Malformed && !runtime.Called);

    std::array<std::uint8_t,5> unsupported{};
    assert(RadioAdapters::EncodeDirectRadioPrimitivePrefix(0x4321,1,unsupported.data(),unsupported.size()));
    unsupported[4]=0x01;
    disposition=RadioAdapters::AdmitDirectRadioLogicalMessage(
        runtime,registry,provenance,radio,source,Radio::RadioServiceClass::Critical,
        {unsupported.data(),unsupported.size()});
    assert(disposition==Adapters::AdapterSubmissionDisposition::Unsupported);

    assert(RadioAdapters::EncodeDirectRadioPrimitivePrefix(0x1234,2,unsupported.data(),unsupported.size()));
    unsupported[4]=0x01;
    disposition=RadioAdapters::AdmitDirectRadioLogicalMessage(
        runtime,registry,provenance,radio,source,Radio::RadioServiceClass::Critical,
        {unsupported.data(),unsupported.size()});
    assert(disposition==Adapters::AdapterSubmissionDisposition::Unsupported);

    provenanceContext.reject=true;
    disposition=RadioAdapters::AdmitDirectRadioLogicalMessage(
        runtime,registry,provenance,radio,source,Radio::RadioServiceClass::Critical,
        {message.data(),message.size()});
    assert(disposition==Adapters::AdapterSubmissionDisposition::Rejected);
    provenanceContext.reject=false;

    policyContext.malformed=true;
    disposition=RadioAdapters::AdmitDirectRadioLogicalMessage(
        runtime,registry,provenance,radio,source,Radio::RadioServiceClass::Critical,
        {message.data(),message.size()});
    assert(disposition==Adapters::AdapterSubmissionDisposition::Malformed);

    return 0;
}
