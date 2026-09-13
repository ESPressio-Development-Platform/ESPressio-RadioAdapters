#include <ESPressio_Adapters.hpp>
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
    using RequiredEvidence=Primitive::DestinationPrimitiveAdmission;
    using Supersession=Primitive::LatestAuthoritativeValue;
    using ExhaustionDisposition=Primitive::DormantNeedsConvergence;
    static constexpr std::uint64_t MaximumResidenceNanoseconds=1'000'000'000ULL;
    static constexpr std::uint64_t MaximumAdapterAdmissionWaitNanoseconds=1'000'000ULL;
    static constexpr std::uint16_t MaximumAttempts=1;
    static constexpr std::uint64_t MinimumRetrySpacingNanoseconds=0;
    static constexpr std::uint64_t MaximumRetrySpacingNanoseconds=0;
};
struct TestState final : S::TransmissibleState<TestState,Value> {
    static constexpr S::StateTypeId TypeId{0x6901};
    static constexpr std::string_view CanonicalName="RadioAdapters.State.Outbound";
    using ConvergencePolicy=Policy;
};
System::DeviceRuntimeIdentity Identity(std::uint8_t marker,std::uint32_t runtime) {
    System::DeviceIdentifier::Storage bytes{};bytes.back()=marker;
    return {System::DeviceIdentifier{bytes},System::RuntimeIncarnationId{runtime}};
}
Timing::QualifiedTime Capture() { return {1000,Timing::TimeReliability::Synchronized}; }
template<class Predicate> void Eventually(Predicate&& predicate) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!predicate()) { assert(std::chrono::steady_clock::now()<deadline);std::this_thread::yield(); }
}
struct RouteResolver final {
    static bool Validate(void*) noexcept { return true; }
    static bool Resolve(void*,System::DeviceIdentifier device,Adapters::AdapterRouteToken& route) noexcept {
        if(!device) return false;
        route.Value=0xE000U+device.Bytes().back();
        return true;
    }
};
struct Signal final { std::atomic<unsigned> Wakes{0};static void Wake(void* owner) noexcept { ++static_cast<Signal*>(owner)->Wakes; } };
using Arena=Adapters::StaticByteArena<Adapters::ByteClass<256,8>,Adapters::ByteClass<1024,4>>;
using Domain=Adapters::StaticCapacityDomain<1024,4,Arena>;
using Inbound=Adapters::CapacityPlane<Adapters::AdapterDirection::Inbound,Domain,Domain,Domain,Domain,Domain,Domain,Domain,Domain>;
using Outbound=Adapters::CapacityPlane<Adapters::AdapterDirection::Outbound,Domain,Domain,Domain,Domain,Domain,Domain,Domain,Domain>;
using A2=Adapters::AdapterRuntime<Inbound,Outbound,2,4,1,1,8>;
struct Lower final {
    std::array<std::uint8_t,512> Wire{};std::size_t Bytes=0;Adapters::AdapterRouteToken Route{};
    Adapters::AdapterServiceClass Service{Adapters::AdapterServiceClass::BestEffort};std::atomic<unsigned> Calls{0};
    static bool Validate(void*) noexcept { return true; }
    static Adapters::LowerTransportSubmitResult Submit(void* owner,Adapters::AdapterRecordIdentity,
        Primitive::PrimitiveFamilyId family,Primitive::PrimitiveProtocolVersion protocol,
        const Primitive::PrimitivePolicyDescriptor&,Adapters::AdapterServiceClass service,
        Adapters::AdapterByteView bytes,Adapters::AdapterRouteToken route) noexcept {
        auto& self=*static_cast<Lower*>(owner);assert(family==Primitive::FamilyIds::State);assert(protocol==S::StateProtocolVersion);
        assert(bytes.Data&&bytes.Size<=self.Wire.size());std::memcpy(self.Wire.data(),bytes.Data,bytes.Size);
        self.Bytes=bytes.Size;self.Route=route;self.Service=service;const auto generation=++self.Calls;
        return {Adapters::LowerTransportDisposition::PermanentlyRejected,generation,false};
    }
    Adapters::LowerTransportBinding Binding() noexcept {
        Adapters::LowerTransportBinding binding{};binding.Owner=this;binding.Submit=&Lower::Submit;binding.Validate=&Lower::Validate;
        binding.ServiceClassMask=0x3f;binding.ProvidesDestinationPrimitiveAdmission=true;binding.ProvidesValidatedOriginalSource=false;return binding;
    }
};
} // namespace

int main() {
    HostRuntime platform;const auto local=Identity(1,11);const auto remote=Identity(2,22);
    assert(System::RuntimeIdentity::Install(local)==System::RuntimeIdentity::InstallationStatus::Success);
    Primitive::TypeDirectory<1> directory;assert(directory.Register<TestState>()==Primitive::TypeDirectoryRegistrationStatus::Success);
    assert(directory.Initialize()==Primitive::TypeDirectoryInitializationStatus::Success);
    using Runtime=S::Runtime<S::TypeConfiguration<TestState,S::MaximumRemoteOwners<1>,S::MaximumSubscribers<1>>>;
    Runtime runtime;auto owner=runtime.BindOwner<TestState>();assert(owner);
    A2 adapter;RouteResolver routes;Signal signal;
    const RadioAdapters::RadioAdapterSemanticRouteBinding routeBinding{&routes,&RouteResolver::Resolve,&RouteResolver::Validate};
    RadioAdapters::StateRadioAdapterFamilyBinding<Runtime,1,2> family;
    assert((family.BindType<TestState,Serializable::DirectBinary>(runtime,Adapters::AdapterServiceClass::Convergent)==
            RadioAdapters::StateRadioAdapterBindingStatus::Success));
    assert(family.BindAdapter(adapter,routeBinding,{&signal,&Signal::Wake})==RadioAdapters::StateRadioAdapterBindingStatus::Success);
    S::StateTransportBinding<TestState,Serializable::DirectBinary> transport;
    assert((family.InitializeTransport<TestState,Serializable::DirectBinary>(transport)));
    assert(runtime.BindTransport(transport)==S::StateRuntimeStatus::Success);
    assert(runtime.Initialize(directory.View(),&Capture)==S::StateRuntimeStatus::Success);
    assert(family.Freeze()==RadioAdapters::StateRadioAdapterBindingStatus::Success);
    assert((family.ValidateTransport<TestState,Serializable::DirectBinary>(transport.Contract())));

    Lower lower;assert(adapter.BindFamily(family.AdapterBinding())==Adapters::AdapterRuntimeStatus::Success);
    assert(adapter.BindTransport(lower.Binding())==Adapters::AdapterRuntimeStatus::Success);
    Task::TaskExecutionConfiguration worker{};worker.Name="stateRadioA2";worker.StackSize=4096;
    assert(adapter.Initialize(worker,worker)==Adapters::AdapterRuntimeStatus::Success);
    assert(adapter.Start()==Adapters::AdapterRuntimeStatus::Success);
    assert(runtime.Start()==S::StateRuntimeStatus::Success);

    assert(owner.Set({10},Capture())==S::StateSetStatus::Changed);
    const S::StateSessionToken session{77};
    assert(runtime.ReserveSourceSubscriber<TestState>(remote,session)==S::StateRemoteStatus::Success);
    assert(runtime.ActivateSourceSubscriber<TestState>(remote,session,true,runtime.Version<TestState>())==S::StateRemoteStatus::Success);
    assert(owner.Set({20},Capture())==S::StateSetStatus::Changed);
    const auto wakesBefore=signal.Wakes.load();assert(family.Service());Eventually([&]{return lower.Calls.load()>=1;});
    assert(lower.Route.Value==0xE002U);assert(lower.Service==Adapters::AdapterServiceClass::Convergent);
    S::StatePublicationWireHeader header{};assert(S::DecodeStatePublicationHeader(lower.Wire.data(),lower.Bytes,header));
    assert(header.TypeId==TestState::TypeId&&header.Owner==local&&header.Requester==remote&&header.Session==session);
    Value value{};const auto decoded=S::DecodeStatePublicationValue<TestState,Serializable::DirectBinary>(
        header,lower.Wire.data()+S::StatePublicationWireHeaderSize,lower.Bytes-S::StatePublicationWireHeaderSize,value);
    assert(decoded&&value.Number==20);
    Eventually([&]{return signal.Wakes.load()>wakesBefore;});assert(family.Service());
    assert(runtime.NeedsConvergence<TestState>(remote.Device,S::StateContinuitySide::SourceSubscriber));
    assert(!runtime.ServiceLatest<TestState>());
    assert(runtime.Shutdown()==S::StateRuntimeStatus::Success);assert(adapter.Shutdown()==Adapters::AdapterRuntimeStatus::Success);
    return 0;
}
