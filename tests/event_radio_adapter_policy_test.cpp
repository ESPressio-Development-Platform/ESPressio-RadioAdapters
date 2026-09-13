#include <ESPressio_EventRadioAdapterBinding.hpp>
#include <ESPressio_SerializationMacros.hpp>
#include <HostRuntime.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <string_view>

using namespace ESPressio;
namespace E=ESPressio::Event;

namespace {

struct AckDelivery final {
    using PolicyCategory=Primitive::OccurrenceDeliveryPolicyTag;
    using RequiredEvidence=Primitive::DestinationPrimitiveAdmission;
    using TerminalDisposition=Primitive::ReportTerminalFailureToFamily;
    static constexpr std::uint64_t MaximumResidenceNanoseconds=1'000'000'000ULL;
    static constexpr std::uint16_t MaximumAttempts=2;
    static constexpr std::uint64_t MaximumAdapterAdmissionWaitNanoseconds=100'000'000ULL;
    static constexpr std::uint64_t MinimumRetrySpacingNanoseconds=1'000'000ULL;
    static constexpr std::uint64_t MaximumRetrySpacingNanoseconds=100'000'000ULL;
};

struct AckEvent final : E::TransmissibleEvent<AckEvent> {
    static constexpr E::EventTypeId TypeId{0x7A02};
    static constexpr std::size_t MaximumLiveInstances=2;
    static constexpr std::size_t MaximumPendingInstances=2;
    static constexpr std::string_view CanonicalName="RadioAdapters.Event.DestinationAdmission";
    using DeliveryPolicy=AckDelivery;
    std::uint16_t Value=0;
    AckEvent() noexcept=default;
    explicit AckEvent(std::uint16_t value) noexcept:Value(value){}
    ESPRESSIO_SERIALIZABLE_TYPE(AckEvent)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(ESPRESSIO_PROPERTY("value",Value))
};

System::DeviceRuntimeIdentity Identity(std::uint8_t marker,std::uint32_t runtime) {
    System::DeviceIdentifier::Storage bytes{};bytes[0]=marker;
    return {System::DeviceIdentifier{bytes},System::RuntimeIncarnationId{runtime}};
}

} // namespace

int main(){
    HostRuntime platform;
    assert(System::RuntimeIdentity::Install(Identity(1,17))==System::RuntimeIdentity::InstallationStatus::Success);

    Primitive::TypeDirectory<1> directory;
    assert(directory.Register<AckEvent>()==Primitive::TypeDirectoryRegistrationStatus::Success);
    assert(directory.Initialize()==Primitive::TypeDirectoryInitializationStatus::Success);

    std::array<E::RemoteAdmissionReceipt,2> receipts{};
    E::RuntimeConfiguration configuration{};
    configuration.DispatchLane.Name="radioEventPolicy";
    configuration.DispatchLane.StackSize=4096;
    configuration.RemoteAdmissionReceipts=receipts.data();
    configuration.RemoteAdmissionReceiptCapacity=receipts.size();
    E::Runtime runtime(configuration);
    assert(runtime.Initialize(directory.View())==E::EventRuntimeStatus::Success);

    RadioAdapters::EventRadioAdapterFamilyBinding<1> family;
    assert((family.BindType<AckEvent,Serializable::DirectBinary>(
        runtime,Adapters::AdapterServiceClass::Critical)==RadioAdapters::EventRadioAdapterBindingStatus::Success));
    assert(family.Freeze()==RadioAdapters::EventRadioAdapterBindingStatus::Success);

    const auto adapterBinding=family.AdapterBinding();
    assert(adapterBinding.RequiresDestinationAdmissionEvidence);
    assert(adapterBinding.Supports(Adapters::AdapterServiceClass::Critical));
    assert(!adapterBinding.Supports(Adapters::AdapterServiceClass::BestEffort));

    AckEvent payload{19};
    constexpr auto maximum=E::MaximumCompletePrimitiveWireBytes<AckEvent,Serializable::DirectBinary>;
    std::array<std::uint8_t,maximum> wire{};
    const auto encoded=Serializable::SerializeDirectBinary(
        payload,wire.data()+E::EventWireHeaderSize,wire.size()-E::EventWireHeaderSize);
    assert(encoded);
    const E::EventWireHeader header{{AckEvent::TypeId,Identity(2,31),E::ConceptualMessageId{77}},
        {700,Timing::TimeReliability::Acquiring},static_cast<std::uint32_t>(encoded.Bytes)};
    assert(E::EncodeEventWireHeader(header,wire.data(),wire.size()));
    const auto wireBytes=E::EventWireHeaderSize+encoded.Bytes;

    const auto radioBinding=family.RadioBinding();
    assert(radioBinding.IsValid());
    Primitive::PrimitivePolicyDescriptor policy{};
    auto resolved=radioBinding.ResolvePolicy(
        radioBinding.Owner,E::EventProtocolVersion,Adapters::AdapterServiceClass::Critical,
        {wire.data(),wireBytes},policy);
    assert(resolved==RadioAdapters::RadioAdapterBindingResolutionStatus::Success);
    assert(policy.Category==1&&policy.Evidence==1&&policy.Terminal==1);
    assert(policy.MaximumAttempts==AckDelivery::MaximumAttempts);
    assert(policy.MaximumResidenceNanoseconds==AckDelivery::MaximumResidenceNanoseconds);

    resolved=radioBinding.ResolvePolicy(
        radioBinding.Owner,E::EventProtocolVersion,Adapters::AdapterServiceClass::BestEffort,
        {wire.data(),wireBytes},policy);
    assert(resolved==RadioAdapters::RadioAdapterBindingResolutionStatus::Rejected);

    assert(runtime.Shutdown()==E::EventRuntimeStatus::Success);
    return 0;
}
