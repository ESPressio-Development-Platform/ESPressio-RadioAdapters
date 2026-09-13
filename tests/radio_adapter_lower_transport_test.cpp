#include <ESPressio_RadioAdapterLowerTransport.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

using namespace ESPressio;

namespace {

struct FakeRadioRuntime {
    bool Running=true;
    Radio::RadioPeerHandle Peer{};
    Radio::RadioServiceProfile Profile{};
    Radio::RadioTransferTiming Timing{};
    std::array<std::uint8_t,32> Bytes{};
    std::size_t Size=0;
    Radio::RadioSchedulerStatus Next=Radio::RadioSchedulerStatus::Success;

    bool IsRunning() const noexcept { return Running; }
    Radio::RadioTransferSubmissionResult SubmitPeer(
        Radio::RadioPeerHandle peer,const Radio::RadioServiceProfile& profile,
        const Radio::RadioTransferTiming& timing,const std::uint8_t* bytes,std::size_t size) noexcept {
        Peer=peer;Profile=profile;Timing=timing;Size=size;
        assert(size<=Bytes.size());
        for(std::size_t i=0;i<size;++i) Bytes[i]=bytes[i];
        return {Next,Next==Radio::RadioSchedulerStatus::Success?Radio::RadioTransferId{17}:Radio::RadioTransferId{0}};
    }
};

struct RouteContext { bool Valid=true; Radio::RadioPeerHandle Peer{3,7}; };
bool ResolveRoute(void* owner,Adapters::AdapterRouteToken route,Radio::RadioPeerHandle& peer) noexcept {
    auto& context=*static_cast<RouteContext*>(owner);
    if(!context.Valid||route.Value!=0xAABBCCDDULL){peer={};return false;}
    peer=context.Peer;return true;
}
bool ValidateRoute(void* owner) noexcept { return static_cast<RouteContext*>(owner)->Valid; }

struct PolicyContext { bool Valid=true; };
bool ResolvePolicy(void* owner,Primitive::PrimitiveFamilyId family,Primitive::PrimitiveProtocolVersion protocol,
    const Primitive::PrimitivePolicyDescriptor& policy,Adapters::AdapterServiceClass service,std::uint64_t now,
    Radio::RadioServiceProfile& profile,Radio::RadioTransferTiming& timing) noexcept {
    auto& context=*static_cast<PolicyContext*>(owner);
    if(!context.Valid||family!=0x1234||protocol!=2||policy.MaximumAttempts==0) return false;
    const auto radioService=RadioAdapters::ToRadioServiceClass(service);
    if(!Radio::IsValidRadioServiceClass(radioService)) return false;
    profile.Class=radioService;
    profile.DeadlineTreatment=Radio::RadioDeadlineTreatment::ExpiryOnly;
    profile.RequiredDirectLinkEvidence=Radio::RadioDirectLinkEvidenceRequirement::TransmissionCompletion;
    timing.ExpiryNanoseconds=now+1'000'000ULL;
    timing.ServiceDeadlineNanoseconds=0;
    return true;
}
bool ValidatePolicy(void* owner) noexcept { return static_cast<PolicyContext*>(owner)->Valid; }

} // namespace

int main(){
    FakeRadioRuntime radio;
    RouteContext routes;
    PolicyContext policies;
    RadioAdapters::RadioAdapterRouteBinding routeBinding{&routes,&ResolveRoute,&ValidateRoute};
    RadioAdapters::RadioAdapterTransferPolicyBinding policyBinding{&policies,&ResolvePolicy,&ValidatePolicy};
    RadioAdapters::RadioAdapterLowerTransport<FakeRadioRuntime,32> transport(radio,routeBinding,policyBinding);
    auto binding=transport.AdapterBinding();
    assert(binding);
    assert(!binding.ProvidesDestinationPrimitiveAdmission);
    assert(!binding.ProvidesValidatedOriginalSource);
    assert(binding.Validate(binding.Owner));

    Primitive::PrimitivePolicyDescriptor policy{};
    policy.Category=1;policy.Evidence=0;policy.MaximumAttempts=1;
    std::array<std::uint8_t,3> familyBytes{{0xA1,0xB2,0xC3}};
    const Adapters::AdapterRecordIdentity record{Adapters::AdapterDirection::Outbound,Adapters::CapacityDomainKind::ResponsivePrivate,1,4};
    const auto result=binding.Submit(binding.Owner,record,0x1234,2,policy,Adapters::AdapterServiceClass::Responsive,
        {familyBytes.data(),familyBytes.size()},Adapters::AdapterRouteToken{0xAABBCCDDULL});
    assert(result.Disposition==Adapters::LowerTransportDisposition::Accepted);
    assert(!result.DeferredCompletion&&result.Generation==0);
    assert(radio.Peer==routes.Peer);
    assert(radio.Profile.Class==Radio::RadioServiceClass::Responsive);
    assert(radio.Profile.RequiredDirectLinkEvidence==Radio::RadioDirectLinkEvidenceRequirement::TransmissionCompletion);
    assert(radio.Size==7);
    assert(radio.Bytes[0]==0x34&&radio.Bytes[1]==0x12&&radio.Bytes[2]==0x02&&radio.Bytes[3]==0x00);
    assert(radio.Bytes[4]==0xA1&&radio.Bytes[5]==0xB2&&radio.Bytes[6]==0xC3);

    radio.Next=Radio::RadioSchedulerStatus::Busy;
    assert(binding.Submit(binding.Owner,record,0x1234,2,policy,Adapters::AdapterServiceClass::Responsive,
        {familyBytes.data(),familyBytes.size()},Adapters::AdapterRouteToken{0xAABBCCDDULL}).Disposition==
        Adapters::LowerTransportDisposition::TemporarilyUnavailable);
    radio.Next=Radio::RadioSchedulerStatus::ResourceUnavailable;
    assert(binding.Submit(binding.Owner,record,0x1234,2,policy,Adapters::AdapterServiceClass::Responsive,
        {familyBytes.data(),familyBytes.size()},Adapters::AdapterRouteToken{0xAABBCCDDULL}).Disposition==
        Adapters::LowerTransportDisposition::ResourceUnavailable);
    radio.Next=Radio::RadioSchedulerStatus::PayloadTooLarge;
    assert(binding.Submit(binding.Owner,record,0x1234,2,policy,Adapters::AdapterServiceClass::Responsive,
        {familyBytes.data(),familyBytes.size()},Adapters::AdapterRouteToken{0xAABBCCDDULL}).Disposition==
        Adapters::LowerTransportDisposition::PermanentlyRejected);

    routes.Valid=false;
    assert(!binding.Validate(binding.Owner));
    routes.Valid=true;policies.Valid=false;
    assert(!binding.Validate(binding.Owner));
    policies.Valid=true;radio.Running=false;
    assert(!binding.Validate(binding.Owner));
    radio.Running=true;
    binding.Quiesce(binding.Owner);
    assert(!binding.Validate(binding.Owner));

    return 0;
}
