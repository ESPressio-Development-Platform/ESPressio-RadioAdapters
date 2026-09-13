#include <ESPressio_Adapters.hpp>
#include <ESPressio_RuntimeIdentity.hpp>
#include <ESPressio_SerializationMacros.hpp>
#include <ESPressio_StateRadioAdapterBinding.hpp>
#include <ESPressio_TypeDirectory.hpp>
#include <HostRuntime.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <string_view>

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
    static constexpr S::StateTypeId TypeId{0x6902};
    static constexpr std::string_view CanonicalName="RadioAdapters.State.Inbound";
    using ConvergencePolicy=Policy;
};
System::DeviceRuntimeIdentity Identity(std::uint8_t marker,std::uint32_t runtime) {
    System::DeviceIdentifier::Storage bytes{};
    bytes.back()=marker;
    return {System::DeviceIdentifier{bytes},System::RuntimeIncarnationId{runtime}};
}
Timing::QualifiedTime Capture() { return {500,Timing::TimeReliability::Synchronized}; }
struct RouteResolver final {
    static bool Validate(void*) noexcept { return true; }
    static bool Resolve(void*,System::DeviceIdentifier device,Adapters::AdapterRouteToken& route) noexcept {
        if(!device) return false;
        route.Value=0xD000U+device.Bytes().back();
        return true;
    }
};
struct Signal final { static void Wake(void*) noexcept {} };
using Arena=Adapters::StaticByteArena<Adapters::ByteClass<256,4>,Adapters::ByteClass<1024,2>>;
using Domain=Adapters::StaticCapacityDomain<1024,2,Arena>;
using Inbound=Adapters::CapacityPlane<Adapters::AdapterDirection::Inbound,Domain,Domain,Domain,Domain,Domain,Domain,Domain,Domain>;
using Outbound=Adapters::CapacityPlane<Adapters::AdapterDirection::Outbound,Domain,Domain,Domain,Domain,Domain,Domain,Domain,Domain>;
using A2=Adapters::AdapterRuntime<Inbound,Outbound,1,2,1,1,4>;
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
    assert(reserved&&reserved.Handle.OwnerDevice==remote.Device);
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

    const auto binding=family.AdapterBinding();
    const auto radioBinding=family.RadioBinding();
    assert(binding.IsValid()&&radioBinding.IsValid());
    assert(!binding.RequiresValidatedOriginalSource);

    const S::StateSnapshot<RemoteState> newer{{42},{200,Timing::TimeReliability::Holdover}};
    std::array<std::uint8_t,S::MaximumCompleteStatePublicationWireBytes<RemoteState,Format>> publication{};
    const auto encoded=S::EncodeStatePublication<RemoteState,Format>(newer,{false,2},remote,local,reserved.Handle.Session,
                                                                     publication.data(),publication.size());
    assert(encoded);

    Primitive::PrimitivePolicyDescriptor policy{};
    assert(radioBinding.ResolvePolicy(radioBinding.Owner,S::StateProtocolVersion,Adapters::AdapterServiceClass::Convergent,
        {publication.data(),encoded.Bytes},policy)==RadioAdapters::RadioAdapterBindingResolutionStatus::Success);
    assert(policy.Category==2&&policy.Evidence==0);
    assert(radioBinding.ResolvePolicy(radioBinding.Owner,S::StateProtocolVersion,Adapters::AdapterServiceClass::Responsive,
        {publication.data(),encoded.Bytes},policy)==RadioAdapters::RadioAdapterBindingResolutionStatus::Rejected);

    Adapters::AdapterSemanticProvenance provenance{};
    provenance.ImmediatePeer={0x1234};
    provenance.OriginalSource={remote,true};
    assert(binding.AdmitInbound(binding.Owner,S::StateProtocolVersion,
        {publication.data(),encoded.Bytes},provenance)==Primitive::PrimitiveAdmissionDisposition::Accepted);
    S::StateSnapshot<RemoteState> read{};
    assert(runtime.TryReadRemote<RemoteState>(remote.Device,read));
    assert(read.Value.Number==42&&read.TruthTime.Nanoseconds==200);

    assert(binding.AdmitInbound(binding.Owner,S::StateProtocolVersion,
        {publication.data(),encoded.Bytes},provenance)==Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted);

    Adapters::AdapterSemanticProvenance forgedProvenance{};
    forgedProvenance.ImmediatePeer={0x1234};
    forgedProvenance.OriginalSource={forged,true};
    assert(binding.AdmitInbound(binding.Owner,S::StateProtocolVersion,
        {publication.data(),encoded.Bytes},forgedProvenance)==Primitive::PrimitiveAdmissionDisposition::Rejected);

    Adapters::AdapterSemanticProvenance unvalidated{};
    unvalidated.ImmediatePeer={0x1234};
    unvalidated.OriginalSource={remote,false};
    assert(binding.AdmitInbound(binding.Owner,S::StateProtocolVersion,
        {publication.data(),encoded.Bytes},unvalidated)==Primitive::PrimitiveAdmissionDisposition::Rejected);

    S::StateControlWireHeader request{};
    request.Kind=S::StateMessageKind::SubscribeRequest;
    request.TypeId=RemoteState::TypeId;
    request.Owner=local;
    request.Requester=remote;
    request.Session=S::StateSessionToken{77};
    std::array<std::uint8_t,S::StateControlWireHeaderSize> control{};
    const auto controlEncoded=S::EncodeStateControl(request,control.data(),control.size());
    assert(controlEncoded);
    assert(radioBinding.ResolvePolicy(radioBinding.Owner,S::StateProtocolVersion,Adapters::AdapterServiceClass::Convergent,
        {control.data(),controlEncoded.Bytes},policy)==RadioAdapters::RadioAdapterBindingResolutionStatus::Success);

    request.Requester=forged;
    const auto forgedControl=S::EncodeStateControl(request,control.data(),control.size());
    assert(forgedControl);
    assert(binding.AdmitInbound(binding.Owner,S::StateProtocolVersion,
        {control.data(),forgedControl.Bytes},provenance)==Primitive::PrimitiveAdmissionDisposition::Rejected);

    assert(runtime.Shutdown()==S::StateRuntimeStatus::Success);
    return 0;
}
