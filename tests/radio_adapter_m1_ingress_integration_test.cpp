#include <ESPressio_RadioAdapters.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

using namespace ESPressio;

namespace {

using Arena=Radio::RadioStaticByteArena<Radio::RadioByteClass<64,8>,Radio::RadioByteClass<256,4>>;
using Domain=Radio::RadioStaticCapacityDomain<384,4,Arena>;
using Inbound=Radio::RadioCapacityPlane<Radio::RadioCapacityDirection::Inbound,
    Domain,Domain,Domain,Domain,Domain,Domain,Domain,Domain>;
using Table=Radio::RadioReassemblyTable<Inbound,8,8>;

class Provider final : public Radio::IRadio {
public:
    bool Start() override{return true;}
    void Stop() noexcept override{}
    bool IsStarted() const noexcept override{return true;}
    Radio::RadioCapabilities Capabilities() const noexcept override{
        return {Radio::RadioCapability::HardwareAddressing,64,1,256};
    }
    Radio::RadioAddress LocalAddress() const noexcept override{
        const std::uint8_t bytes[2]{0x10,0x11};return Radio::RadioAddress::FromBytes(bytes,2);
    }
    Radio::RadioContentionDomainId ContentionDomain() const noexcept override{return {7};}
    Radio::RadioProviderResourceProfile ProviderResources() const noexcept override{return {4,2,0,0};}
    bool IsTransmitReady() const noexcept override{return true;}
    Radio::RadioTransmissionCost EstimateTransmissionCost(
        const Radio::RadioAddress&,std::size_t bytes,const Radio::RadioServiceProfile&) const noexcept override{
        return {bytes+1,(bytes+1)*1000,Radio::RadioCostEstimateQuality::ConservativeAirtime};
    }
    Radio::RadioSendResult Send(const Radio::RadioAddress&,const std::uint8_t*,std::size_t) noexcept override{
        return Radio::RadioSendResult::Accepted(Radio::RadioDirectLinkEvidence::CompletedWithoutPeerAcknowledgement());
    }
    void SetReceiver(Radio::IRadioReceiver*) noexcept override{}
    void SetRuntimeSink(Radio::IRadioRuntimeSink*) noexcept override{}
    Radio::ManagedRadioIngressServiceResult ServiceInbound(std::size_t) noexcept override{return {};}
};

struct PolicyContext final {};
RadioAdapters::RadioAdapterBindingResolutionStatus ResolvePolicy(
    void*,Primitive::PrimitiveProtocolVersion protocol,Adapters::AdapterByteView bytes,
    Primitive::PrimitivePolicyDescriptor& policy) noexcept {
    if(protocol!=1) return RadioAdapters::RadioAdapterBindingResolutionStatus::Unsupported;
    if(bytes.Data==nullptr||bytes.Size==0) return RadioAdapters::RadioAdapterBindingResolutionStatus::Malformed;
    policy={};policy.Category=1;policy.Evidence=1;policy.MaximumAttempts=1;
    return RadioAdapters::RadioAdapterBindingResolutionStatus::Success;
}

struct ProvenanceContext final { Adapters::AdapterRouteToken Route{0x55667788ULL}; };
RadioAdapters::RadioAdapterBindingResolutionStatus ResolveProvenance(
    void* owner,Radio::IRadio&,const Radio::RadioAddress& source,
    Adapters::AdapterSemanticProvenance& provenance,Adapters::AdapterRouteToken& route) noexcept {
    if(!source.IsValid()) return RadioAdapters::RadioAdapterBindingResolutionStatus::Malformed;
    auto& context=*static_cast<ProvenanceContext*>(owner);
    provenance={};provenance.ImmediatePeer.Token=0x11223344ULL;route=context.Route;
    return RadioAdapters::RadioAdapterBindingResolutionStatus::Success;
}

struct Runtime final {
    Adapters::AdapterSubmissionDisposition Next=Adapters::AdapterSubmissionDisposition::Accepted;
    std::size_t Calls=0;
    Primitive::PrimitiveFamilyId Family=0;
    std::uint64_t Correlation=0;
    Adapters::AdapterInboundCompletionTarget Completion{};

    Adapters::AdapterSubmissionDisposition AdmitTrustedInbound(
        Primitive::PrimitiveFamilyId family,Adapters::AdapterServiceClass,
        Primitive::PrimitiveProtocolVersion,Adapters::AdapterByteView,
        const Adapters::AdapterSemanticProvenance&,Adapters::AdapterRouteToken,
        Primitive::PrimitivePolicyDescriptor,std::uint64_t correlation,
        Adapters::AdapterInboundCompletionTarget completion) noexcept {
        ++Calls;Family=family;Correlation=correlation;Completion=completion;return Next;
    }

    void Complete(Primitive::PrimitiveAdmissionDisposition admission) noexcept {
        assert(Completion&&Correlation!=0);
        Completion.Complete(Completion.Owner,{Correlation,admission,Adapters::AdapterEvidence::DestinationPrimitiveAdmission});
        Completion={};Correlation=0;
    }
};

struct M1State final {
    std::size_t Sends=0;
    std::size_t Handles=0;
    Radio::IRadio* LastProvider=nullptr;
    Radio::RadioAddress LastSource{};
    Radio::RadioTransferId LastTransfer=0;
    Radio::RadioServiceClass LastService=Radio::RadioServiceClass::Invalid;
    Primitive::PrimitiveAdmissionDisposition LastAdmission=Primitive::PrimitiveAdmissionDisposition::Rejected;
    Adapters::AdapterRouteToken LastRoute{};
    Radio::RadioContentionDomainId LastDomain{};
    bool AcceptHandle=true;
};

bool HandleReceipt(void* owner,Adapters::AdapterRouteToken route,Radio::RadioContentionDomainId domain,
    Radio::RadioTransferId transfer,Primitive::PrimitiveAdmissionDisposition admission) noexcept {
    auto& state=*static_cast<M1State*>(owner);++state.Handles;state.LastRoute=route;state.LastDomain=domain;
    state.LastTransfer=transfer;state.LastAdmission=admission;return state.AcceptHandle;
}
bool SendReceipt(void* owner,Radio::IRadio& provider,const Radio::RadioAddress& source,
    Radio::RadioTransferId transfer,Radio::RadioServiceClass service,
    Primitive::PrimitiveAdmissionDisposition admission) noexcept {
    auto& state=*static_cast<M1State*>(owner);++state.Sends;state.LastProvider=&provider;state.LastSource=source;
    state.LastTransfer=transfer;state.LastService=service;state.LastAdmission=admission;return true;
}

Radio::RadioTransportV3FragmentView MakeFragment(
    Radio::RadioTransferId transfer,const Radio::RadioAddress& source,Radio::RadioServiceClass service,
    const std::uint8_t* payload,std::size_t payloadBytes,std::array<std::uint8_t,128>& wire) {
    Radio::RadioTransportV3Header header{transfer,0,1,static_cast<std::uint16_t>(payloadBytes),source,service,100};
    std::size_t encoded=0;
    assert(Radio::EncodeRadioTransportV3Fragment(header,payload,payloadBytes,wire.data(),wire.size(),encoded));
    Radio::RadioTransportV3FragmentView view{};
    assert(Radio::DecodeRadioTransportV3Fragment(wire.data(),encoded,view));
    return view;
}

void PutComplete(Table& table,Provider& provider,const Radio::RadioAddress& source,
    const Radio::RadioAddress& destination,Radio::RadioTransferId transfer,Radio::RadioServiceClass service,
    const std::uint8_t* payload,std::size_t payloadBytes) {
    std::array<std::uint8_t,128> wire{};
    const auto fragment=MakeFragment(transfer,source,service,payload,payloadBytes,wire);
    Radio::RadioPacketView packet{};packet.Source=source;packet.Destination=destination;packet.Flags=Radio::RadioPacketFlag::None;
    assert(table.Accept(provider,packet,fragment,1'000'000ULL+transfer,true)==Radio::RadioReassemblyStatus::Complete);
}

} // namespace

int main(){
    Inbound capacity;capacity.Initialize();Table table(capacity);Provider provider;
    const std::uint8_t sourceBytes[2]{0x21,0x22};const auto source=Radio::RadioAddress::FromBytes(sourceBytes,2);
    const std::uint8_t destinationBytes[2]{0x31,0x32};const auto destination=Radio::RadioAddress::FromBytes(destinationBytes,2);

    PolicyContext policy{};RadioAdapters::RadioAdapterBindingRegistry<2> registry;
    RadioAdapters::RadioAdapterBindingDescriptor descriptor{};descriptor.Family=0x1234;descriptor.Protocols={1,1};
    descriptor.Owner=&policy;descriptor.ResolvePolicy=&ResolvePolicy;
    assert(registry.Bind(descriptor)==Adapters::AdapterRuntimeStatus::Success);
    assert(registry.Freeze()==Adapters::AdapterRuntimeStatus::Success);

    ProvenanceContext provenanceState{};
    const RadioAdapters::RadioAdapterProvenanceBinding provenance{&provenanceState,&ResolveProvenance};
    Runtime runtime;M1State m1State;
    const RadioAdapters::RadioAdapterM1ReceiptBinding m1{&m1State,&HandleReceipt,&SendReceipt};
    RadioAdapters::RadioAdapterReassemblyIngress<Table,Runtime,2,1> ingress(table,runtime,registry,provenance,m1);

    std::array<std::uint8_t,6> data{};
    assert(RadioAdapters::EncodeDirectRadioPrimitivePrefix(0x1234,1,data.data(),data.size()));
    data[4]=0xAA;data[5]=0xBB;

    // Accepted Radio data is handed to A2 first; exact M1 is emitted only when A2 publishes semantic completion.
    PutComplete(table,provider,source,destination,41,Radio::RadioServiceClass::Responsive,data.data(),data.size());
    ingress.RadioReassemblyReady(provider,source,41,Radio::RadioServiceClass::Responsive);
    assert(runtime.Calls==1&&runtime.Family==0x1234&&runtime.Correlation!=0&&runtime.Completion);
    assert(m1State.Sends==0);

    // The one-slot receipt context is intentionally held. A second completed message receives ResourceUnavailable
    // without another A2 admission, proving bounded nonblocking saturation.
    PutComplete(table,provider,source,destination,42,Radio::RadioServiceClass::Responsive,data.data(),data.size());
    ingress.RadioReassemblyReady(provider,source,42,Radio::RadioServiceClass::Responsive);
    assert(runtime.Calls==1);
    assert(m1State.Sends==1&&m1State.LastTransfer==42);
    assert(m1State.LastAdmission==Primitive::PrimitiveAdmissionDisposition::ResourceUnavailable);

    runtime.Complete(Primitive::PrimitiveAdmissionDisposition::Accepted);
    assert(m1State.Sends==2&&m1State.LastProvider==&provider&&m1State.LastSource==source);
    assert(m1State.LastTransfer==41&&m1State.LastService==Radio::RadioServiceClass::Responsive);
    assert(m1State.LastAdmission==Primitive::PrimitiveAdmissionDisposition::Accepted);
    assert(ingress.ReceiptsSentCount()==2);

    // Immediate family-resolution failure maps directly to exact M1 and never enters A2.
    std::array<std::uint8_t,5> unsupported{};
    assert(RadioAdapters::EncodeDirectRadioPrimitivePrefix(0x9999,1,unsupported.data(),unsupported.size()));
    unsupported[4]=0x01;
    PutComplete(table,provider,source,destination,43,Radio::RadioServiceClass::Critical,unsupported.data(),unsupported.size());
    ingress.RadioReassemblyReady(provider,source,43,Radio::RadioServiceClass::Critical);
    assert(runtime.Calls==1);
    assert(m1State.Sends==3&&m1State.LastTransfer==43);
    assert(m1State.LastAdmission==Primitive::PrimitiveAdmissionDisposition::Unsupported);

    // M1 control receipts are intercepted before family demux and cannot recursively generate a receipt.
    std::array<std::uint8_t,RadioAdapters::DirectRadioM1ReceiptBytes> receipt{};
    assert(RadioAdapters::EncodeDirectRadioM1Receipt(77,Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted,
        receipt.data(),receipt.size()));
    PutComplete(table,provider,source,destination,44,Radio::RadioServiceClass::Infrastructure,receipt.data(),receipt.size());
    ingress.RadioReassemblyReady(provider,source,44,Radio::RadioServiceClass::Infrastructure);
    assert(runtime.Calls==1&&m1State.Handles==1&&m1State.Sends==3);
    assert(m1State.LastRoute.Value==provenanceState.Route.Value&&m1State.LastDomain==provider.ContentionDomain());
    assert(m1State.LastTransfer==77&&m1State.LastAdmission==Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted);
    assert(ingress.ReceiptsConsumedCount()==1);

    // A receipt that fails route/correlation validation is rejected locally and still does not recurse.
    m1State.AcceptHandle=false;
    assert(RadioAdapters::EncodeDirectRadioM1Receipt(78,Primitive::PrimitiveAdmissionDisposition::Accepted,
        receipt.data(),receipt.size()));
    PutComplete(table,provider,source,destination,45,Radio::RadioServiceClass::Infrastructure,receipt.data(),receipt.size());
    ingress.RadioReassemblyReady(provider,source,45,Radio::RadioServiceClass::Infrastructure);
    assert(runtime.Calls==1&&m1State.Handles==2&&m1State.Sends==3);
    assert(ingress.ReceiptsConsumedCount()==1);

    assert(ingress.AcceptedCount()==2); // transfer 41 plus consumed control receipt 44
    assert(ingress.RejectedCount()==3); // saturation 42, unsupported 43, rejected control receipt 45
    return 0;
}
