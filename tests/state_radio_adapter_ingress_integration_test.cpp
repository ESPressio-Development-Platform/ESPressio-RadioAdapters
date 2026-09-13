#include <ESPressio_Adapters.hpp>
#include <ESPressio_RadioAdapterEnvelope.hpp>
#include <ESPressio_RadioAdapterIngress.hpp>
#include <ESPressio_RuntimeIdentity.hpp>
#include <ESPressio_SerializationMacros.hpp>
#include <ESPressio_StateRadioAdapterBinding.hpp>
#include <ESPressio_TypeDirectory.hpp>
#include <HostRuntime.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <thread>

using namespace ESPressio;
namespace S=ESPressio::State;

namespace {
struct Value final {
    std::uint32_t Number=0;
    constexpr bool operator==(const Value& other) const noexcept { return Number==other.Number; }
    ESPRESSIO_SERIALIZABLE_TYPE(Value)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(ESPRESSIO_PROPERTY("number",Number))
};
struct Policy final {
    using PolicyCategory=Primitive::StateConvergencePolicyTag;
    using RequiredEvidence=Primitive::NoRemoteEvidence;
    using Supersession=Primitive::LatestAuthoritativeValue;
    using ExhaustionDisposition=Primitive::DormantNeedsConvergence;
    static constexpr std::uint64_t MaximumResidenceNanoseconds=0;
    static constexpr std::uint16_t MaximumAttempts=1;
    static constexpr std::uint64_t MaximumAdapterAdmissionWaitNanoseconds=0;
    static constexpr std::uint64_t MinimumRetrySpacingNanoseconds=0;
    static constexpr std::uint64_t MaximumRetrySpacingNanoseconds=0;
};
struct RemoteState final : S::TransmissibleState<RemoteState,Value> {
    static constexpr S::StateTypeId TypeId{0x6903};
    static constexpr std::string_view CanonicalName="RadioAdapters.State.IngressIntegration";
    using ConvergencePolicy=Policy;
};
System::DeviceRuntimeIdentity Identity(std::uint8_t marker,std::uint32_t runtime) {
    System::DeviceIdentifier::Storage bytes{};
    bytes.back()=marker;
    return {System::DeviceIdentifier{bytes},System::RuntimeIncarnationId{runtime}};
}
Timing::QualifiedTime Capture() { return {500,Timing::TimeReliability::Synchronized}; }
template<class Predicate> void Eventually(Predicate&& predicate) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!predicate()) {
        assert(std::chrono::steady_clock::now()<deadline);
        std::this_thread::yield();
    }
}
struct RouteResolver final {
    static bool Validate(void*) noexcept { return true; }
    static bool Resolve(void*,System::DeviceIdentifier device,Adapters::AdapterRouteToken& route) noexcept {
        if(!device) return false;
        route.Value=0xD000U+device.Bytes().back();
        return true;
    }
};
struct Signal final { static void Wake(void*) noexcept {} };
using Arena=Adapters::StaticByteArena<Adapters::ByteClass<256,6>,Adapters::ByteClass<1024,2>>;
using Domain=Adapters::StaticCapacityDomain<1024,2,Arena>;
using Inbound=Adapters::CapacityPlane<Adapters::AdapterDirection::Inbound,Domain,Domain,Domain,Domain,Domain,Domain,Domain,Domain>;
using Outbound=Adapters::CapacityPlane<Adapters::AdapterDirection::Outbound,Domain,Domain,Domain,Domain,Domain,Domain,Domain,Domain>;
using A2=Adapters::AdapterRuntime<Inbound,Outbound,1,2,1,1,4>;

struct DummyLower final {
    static bool Validate(void*) noexcept { return true; }
    static Adapters::LowerTransportSubmitResult Submit(
        void*,Adapters::AdapterRecordIdentity,Primitive::PrimitiveFamilyId,Primitive::PrimitiveProtocolVersion,
        const Primitive::PrimitivePolicyDescriptor&,Adapters::AdapterServiceClass,Adapters::AdapterByteView,
        Adapters::AdapterRouteToken) noexcept {
        return {Adapters::LowerTransportDisposition::Accepted,0,false};
    }
    Adapters::LowerTransportBinding Binding() noexcept {
        Adapters::LowerTransportBinding binding{};
        binding.Owner=this;
        binding.Submit=&DummyLower::Submit;
        binding.Validate=&DummyLower::Validate;
        binding.ServiceClassMask=0x3f;
        return binding;
    }
};

class FakeProvider final : public Radio::IRadio {
public:
    bool Start() override { return true; }
    void Stop() noexcept override {}
    bool IsStarted() const noexcept override { return true; }
    Radio::RadioCapabilities Capabilities() const noexcept override { return {}; }
    Radio::RadioAddress LocalAddress() const noexcept override {
        const std::uint8_t bytes[2]{9,9};
        return Radio::RadioAddress::FromBytes(bytes,2);
    }
    Radio::RadioContentionDomainId ContentionDomain() const noexcept override { return {5}; }
    Radio::RadioProviderResourceProfile ProviderResources() const noexcept override { return {}; }
    bool IsTransmitReady() const noexcept override { return true; }
    Radio::RadioTransmissionCost EstimateTransmissionCost(
        const Radio::RadioAddress&,std::size_t size,const Radio::RadioServiceProfile&) const noexcept override {
        return {size+1,(size+1)*1000,Radio::RadioCostEstimateQuality::ConservativeAirtime};
    }
    Radio::RadioSendResult Send(const Radio::RadioAddress&,const std::uint8_t*,std::size_t) noexcept override {
        return Radio::RadioSendResult::Accepted(Radio::RadioDirectLinkEvidence::CompletedWithoutPeerAcknowledgement());
    }
    void SetReceiver(Radio::IRadioReceiver*) noexcept override {}
    void SetRuntimeSink(Radio::IRadioRuntimeSink*) noexcept override {}
    Radio::ManagedRadioIngressServiceResult ServiceInbound(std::size_t) noexcept override { return {}; }
};

struct ProvenanceContext final {
    System::DeviceRuntimeIdentity Source{};
    Adapters::AdapterRouteToken Route{};
};
RadioAdapters::RadioAdapterBindingResolutionStatus ResolveProvenance(
    void* owner,Radio::IRadio&,const Radio::RadioAddress& source,
    Adapters::AdapterSemanticProvenance& provenance,Adapters::AdapterRouteToken& route) noexcept {
    if(!source.IsValid()) return RadioAdapters::RadioAdapterBindingResolutionStatus::Malformed;
    auto& context=*static_cast<ProvenanceContext*>(owner);
    provenance={};
    provenance.ImmediatePeer.Token=context.Route.Value;
    provenance.OriginalSource={context.Source,true};
    route=context.Route;
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

int main() {
    using Format=Serializable::DirectBinary;
    HostRuntime platform;
    const auto local=Identity(1,11);
    const auto remote=Identity(2,41);
    const auto forged=Identity(3,99);
    assert(System::RuntimeIdentity::Install(local)==System::RuntimeIdentity::InstallationStatus::Success);

    Primitive::TypeDirectory<1> directory;
    assert(directory.Register<RemoteState>()==Primitive::TypeDirectoryRegistrationStatus::Success);
    assert(directory.Initialize()==Primitive::TypeDirectoryInitializationStatus::Success);
    using Runtime=S::Runtime<S::TypeConfiguration<RemoteState,S::MaximumRemoteOwners<1>,S::MaximumSubscribers<0>>>;
    Runtime runtime;
    assert(runtime.Initialize(directory.View(),&Capture)==S::StateRuntimeStatus::Success);
    assert(runtime.Start()==S::StateRuntimeStatus::Success);
    const auto reserved=runtime.ReserveSubscriptionSession<RemoteState>(remote.Device);
    assert(reserved);
    const S::StateSnapshot<RemoteState> baseline{{10},{100,Timing::TimeReliability::Synchronized}};
    assert(runtime.InstallSubscribeSnapshot<RemoteState>(remote,reserved.Handle.Session,{false,1},baseline)==S::StateRemoteStatus::Success);

    A2 adapter;
    RouteResolver routes;
    RadioAdapters::StateRadioAdapterFamilyBinding<Runtime,1> family;
    assert((family.BindType<RemoteState,Format>(runtime,Adapters::AdapterServiceClass::Convergent)==
            RadioAdapters::StateRadioAdapterBindingStatus::Success));
    assert(family.BindAdapter(adapter,{&routes,&RouteResolver::Resolve,&RouteResolver::Validate},{&adapter,&Signal::Wake})==
           RadioAdapters::StateRadioAdapterBindingStatus::Success);
    assert(family.Freeze()==RadioAdapters::StateRadioAdapterBindingStatus::Success);

    RadioAdapters::RadioAdapterBindingRegistry<1> registry;
    assert(registry.Bind(family.RadioBinding())==Adapters::AdapterRuntimeStatus::Success);
    assert(registry.Freeze()==Adapters::AdapterRuntimeStatus::Success);
    assert(adapter.BindFamily(family.AdapterBinding())==Adapters::AdapterRuntimeStatus::Success);
    DummyLower lower;
    assert(adapter.BindTransport(lower.Binding())==Adapters::AdapterRuntimeStatus::Success);
    Task::TaskExecutionConfiguration worker{};
    worker.Name="stateRadioIngressA2";
    worker.StackSize=4096;
    assert(adapter.Initialize(worker,worker)==Adapters::AdapterRuntimeStatus::Success);
    assert(adapter.Start()==Adapters::AdapterRuntimeStatus::Success);

    const S::StateSnapshot<RemoteState> newer{{42},{200,Timing::TimeReliability::Holdover}};
    std::array<std::uint8_t,S::MaximumCompleteStatePublicationWireBytes<RemoteState,Format>> stateWire{};
    const auto encoded=S::EncodeStatePublication<RemoteState,Format>(
        newer,{false,2},remote,local,reserved.Handle.Session,stateWire.data(),stateWire.size());
    assert(encoded);
    std::array<std::uint8_t,RadioAdapters::DirectRadioPrimitivePrefixBytes+stateWire.size()> logical{};
    assert(RadioAdapters::EncodeDirectRadioPrimitivePrefix(
        Primitive::FamilyIds::State,S::StateProtocolVersion,logical.data(),logical.size()));
    std::memcpy(logical.data()+RadioAdapters::DirectRadioPrimitivePrefixBytes,stateWire.data(),encoded.Bytes);
    const auto logicalBytes=RadioAdapters::DirectRadioPrimitivePrefixBytes+encoded.Bytes;

    FakeProvider provider;
    const std::uint8_t sourceBytes[2]{7,7};
    const auto source=Radio::RadioAddress::FromBytes(sourceBytes,2);
    ProvenanceContext provenanceContext{remote,Adapters::AdapterRouteToken{0xD002}};
    const RadioAdapters::RadioAdapterProvenanceBinding provenance{&provenanceContext,&ResolveProvenance};

    CompletionCapture accepted{};
    auto disposition=RadioAdapters::AdmitDirectRadioLogicalMessage(
        adapter,registry,provenance,provider,source,Radio::RadioServiceClass::Convergent,
        {logical.data(),logicalBytes},701,{&accepted,&CompletionCapture::Complete});
    assert(disposition==Adapters::AdapterSubmissionDisposition::Accepted);
    Eventually([&]{return accepted.Done.load(std::memory_order_acquire);});
    assert(accepted.Admission==Primitive::PrimitiveAdmissionDisposition::Accepted);
    S::StateSnapshot<RemoteState> read{};
    assert(runtime.TryReadRemote<RemoteState>(remote.Device,read));
    assert(read.Value.Number==42&&read.TruthTime.Nanoseconds==200);

    CompletionCapture duplicate{};
    disposition=RadioAdapters::AdmitDirectRadioLogicalMessage(
        adapter,registry,provenance,provider,source,Radio::RadioServiceClass::Convergent,
        {logical.data(),logicalBytes},702,{&duplicate,&CompletionCapture::Complete});
    assert(disposition==Adapters::AdapterSubmissionDisposition::Accepted);
    Eventually([&]{return duplicate.Done.load(std::memory_order_acquire);});
    assert(duplicate.Admission==Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted);

    disposition=RadioAdapters::AdmitDirectRadioLogicalMessage(
        adapter,registry,provenance,provider,source,Radio::RadioServiceClass::Responsive,
        {logical.data(),logicalBytes});
    assert(disposition==Adapters::AdapterSubmissionDisposition::Rejected);

    provenanceContext.Source=forged;
    CompletionCapture forgedCapture{};
    disposition=RadioAdapters::AdmitDirectRadioLogicalMessage(
        adapter,registry,provenance,provider,source,Radio::RadioServiceClass::Convergent,
        {logical.data(),logicalBytes},703,{&forgedCapture,&CompletionCapture::Complete});
    assert(disposition==Adapters::AdapterSubmissionDisposition::Accepted);
    Eventually([&]{return forgedCapture.Done.load(std::memory_order_acquire);});
    assert(forgedCapture.Admission==Primitive::PrimitiveAdmissionDisposition::Rejected);
    assert(runtime.TryReadRemote<RemoteState>(remote.Device,read));
    assert(read.Value.Number==42);

    assert(adapter.Shutdown()==Adapters::AdapterRuntimeStatus::Success);
    assert(runtime.Shutdown()==S::StateRuntimeStatus::Success);
    return 0;
}
