#include <ESPressio_RadioAdapterM1.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

using namespace ESPressio;

namespace {

struct FakeRadioRuntime {
    bool Running=true;
    Radio::RadioPeerHandle Peer{4,9};
    Radio::IRadio* ObservedProvider=nullptr;
    Radio::RadioAddress ObservedAddress{};
    Radio::RadioServiceProfile LastProfile{};
    Radio::RadioTransferTiming LastTiming{};
    std::array<std::uint8_t,32> Bytes{};
    std::size_t Size=0;
    std::uint64_t Correlation=99;

    Radio::RadioPeerObserveResult ObservePeer(Radio::IRadio& provider,const Radio::RadioAddress& address,
        Radio::RadioPeerHandle& handle) noexcept {
        ObservedProvider=&provider;ObservedAddress=address;handle=Peer;return Radio::RadioPeerObserveResult::Observed;
    }
    Radio::RadioTransferSubmissionResult SubmitPeer(Radio::RadioPeerHandle peer,
        const Radio::RadioServiceProfile& profile,const Radio::RadioTransferTiming& timing,
        const std::uint8_t* bytes,std::size_t size,std::uint64_t correlation=0) noexcept {
        assert(peer==Peer);LastProfile=profile;LastTiming=timing;Correlation=correlation;Size=size;
        assert(size<=Bytes.size());for(std::size_t i=0;i<size;++i)Bytes[i]=bytes[i];
        return {Radio::RadioSchedulerStatus::Success,27};
    }
};

struct Provider final:Radio::IRadio {
    Radio::RadioAddress Address{};
    Provider(){const std::uint8_t raw[2]{0x44,0x55};Address=Radio::RadioAddress::FromBytes(raw,2);}
    bool Start()override{return true;} void Stop()noexcept override{} bool IsStarted()const noexcept override{return true;}
    Radio::RadioCapabilities Capabilities()const noexcept override{return {Radio::RadioCapability::HardwareAddressing,32,1,128};}
    Radio::RadioAddress LocalAddress()const noexcept override{return Address;}
    Radio::RadioContentionDomainId ContentionDomain()const noexcept override{return {7};}
    Radio::RadioProviderResourceProfile ProviderResources()const noexcept override{return {1,1,1,0};}
    bool IsTransmitReady()const noexcept override{return true;}
    Radio::RadioTransmissionCost EstimateTransmissionCost(const Radio::RadioAddress&,std::size_t,const Radio::RadioServiceProfile&)const noexcept override{return {1,1,Radio::RadioCostEstimateQuality::ConservativeAirtime};}
    Radio::RadioSendResult Send(const Radio::RadioAddress&,const std::uint8_t*,std::size_t)noexcept override{return Radio::RadioSendResult::Accepted(Radio::RadioDirectLinkEvidence::CompletedWithoutPeerAcknowledgement());}
    void SetReceiver(Radio::IRadioReceiver*)noexcept override{} void SetRuntimeSink(Radio::IRadioRuntimeSink*)noexcept override{}
    Radio::ManagedRadioIngressServiceResult ServiceInbound(std::size_t)noexcept override{return {};}
};

struct FakeAdapterRuntime {
    std::array<Adapters::LowerTransportCompletion,8> Completions{};
    std::size_t Count=0;
    Adapters::AdapterSubmissionDisposition Next=Adapters::AdapterSubmissionDisposition::Accepted;
    Adapters::AdapterSubmissionDisposition CompleteTransport(const Adapters::LowerTransportCompletion& completion) noexcept {
        assert(Count<Completions.size());Completions[Count++]=completion;return Next;
    }
};

struct ReceiptPolicy { bool Valid=true; };
bool ResolveReceipt(void* owner,Radio::RadioServiceClass inbound,std::uint64_t now,
    Radio::RadioServiceProfile& profile,Radio::RadioTransferTiming& timing) noexcept {
    auto& state=*static_cast<ReceiptPolicy*>(owner);if(!state.Valid||!Radio::IsValidRadioServiceClass(inbound))return false;
    profile.Class=Radio::RadioServiceClass::Infrastructure;
    profile.DeadlineTreatment=Radio::RadioDeadlineTreatment::ExpiryOnly;
    profile.RequiredDirectLinkEvidence=Radio::RadioDirectLinkEvidenceRequirement::TransmissionCompletion;
    timing.ExpiryNanoseconds=now+1'000'000ULL;timing.ServiceDeadlineNanoseconds=0;return true;
}
bool ValidateReceipt(void* owner) noexcept {return static_cast<ReceiptPolicy*>(owner)->Valid;}

struct WakeState { std::size_t Count=0; };
void Wake(void* owner) noexcept {++static_cast<WakeState*>(owner)->Count;}

} // namespace

int main(){
    FakeRadioRuntime radio;FakeAdapterRuntime adapter;ReceiptPolicy receiptPolicy;WakeState wake;
    RadioAdapters::RadioAdapterM1Controller<FakeRadioRuntime,4> controller(
        radio,{&receiptPolicy,&ResolveReceipt,&ValidateReceipt},{&wake,&Wake});
    assert(controller.BindAdapterRuntime(adapter));
    assert(controller.IsValid()&&controller.IsActive()&&controller.LifecycleGeneration()==1);

    const Radio::RadioContentionDomainId domain{7};
    const Adapters::AdapterRecordIdentity record{
        Adapters::AdapterDirection::Outbound,Adapters::CapacityDomainKind::ResponsivePrivate,2,11};
    const Adapters::AdapterRouteToken route{0x11223344ULL};
    std::uint64_t correlation=0;
    assert(controller.Reserve(record,route,correlation)&&correlation!=0);
    assert(controller.OutstandingAttempts()==1);

    auto lease=controller.TransferIdLeaseTarget();
    assert(lease);
    assert(!lease.IsReserved(lease.Context,domain,17));
    assert(lease.ReserveIssued(lease.Context,domain,correlation,17));
    assert(lease.IsReserved(lease.Context,domain,17));

    controller.RadioLogicalTransferResolved({domain,{17,Radio::RadioTransferTerminalStatus::Completed,
        Radio::RadioDirectLinkEvidence::CompletedWithoutPeerAcknowledgement()}});
    assert(wake.Count==0);
    assert(controller.ServiceOne()==Adapters::AdapterSubmissionDisposition::Rejected);
    assert(adapter.Count==0);

    assert(!controller.HandleReceipt({0x9999ULL},domain,17,Primitive::PrimitiveAdmissionDisposition::Accepted));
    assert(controller.HandleReceipt(route,domain,17,Primitive::PrimitiveAdmissionDisposition::Accepted));
    assert(wake.Count==1);
    assert(adapter.Count==0);
    assert(controller.ServiceOne()==Adapters::AdapterSubmissionDisposition::Accepted);
    assert(adapter.Count==1);
    assert(adapter.Completions[0].Record==record);
    assert(adapter.Completions[0].TransportGeneration==correlation);
    assert(adapter.Completions[0].HasDestinationAdmission);
    assert(adapter.Completions[0].DestinationAdmission==Primitive::PrimitiveAdmissionDisposition::Accepted);
    assert(adapter.Completions[0].Disposition==Adapters::LowerTransportDisposition::Accepted);
    assert(controller.OutstandingAttempts()==0);
    // A completed M1 id remains in the bounded restart exclusion window rather than becoming immediately reusable.
    assert(lease.IsReserved(lease.Context,domain,17));
    assert(!controller.HandleReceipt(route,domain,17,Primitive::PrimitiveAdmissionDisposition::Accepted));

    std::uint64_t cancelledCorrelation=0;
    assert(controller.Reserve(record,route,cancelledCorrelation));
    assert(lease.ReserveIssued(lease.Context,domain,cancelledCorrelation,19));
    auto transportBinding=controller.TransportBinding();
    assert(transportBinding);
    transportBinding.CancelRecord(transportBinding.Owner,record);
    assert(controller.OutstandingAttempts()==0);
    assert(lease.IsReserved(lease.Context,domain,19));

    std::uint64_t failedCorrelation=0;
    assert(controller.Reserve(record,route,failedCorrelation));
    assert(lease.ReserveIssued(lease.Context,domain,failedCorrelation,18));
    controller.RadioLogicalTransferResolved({domain,{18,Radio::RadioTransferTerminalStatus::TransmissionFailed,{}}});
    assert(wake.Count==2);
    assert(controller.ServiceOne()==Adapters::AdapterSubmissionDisposition::Accepted);
    assert(adapter.Count==2);
    assert(adapter.Completions[1].TransportGeneration==failedCorrelation);
    assert(!adapter.Completions[1].HasDestinationAdmission);
    assert(adapter.Completions[1].Disposition==Adapters::LowerTransportDisposition::ResourceUnavailable);
    assert(controller.OutstandingAttempts()==0);
    assert(lease.IsReserved(lease.Context,domain,18));

    std::array<std::uint8_t,RadioAdapters::DirectRadioM1ReceiptBytes> encoded{};
    assert(RadioAdapters::EncodeDirectRadioM1Receipt(0x1234,Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted,
        encoded.data(),encoded.size()));
    RadioAdapters::DirectRadioM1Receipt decoded{};
    assert(RadioAdapters::DecodeDirectRadioM1Receipt({encoded.data(),encoded.size()},decoded));
    assert(decoded.OriginalTransferId==0x1234);
    assert(decoded.Admission==Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted);
    encoded[7]=1;
    assert(!RadioAdapters::DecodeDirectRadioM1Receipt({encoded.data(),encoded.size()},decoded));

    Provider provider;const std::uint8_t sourceRaw[2]{0x66,0x77};
    const auto source=Radio::RadioAddress::FromBytes(sourceRaw,2);
    assert(controller.SendReceipt(provider,source,0x3344,Radio::RadioServiceClass::Responsive,
        Primitive::PrimitiveAdmissionDisposition::ResourceUnavailable));
    assert(radio.ObservedProvider==&provider&&radio.ObservedAddress==source);
    assert(radio.Correlation==0);
    assert(radio.Size==RadioAdapters::DirectRadioM1ReceiptBytes);
    assert(RadioAdapters::DecodeDirectRadioM1Receipt({radio.Bytes.data(),radio.Size},decoded));
    assert(decoded.OriginalTransferId==0x3344);
    assert(decoded.Admission==Primitive::PrimitiveAdmissionDisposition::ResourceUnavailable);
    assert(radio.LastProfile.Class==Radio::RadioServiceClass::Infrastructure);

    // Hold one live attempt across controlled shutdown. Quiesce must release the volatile attempt while remembering
    // its transfer id so a fresh Radio scheduler cannot immediately reuse it for a different semantic occurrence.
    std::uint64_t oldLifecycleCorrelation=0;
    assert(controller.Reserve(record,route,oldLifecycleCorrelation));
    assert(lease.ReserveIssued(lease.Context,domain,oldLifecycleCorrelation,21));
    assert(controller.OutstandingAttempts()==1);
    assert(transportBinding.Quiesce&&transportBinding.LifecycleGeneration);
    transportBinding.Quiesce(transportBinding.Owner);
    assert(!controller.IsActive()&&!controller.IsValid());
    assert(controller.OutstandingAttempts()==0);
    assert(controller.LifecycleGeneration()==1);
    assert(lease.IsReserved(lease.Context,domain,21));
    const auto staleBefore=controller.StaleLifecycleSignals();
    controller.RadioLogicalTransferResolved({domain,{21,Radio::RadioTransferTerminalStatus::TransmissionFailed,{}}});
    assert(!controller.HandleReceipt(route,domain,21,Primitive::PrimitiveAdmissionDisposition::Accepted));
    assert(controller.StaleLifecycleSignals()>=staleBefore+2);
    assert(adapter.Count==2);
    assert(!controller.SendReceipt(provider,source,21,Radio::RadioServiceClass::Responsive,
        Primitive::PrimitiveAdmissionDisposition::Accepted));

    FakeRadioRuntime replacementRadio;FakeAdapterRuntime replacementAdapter;
    assert(controller.RebindRadioRuntime(replacementRadio));
    assert(controller.BindAdapterRuntime(replacementAdapter));
    assert(controller.IsActive()&&controller.IsValid()&&controller.LifecycleGeneration()==2);

    std::uint64_t replacementCorrelation=0;
    assert(controller.Reserve(record,route,replacementCorrelation));
    // The old lifecycle id is still excluded even though the replacement scheduler could have restarted at 1.
    assert(!lease.ReserveIssued(lease.Context,domain,replacementCorrelation,21));
    assert(lease.ReserveIssued(lease.Context,domain,replacementCorrelation,22));
    assert(!controller.HandleReceipt(route,domain,21,Primitive::PrimitiveAdmissionDisposition::Accepted));
    assert(controller.HandleReceipt(route,domain,22,Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted));
    assert(controller.ServiceOne()==Adapters::AdapterSubmissionDisposition::Accepted);
    assert(replacementAdapter.Count==1);
    assert(replacementAdapter.Completions[0].TransportGeneration==replacementCorrelation);
    assert(replacementAdapter.Completions[0].DestinationAdmission==Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted);
    assert(controller.OutstandingAttempts()==0);

    return 0;
}