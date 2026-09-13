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
    std::uint64_t Correlation=0;
    Radio::RadioSchedulerStatus Next=Radio::RadioSchedulerStatus::Success;

    bool IsRunning() const noexcept { return Running; }
    Radio::RadioTransferSubmissionResult SubmitPeer(
        Radio::RadioPeerHandle peer,const Radio::RadioServiceProfile& profile,
        const Radio::RadioTransferTiming& timing,const std::uint8_t* bytes,std::size_t size,
        std::uint64_t correlation=0) noexcept {
        Peer=peer;Profile=profile;Timing=timing;Size=size;Correlation=correlation;
        assert(size<=Bytes.size());for(std::size_t i=0;i<size;++i)Bytes[i]=bytes[i];
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
    timing.ExpiryNanoseconds=now+1'000'000ULL;timing.ServiceDeadlineNanoseconds=0;return true;
}
bool ValidatePolicy(void* owner) noexcept { return static_cast<PolicyContext*>(owner)->Valid; }

struct M1Context {
    bool ReserveAllowed=true;
    std::uint64_t NextToken=0x12340001ULL;
    std::uint64_t Generation=1;
    std::size_t ReserveCount=0;
    std::size_t CancelCount=0;
    std::size_t CancelRecordCount=0;
    std::size_t QuiesceCount=0;
    Adapters::AdapterRecordIdentity LastRecord{};
    Adapters::AdapterRouteToken LastRoute{};
    std::uint64_t LastCancelled=0;
};
bool ReserveM1(void* owner,Adapters::AdapterRecordIdentity record,Adapters::AdapterRouteToken route,std::uint64_t& token) noexcept {
    auto& state=*static_cast<M1Context*>(owner);++state.ReserveCount;state.LastRecord=record;state.LastRoute=route;
    if(!state.ReserveAllowed){token=0;return false;} token=state.NextToken;return true;
}
void CancelM1(void* owner,std::uint64_t token) noexcept {auto& state=*static_cast<M1Context*>(owner);++state.CancelCount;state.LastCancelled=token;}
void CancelRecordM1(void* owner,Adapters::AdapterRecordIdentity record) noexcept {auto& state=*static_cast<M1Context*>(owner);++state.CancelRecordCount;state.LastRecord=record;}
void QuiesceM1(void* owner) noexcept {++static_cast<M1Context*>(owner)->QuiesceCount;}
std::uint64_t M1Generation(void* owner) noexcept {return static_cast<M1Context*>(owner)->Generation;}

} // namespace

int main(){
    FakeRadioRuntime radio;RouteContext routes;PolicyContext policies;M1Context m1;
    RadioAdapters::RadioAdapterRouteBinding routeBinding{&routes,&ResolveRoute,&ValidateRoute};
    RadioAdapters::RadioAdapterTransferPolicyBinding policyBinding{&policies,&ResolvePolicy,&ValidatePolicy};
    RadioAdapters::RadioAdapterM1TransportBinding m1Binding{
        &m1,&ReserveM1,&CancelM1,&CancelRecordM1,&QuiesceM1,&M1Generation};
    RadioAdapters::RadioAdapterLowerTransport<FakeRadioRuntime,32> transport(radio,routeBinding,policyBinding,m1Binding);
    auto binding=transport.AdapterBinding();
    assert(binding&&binding.ProvidesDestinationPrimitiveAdmission&&!binding.ProvidesValidatedOriginalSource);
    assert(binding.Validate(binding.Owner));
    assert(!transport.IsQuiesced()&&transport.LifecycleGeneration()==1);

    Primitive::PrimitivePolicyDescriptor policy{};policy.Category=1;policy.Evidence=0;policy.MaximumAttempts=1;
    std::array<std::uint8_t,3> familyBytes{{0xA1,0xB2,0xC3}};
    const Adapters::AdapterRecordIdentity record{Adapters::AdapterDirection::Outbound,Adapters::CapacityDomainKind::ResponsivePrivate,1,4};
    const Adapters::AdapterRouteToken route{0xAABBCCDDULL};

    const auto immediate=binding.Submit(binding.Owner,record,0x1234,2,policy,Adapters::AdapterServiceClass::Responsive,
        {familyBytes.data(),familyBytes.size()},route);
    assert(immediate.Disposition==Adapters::LowerTransportDisposition::Accepted&&!immediate.DeferredCompletion&&immediate.Generation==0);
    assert(radio.Correlation==0&&m1.ReserveCount==0);
    assert(radio.Peer==routes.Peer&&radio.Profile.Class==Radio::RadioServiceClass::Responsive);
    assert(radio.Size==7&&radio.Bytes[0]==0x34&&radio.Bytes[1]==0x12&&radio.Bytes[2]==0x02&&radio.Bytes[3]==0x00);
    assert(radio.Bytes[4]==0xA1&&radio.Bytes[5]==0xB2&&radio.Bytes[6]==0xC3);

    policy.Evidence=1;radio.Next=Radio::RadioSchedulerStatus::Success;
    const auto deferred=binding.Submit(binding.Owner,record,0x1234,2,policy,Adapters::AdapterServiceClass::Responsive,
        {familyBytes.data(),familyBytes.size()},route);
    assert(deferred.Disposition==Adapters::LowerTransportDisposition::Accepted&&deferred.DeferredCompletion);
    assert(deferred.Generation==m1.NextToken&&radio.Correlation==m1.NextToken&&m1.ReserveCount==1);
    assert(m1.LastRecord==record&&m1.LastRoute.Value==route.Value);

    binding.Cancel(binding.Owner,record);
    assert(m1.CancelRecordCount==1&&m1.LastRecord==record);

    radio.Next=Radio::RadioSchedulerStatus::ResourceUnavailable;
    const auto failed=binding.Submit(binding.Owner,record,0x1234,2,policy,Adapters::AdapterServiceClass::Responsive,
        {familyBytes.data(),familyBytes.size()},route);
    assert(failed.Disposition==Adapters::LowerTransportDisposition::ResourceUnavailable&&!failed.DeferredCompletion);
    assert(m1.CancelCount==1&&m1.LastCancelled==m1.NextToken);

    m1.ReserveAllowed=false;
    const auto full=binding.Submit(binding.Owner,record,0x1234,2,policy,Adapters::AdapterServiceClass::Responsive,
        {familyBytes.data(),familyBytes.size()},route);
    assert(full.Disposition==Adapters::LowerTransportDisposition::ResourceUnavailable);

    policy.Evidence=0;radio.Next=Radio::RadioSchedulerStatus::Busy;
    assert(binding.Submit(binding.Owner,record,0x1234,2,policy,Adapters::AdapterServiceClass::Responsive,
        {familyBytes.data(),familyBytes.size()},route).Disposition==Adapters::LowerTransportDisposition::TemporarilyUnavailable);
    radio.Next=Radio::RadioSchedulerStatus::PayloadTooLarge;
    assert(binding.Submit(binding.Owner,record,0x1234,2,policy,Adapters::AdapterServiceClass::Responsive,
        {familyBytes.data(),familyBytes.size()},route).Disposition==Adapters::LowerTransportDisposition::PermanentlyRejected);

    routes.Valid=false;assert(!binding.Validate(binding.Owner));routes.Valid=true;policies.Valid=false;assert(!binding.Validate(binding.Owner));
    policies.Valid=true;radio.Running=false;assert(!binding.Validate(binding.Owner));radio.Running=true;

    binding.Quiesce(binding.Owner);
    assert(!binding.Validate(binding.Owner));
    assert(transport.IsQuiesced()&&transport.LifecycleGeneration()==1&&m1.QuiesceCount==1);
    assert(binding.Submit(binding.Owner,record,0x1234,2,policy,Adapters::AdapterServiceClass::Responsive,
        {familyBytes.data(),familyBytes.size()},route).Disposition==Adapters::LowerTransportDisposition::PermanentlyRejected);
    binding.Quiesce(binding.Owner);
    assert(m1.QuiesceCount==1);

    FakeRadioRuntime replacement;
    replacement.Next=Radio::RadioSchedulerStatus::Success;
    assert(transport.Restart(replacement));
    assert(!transport.IsQuiesced()&&transport.LifecycleGeneration()==2&&binding.Validate(binding.Owner));
    const auto restarted=binding.Submit(binding.Owner,record,0x1234,2,policy,Adapters::AdapterServiceClass::Responsive,
        {familyBytes.data(),familyBytes.size()},route);
    assert(restarted.Disposition==Adapters::LowerTransportDisposition::Accepted);
    assert(replacement.Size==7&&replacement.Correlation==0);
    assert(!transport.Restart(replacement));

    return 0;
}