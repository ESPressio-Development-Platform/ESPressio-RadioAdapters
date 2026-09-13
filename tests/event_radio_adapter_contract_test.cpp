#include <ESPressio_Adapters.hpp>
#include <ESPressio_EventRadioAdapterBinding.hpp>
#include <ESPressio_EventRadioAdapterOutboundTarget.hpp>
#include <ESPressio_RadioAdapterIngress.hpp>
#include <ESPressio_RadioAdapterLowerTransport.hpp>
#include <ESPressio_RadioAdapters.hpp>
#include <ESPressio_SerializationMacros.hpp>
#include <HostRuntime.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <thread>

using namespace ESPressio;
namespace E=ESPressio::Event;

namespace {

struct Delivery final {
    using PolicyCategory=Primitive::OccurrenceDeliveryPolicyTag;
    using RequiredEvidence=Primitive::NoRemoteEvidence;
    using TerminalDisposition=Primitive::DiagnosticOnlyAfterBudget;
    static constexpr std::uint64_t MaximumResidenceNanoseconds=0;
    static constexpr std::uint16_t MaximumAttempts=1;
    static constexpr std::uint64_t MaximumAdapterAdmissionWaitNanoseconds=0;
    static constexpr std::uint64_t MinimumRetrySpacingNanoseconds=0;
    static constexpr std::uint64_t MaximumRetrySpacingNanoseconds=0;
};

struct RadioEvent final : E::TransmissibleEvent<RadioEvent> {
    static constexpr E::EventTypeId TypeId{0x7A01};
    static constexpr std::size_t MaximumLiveInstances=4;
    static constexpr std::size_t MaximumPendingInstances=2;
    static constexpr std::string_view CanonicalName="RadioAdapters.Event.Direct";
    using DeliveryPolicy=Delivery;
    std::uint32_t Value=0;
    RadioEvent() noexcept=default;
    explicit RadioEvent(std::uint32_t value) noexcept:Value(value){}
    ESPRESSIO_SERIALIZABLE_TYPE(RadioEvent)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(ESPRESSIO_PROPERTY("value",Value))
};

System::DeviceRuntimeIdentity Identity(std::uint8_t marker,std::uint32_t runtime) {
    System::DeviceIdentifier::Storage bytes{};bytes[0]=marker;
    return {System::DeviceIdentifier{bytes},System::RuntimeIncarnationId{runtime}};
}

template<class Predicate>
void Eventually(Predicate&& predicate) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!predicate()) {
        assert(std::chrono::steady_clock::now()<deadline);
        std::this_thread::yield();
    }
}

using Arena=Adapters::StaticByteArena<Adapters::ByteClass<256,8>,Adapters::ByteClass<1024,4>>;
using Domain=Adapters::StaticCapacityDomain<1024,4,Arena>;
using Inbound=Adapters::CapacityPlane<Adapters::AdapterDirection::Inbound,
    Domain,Domain,Domain,Domain,Domain,Domain,Domain,Domain>;
using Outbound=Adapters::CapacityPlane<Adapters::AdapterDirection::Outbound,
    Domain,Domain,Domain,Domain,Domain,Domain,Domain,Domain>;
using AdapterRuntime=Adapters::AdapterRuntime<Inbound,Outbound,1,4,1,1,8>;

struct FakeRadioRuntime final {
    bool Running=true;
    Radio::RadioPeerHandle Peer{};
    Radio::RadioServiceProfile Profile{};
    Radio::RadioTransferTiming Timing{};
    std::array<std::uint8_t,1024> Bytes{};
    std::size_t Size=0;
    std::uint64_t Correlation=0;
    std::atomic<unsigned> Calls{0};

    bool IsRunning() const noexcept { return Running; }
    Radio::RadioTransferSubmissionResult SubmitPeer(
        Radio::RadioPeerHandle peer,const Radio::RadioServiceProfile& profile,
        const Radio::RadioTransferTiming& timing,const std::uint8_t* bytes,std::size_t size,
        std::uint64_t correlation=0) noexcept {
        assert(bytes&&size<=Bytes.size());
        Peer=peer;Profile=profile;Timing=timing;Size=size;Correlation=correlation;
        std::memcpy(Bytes.data(),bytes,size);
        ++Calls;
        return {Radio::RadioSchedulerStatus::Success,Radio::RadioTransferId{17}};
    }
};

struct RouteContext final { Radio::RadioPeerHandle Peer{3,7}; };
bool ResolveRoute(void* owner,Adapters::AdapterRouteToken route,Radio::RadioPeerHandle& peer) noexcept {
    auto& context=*static_cast<RouteContext*>(owner);
    if(route.Value!=0x7001ULL){peer={};return false;}
    peer=context.Peer;return true;
}
bool ValidateRoute(void*) noexcept { return true; }

bool ResolveTransferPolicy(
    void*,Primitive::PrimitiveFamilyId family,Primitive::PrimitiveProtocolVersion protocol,
    const Primitive::PrimitivePolicyDescriptor& policy,Adapters::AdapterServiceClass service,std::uint64_t now,
    Radio::RadioServiceProfile& profile,Radio::RadioTransferTiming& timing) noexcept {
    if(family!=Primitive::FamilyIds::Event||protocol!=E::EventProtocolVersion||policy.Evidence!=0||
       service!=Adapters::AdapterServiceClass::BestEffort) return false;
    profile.Class=RadioAdapters::ToRadioServiceClass(service);
    profile.DeadlineTreatment=Radio::RadioDeadlineTreatment::ExpiryOnly;
    profile.RequiredDirectLinkEvidence=Radio::RadioDirectLinkEvidenceRequirement::TransmissionCompletion;
    timing.ExpiryNanoseconds=now+1'000'000'000ULL;
    timing.ServiceDeadlineNanoseconds=0;
    return true;
}
bool ValidateTransferPolicy(void*) noexcept { return true; }

class FakeProvider final : public Radio::IRadio {
public:
    bool Start() override{return true;}
    void Stop() noexcept override{}
    bool IsStarted()const noexcept override{return true;}
    Radio::RadioCapabilities Capabilities()const noexcept override{return {};}
    Radio::RadioAddress LocalAddress()const noexcept override{const std::uint8_t b[2]{9,9};return Radio::RadioAddress::FromBytes(b,2);}
    Radio::RadioContentionDomainId ContentionDomain()const noexcept override{return {5};}
    Radio::RadioProviderResourceProfile ProviderResources()const noexcept override{return {};}
    bool IsTransmitReady()const noexcept override{return true;}
    Radio::RadioTransmissionCost EstimateTransmissionCost(const Radio::RadioAddress&,std::size_t n,const Radio::RadioServiceProfile&)const noexcept override{return {n+1,(n+1)*1000,Radio::RadioCostEstimateQuality::ConservativeAirtime};}
    Radio::RadioSendResult Send(const Radio::RadioAddress&,const std::uint8_t*,std::size_t)noexcept override{return Radio::RadioSendResult::Accepted(Radio::RadioDirectLinkEvidence::CompletedWithoutPeerAcknowledgement());}
    void SetReceiver(Radio::IRadioReceiver*)noexcept override{}
    void SetRuntimeSink(Radio::IRadioRuntimeSink*)noexcept override{}
    Radio::ManagedRadioIngressServiceResult ServiceInbound(std::size_t)noexcept override{return {};}
};

struct ProvenanceContext final { Adapters::AdapterRouteToken Route{0x7001}; };
RadioAdapters::RadioAdapterBindingResolutionStatus ResolveProvenance(
    void* owner,Radio::IRadio&,const Radio::RadioAddress& source,
    Adapters::AdapterSemanticProvenance& provenance,Adapters::AdapterRouteToken& route) noexcept {
    if(!source.IsValid()) return RadioAdapters::RadioAdapterBindingResolutionStatus::Malformed;
    auto& context=*static_cast<ProvenanceContext*>(owner);
    provenance={};provenance.ImmediatePeer.Token=0x9001;route=context.Route;
    return RadioAdapters::RadioAdapterBindingResolutionStatus::Success;
}

struct CompletionCapture final {
    std::atomic<bool> Done{false};
    Primitive::PrimitiveAdmissionDisposition Admission{Primitive::PrimitiveAdmissionDisposition::Rejected};
    static void Complete(void* owner,const Adapters::AdapterInboundCompletion& result) noexcept {
        auto& self=*static_cast<CompletionCapture*>(owner);
        self.Admission=result.Admission;
        self.Done.store(true,std::memory_order_release);
    }
};

} // namespace

int main(){
    HostRuntime platform;
    const auto local=Identity(1,17);
    const auto remote=Identity(2,31);
    assert(System::RuntimeIdentity::Install(local)==System::RuntimeIdentity::InstallationStatus::Success);

    Primitive::TypeDirectory<1> directory;
    assert(directory.Register<RadioEvent>()==Primitive::TypeDirectoryRegistrationStatus::Success);
    assert(directory.Initialize()==Primitive::TypeDirectoryInitializationStatus::Success);

    std::array<E::RemoteAdmissionReceipt,4> receipts{};
    E::RuntimeConfiguration eventConfiguration{};
    eventConfiguration.DispatchLane.Name="radioEvent";
    eventConfiguration.DispatchLane.StackSize=4096;
    eventConfiguration.RemoteAdmissionReceipts=receipts.data();
    eventConfiguration.RemoteAdmissionReceiptCapacity=receipts.size();
    E::Runtime eventRuntime(eventConfiguration);
    assert(eventRuntime.Initialize(directory.View())==E::EventRuntimeStatus::Success);

    RadioAdapters::EventRadioAdapterFamilyBinding<1> family;
    assert((family.BindType<RadioEvent,Serializable::DirectBinary>(
        eventRuntime,Adapters::AdapterServiceClass::BestEffort)==RadioAdapters::EventRadioAdapterBindingStatus::Success));
    assert(family.Freeze()==RadioAdapters::EventRadioAdapterBindingStatus::Success);

    RadioAdapters::RadioAdapterBindingRegistry<1> radioRegistry;
    assert(radioRegistry.Bind(family.RadioBinding())==Adapters::AdapterRuntimeStatus::Success);
    assert(radioRegistry.Freeze()==Adapters::AdapterRuntimeStatus::Success);

    FakeRadioRuntime radioRuntime;
    RouteContext routes;
    RadioAdapters::RadioAdapterRouteBinding routeBinding{&routes,&ResolveRoute,&ValidateRoute};
    RadioAdapters::RadioAdapterTransferPolicyBinding transferPolicy{
        &routes,&ResolveTransferPolicy,&ValidateTransferPolicy};
    RadioAdapters::RadioAdapterLowerTransport<FakeRadioRuntime,1024> lower(
        radioRuntime,routeBinding,transferPolicy);

    AdapterRuntime adapter;
    assert(adapter.BindFamily(family.AdapterBinding())==Adapters::AdapterRuntimeStatus::Success);
    assert(adapter.BindTransport(lower.AdapterBinding())==Adapters::AdapterRuntimeStatus::Success);
    Task::TaskExecutionConfiguration worker{};worker.Name="radioA2";worker.StackSize=4096;
    assert(adapter.Initialize(worker,worker)==Adapters::AdapterRuntimeStatus::Success);
    assert(adapter.Start()==Adapters::AdapterRuntimeStatus::Success);

    using Target=RadioAdapters::EventRadioAdapterOutboundTarget<
        AdapterRuntime,1,RadioEvent,Serializable::DirectBinary>;
    Target target(adapter,family,Adapters::AdapterRouteToken{0x7001});
    assert(target.Initialize()==E::EventRuntimeStatus::Success);
    assert(eventRuntime.Start()==E::EventRuntimeStatus::Success);

    const auto dispatched=RadioEvent::TryDispatch(73U);
    assert(dispatched.Status==E::EventDispatchStatus::Accepted);
    Eventually([&]{return radioRuntime.Calls.load(std::memory_order_acquire)>=1;});
    assert(radioRuntime.Peer==routes.Peer);
    assert(radioRuntime.Profile.Class==Radio::RadioServiceClass::BestEffort);
    assert(radioRuntime.Correlation==0);
    assert(radioRuntime.Size>RadioAdapters::DirectRadioPrimitivePrefixBytes+E::EventWireHeaderSize);

    RadioAdapters::DirectRadioPrimitivePrefix outer{};
    assert(RadioAdapters::DecodeDirectRadioPrimitivePrefix(radioRuntime.Bytes.data(),radioRuntime.Size,outer));
    assert(outer.Family==Primitive::FamilyIds::Event&&outer.Protocol==E::EventProtocolVersion);

    E::EventWireHeader outboundHeader{};
    const auto* eventBytes=radioRuntime.Bytes.data()+RadioAdapters::DirectRadioPrimitivePrefixBytes;
    const auto eventSize=radioRuntime.Size-RadioAdapters::DirectRadioPrimitivePrefixBytes;
    assert(E::DecodeEventWireHeader(eventBytes,eventSize,outboundHeader));
    assert(outboundHeader.Key.TypeId==RadioEvent::TypeId);
    assert(outboundHeader.Key.Origin==local);
    assert(outboundHeader.Key.MessageId==dispatched.MessageId);
    RadioEvent outboundPayload{};
    const auto decoded=Serializable::DeserializeBoundedDirectBinary(
        eventBytes+E::EventWireHeaderSize,eventSize-E::EventWireHeaderSize,outboundPayload);
    assert(decoded&&outboundPayload.Value==73U);

    FakeProvider provider;
    const std::uint8_t sourceBytes[2]{0x21,0x22};
    const auto source=Radio::RadioAddress::FromBytes(sourceBytes,2);
    ProvenanceContext provenanceContext{};
    const RadioAdapters::RadioAdapterProvenanceBinding provenance{&provenanceContext,&ResolveProvenance};

    RadioEvent remotePayload{91U};
    constexpr auto maximumEvent=E::MaximumCompletePrimitiveWireBytes<RadioEvent,Serializable::DirectBinary>;
    std::array<std::uint8_t,RadioAdapters::DirectRadioPrimitivePrefixBytes+maximumEvent> remoteWire{};
    assert(RadioAdapters::EncodeDirectRadioPrimitivePrefix(
        Primitive::FamilyIds::Event,E::EventProtocolVersion,remoteWire.data(),remoteWire.size()));
    auto* remoteEventWire=remoteWire.data()+RadioAdapters::DirectRadioPrimitivePrefixBytes;
    const auto remoteEncoded=Serializable::SerializeDirectBinary(
        remotePayload,remoteEventWire+E::EventWireHeaderSize,maximumEvent-E::EventWireHeaderSize);
    assert(remoteEncoded);
    const E::EventWireHeader remoteHeader{{RadioEvent::TypeId,remote,E::ConceptualMessageId{44}},
        {900,Timing::TimeReliability::Acquiring},static_cast<std::uint32_t>(remoteEncoded.Bytes)};
    assert(E::EncodeEventWireHeader(remoteHeader,remoteEventWire,maximumEvent));
    const auto remoteEventBytes=E::EventWireHeaderSize+remoteEncoded.Bytes;
    const auto remotePhysicalBytes=RadioAdapters::DirectRadioPrimitivePrefixBytes+remoteEventBytes;

    CompletionCapture accepted{};
    auto disposition=RadioAdapters::AdmitDirectRadioLogicalMessage(
        adapter,radioRegistry,provenance,provider,source,Radio::RadioServiceClass::BestEffort,
        {remoteWire.data(),remotePhysicalBytes},501,{&accepted,&CompletionCapture::Complete});
    assert(disposition==Adapters::AdapterSubmissionDisposition::Accepted);
    Eventually([&]{return accepted.Done.load(std::memory_order_acquire);});
    assert(accepted.Admission==Primitive::PrimitiveAdmissionDisposition::Accepted);
    Eventually([&]{return E::EventTypeRuntime<RadioEvent>::Get().LiveInstances()==0;});
    assert(radioRuntime.Calls.load(std::memory_order_acquire)==1); // remote origin never re-egresses

    disposition=RadioAdapters::AdmitDirectRadioLogicalMessage(
        adapter,radioRegistry,provenance,provider,source,Radio::RadioServiceClass::Critical,
        {remoteWire.data(),remotePhysicalBytes});
    assert(disposition==Adapters::AdapterSubmissionDisposition::Rejected);
    assert(radioRuntime.Calls.load(std::memory_order_acquire)==1);

    std::array<std::uint8_t,RadioAdapters::DirectRadioPrimitivePrefixBytes+maximumEvent> localReplay{};
    assert(RadioAdapters::EncodeDirectRadioPrimitivePrefix(
        Primitive::FamilyIds::Event,E::EventProtocolVersion,localReplay.data(),localReplay.size()));
    auto* replayEvent=localReplay.data()+RadioAdapters::DirectRadioPrimitivePrefixBytes;
    const auto replayPayload=Serializable::SerializeDirectBinary(
        remotePayload,replayEvent+E::EventWireHeaderSize,maximumEvent-E::EventWireHeaderSize);
    assert(replayPayload);
    const E::EventWireHeader replayHeader{{RadioEvent::TypeId,local,E::ConceptualMessageId{45}},
        {901,Timing::TimeReliability::Acquiring},static_cast<std::uint32_t>(replayPayload.Bytes)};
    assert(E::EncodeEventWireHeader(replayHeader,replayEvent,maximumEvent));
    const auto replaySize=RadioAdapters::DirectRadioPrimitivePrefixBytes+E::EventWireHeaderSize+replayPayload.Bytes;
    CompletionCapture rejected{};
    disposition=RadioAdapters::AdmitDirectRadioLogicalMessage(
        adapter,radioRegistry,provenance,provider,source,Radio::RadioServiceClass::BestEffort,
        {localReplay.data(),replaySize},502,{&rejected,&CompletionCapture::Complete});
    assert(disposition==Adapters::AdapterSubmissionDisposition::Accepted);
    Eventually([&]{return rejected.Done.load(std::memory_order_acquire);});
    assert(rejected.Admission==Primitive::PrimitiveAdmissionDisposition::Rejected);
    assert(radioRuntime.Calls.load(std::memory_order_acquire)==1);

    assert(eventRuntime.Shutdown()==E::EventRuntimeStatus::Success);
    target.Shutdown();
    assert(adapter.Shutdown()==Adapters::AdapterRuntimeStatus::Success);
    return 0;
}
