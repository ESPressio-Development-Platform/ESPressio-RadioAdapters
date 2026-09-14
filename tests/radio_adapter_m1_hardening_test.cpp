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

    Radio::RadioPeerObserveResult ObservePeer(
        Radio::IRadio&,const Radio::RadioAddress&,Radio::RadioPeerHandle& handle) noexcept {
        handle=Peer;
        return Radio::RadioPeerObserveResult::Observed;
    }

    Radio::RadioTransferSubmissionResult SubmitPeer(
        Radio::RadioPeerHandle,
        const Radio::RadioServiceProfile&,
        const Radio::RadioTransferTiming&,
        const std::uint8_t*,std::size_t,std::uint64_t=0) noexcept {
        return {Radio::RadioSchedulerStatus::Success,27};
    }
};

struct FakeAdapterRuntime {
    std::array<Adapters::LowerTransportCompletion,32> Completions{};
    std::size_t Count=0;

    Adapters::AdapterSubmissionDisposition CompleteTransport(
        const Adapters::LowerTransportCompletion& completion) noexcept {
        assert(Count<Completions.size());
        Completions[Count++]=completion;
        return Adapters::AdapterSubmissionDisposition::Accepted;
    }
};

struct ReceiptPolicy { bool Valid=true; };

bool ResolveReceipt(
    void* owner,Radio::RadioServiceClass inbound,std::uint64_t now,
    Radio::RadioServiceProfile& profile,Radio::RadioTransferTiming& timing) noexcept {
    auto& state=*static_cast<ReceiptPolicy*>(owner);
    if(!state.Valid||!Radio::IsValidRadioServiceClass(inbound)) return false;
    profile.Class=Radio::RadioServiceClass::Infrastructure;
    profile.DeadlineTreatment=Radio::RadioDeadlineTreatment::ExpiryOnly;
    profile.RequiredDirectLinkEvidence=Radio::RadioDirectLinkEvidenceRequirement::TransmissionCompletion;
    timing.ExpiryNanoseconds=now+1'000'000ULL;
    return true;
}

bool ValidateReceipt(void* owner) noexcept {
    return static_cast<ReceiptPolicy*>(owner)->Valid;
}

struct WakeState { std::size_t Count=0; };
void Wake(void* owner) noexcept { ++static_cast<WakeState*>(owner)->Count; }

Adapters::AdapterRecordIdentity Record(std::uint64_t generation) noexcept {
    return {Adapters::AdapterDirection::Outbound,Adapters::CapacityDomainKind::ResponsivePrivate,2,generation};
}

} // namespace

int main() {
    // The control codec accepts exactly the locked seven M1 dispositions and no
    // other byte value. Prefix corruption and non-zero reserved bits fail closed.
    for(unsigned raw=0;raw<=static_cast<unsigned>(Primitive::PrimitiveAdmissionDisposition::Malformed);++raw) {
        std::array<std::uint8_t,RadioAdapters::DirectRadioM1ReceiptBytes> bytes{};
        const auto admission=static_cast<Primitive::PrimitiveAdmissionDisposition>(raw);
        assert(RadioAdapters::EncodeDirectRadioM1Receipt(0x4321,admission,bytes.data(),bytes.size()));
        RadioAdapters::DirectRadioM1Receipt decoded{};
        assert(RadioAdapters::DecodeDirectRadioM1Receipt({bytes.data(),bytes.size()},decoded));
        assert(decoded.OriginalTransferId==0x4321);
        assert(decoded.Admission==admission);
        assert(Primitive::EstablishesDestinationAdmission(admission)==
            (admission==Primitive::PrimitiveAdmissionDisposition::Accepted||
             admission==Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted));

        for(std::size_t byte=0;byte<4;++byte) {
            for(std::uint8_t bit=0;bit<8;++bit) {
                auto corrupted=bytes;
                corrupted[byte]=static_cast<std::uint8_t>(corrupted[byte]^(std::uint8_t{1}<<bit));
                assert(!RadioAdapters::DecodeDirectRadioM1Receipt({corrupted.data(),corrupted.size()},decoded));
            }
        }
        auto reserved=bytes;
        reserved[7]=1;
        assert(!RadioAdapters::DecodeDirectRadioM1Receipt({reserved.data(),reserved.size()},decoded));
    }

    for(unsigned raw=static_cast<unsigned>(Primitive::PrimitiveAdmissionDisposition::Malformed)+1;raw<=0xFFu;++raw) {
        std::array<std::uint8_t,RadioAdapters::DirectRadioM1ReceiptBytes> bytes{};
        assert(RadioAdapters::EncodeDirectRadioM1Receipt(
            0x4321,Primitive::PrimitiveAdmissionDisposition::Accepted,bytes.data(),bytes.size()));
        bytes[6]=static_cast<std::uint8_t>(raw);
        RadioAdapters::DirectRadioM1Receipt decoded{};
        assert(!RadioAdapters::DecodeDirectRadioM1Receipt({bytes.data(),bytes.size()},decoded));
    }

    FakeRadioRuntime radio;
    FakeAdapterRuntime adapter;
    ReceiptPolicy receiptPolicy;
    WakeState wake;
    RadioAdapters::RadioAdapterM1Controller<FakeRadioRuntime,4,8> controller(
        radio,{&receiptPolicy,&ResolveReceipt,&ValidateReceipt},{&wake,&Wake});
    assert(controller.BindAdapterRuntime(adapter));

    const Radio::RadioContentionDomainId domain{7};
    const Radio::RadioContentionDomainId wrongDomain{8};
    const Adapters::AdapterRouteToken route{0x11223344ULL};
    const Adapters::AdapterRouteToken wrongRoute{0x11223345ULL};
    auto lease=controller.TransferIdLeaseTarget();
    assert(lease);

    // Even terminal, peer-acknowledged physical evidence cannot complete M1.
    std::uint64_t correlation=0;
    assert(controller.Reserve(Record(1),route,correlation));
    assert(lease.ReserveIssued(lease.Context,domain,correlation,42));
    controller.RadioLogicalTransferResolved({domain,{42,Radio::RadioTransferTerminalStatus::Completed,
        Radio::RadioDirectLinkEvidence::CompletedAndAcknowledged()}});
    assert(controller.ServiceOne()==Adapters::AdapterSubmissionDisposition::Rejected);
    assert(adapter.Count==0);

    // Route/domain/id noise is never allowed to alias the live correlation.
    assert(!controller.HandleReceipt(wrongRoute,domain,42,Primitive::PrimitiveAdmissionDisposition::Accepted));
    assert(!controller.HandleReceipt(route,wrongDomain,42,Primitive::PrimitiveAdmissionDisposition::Accepted));
    for(Radio::RadioTransferId id=1;id<512;++id) {
        if(id==42) continue;
        assert(!controller.HandleReceipt(route,domain,id,Primitive::PrimitiveAdmissionDisposition::Accepted));
    }
    assert(adapter.Count==0);
    assert(controller.HandleReceipt(route,domain,42,Primitive::PrimitiveAdmissionDisposition::Accepted));
    assert(controller.ServiceOne()==Adapters::AdapterSubmissionDisposition::Accepted);
    assert(adapter.Count==1);
    assert(adapter.Completions[0].HasDestinationAdmission);
    assert(adapter.Completions[0].DestinationAdmission==Primitive::PrimitiveAdmissionDisposition::Accepted);

    // Transport every exact disposition through a fresh correlation and prove
    // that only Accepted / AlreadyAccepted carry destination-admission success.
    const std::array<Primitive::PrimitiveAdmissionDisposition,7> dispositions{
        Primitive::PrimitiveAdmissionDisposition::Accepted,
        Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted,
        Primitive::PrimitiveAdmissionDisposition::TemporarilyUnavailable,
        Primitive::PrimitiveAdmissionDisposition::ResourceUnavailable,
        Primitive::PrimitiveAdmissionDisposition::Unsupported,
        Primitive::PrimitiveAdmissionDisposition::Rejected,
        Primitive::PrimitiveAdmissionDisposition::Malformed
    };
    Radio::RadioTransferId transfer=100;
    std::uint64_t generation=10;
    for(const auto admission:dispositions) {
        std::uint64_t token=0;
        assert(controller.Reserve(Record(generation++),route,token));
        assert(lease.ReserveIssued(lease.Context,domain,token,transfer));
        assert(controller.HandleReceipt(route,domain,transfer,admission));
        assert(controller.ServiceOne()==Adapters::AdapterSubmissionDisposition::Accepted);
        const auto& completion=adapter.Completions[adapter.Count-1];
        assert(completion.HasDestinationAdmission);
        assert(completion.DestinationAdmission==admission);
        assert(Primitive::EstablishesDestinationAdmission(admission)==
            (admission==Primitive::PrimitiveAdmissionDisposition::Accepted||
             admission==Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted));
        ++transfer;
    }

    // Quiesce invalidates every volatile attempt. Late terminal callbacks and M1
    // receipts from the old lifecycle remain stale after a replacement runtime is bound.
    std::uint64_t oldToken=0;
    assert(controller.Reserve(Record(99),route,oldToken));
    assert(lease.ReserveIssued(lease.Context,domain,oldToken,250));
    const auto before=adapter.Count;
    const auto transportBinding=controller.TransportBinding();
    transportBinding.Quiesce(transportBinding.Owner);
    controller.RadioLogicalTransferResolved({domain,{250,Radio::RadioTransferTerminalStatus::Completed,
        Radio::RadioDirectLinkEvidence::CompletedAndAcknowledged()}});
    assert(!controller.HandleReceipt(route,domain,250,Primitive::PrimitiveAdmissionDisposition::Accepted));
    assert(adapter.Count==before);

    FakeRadioRuntime replacementRadio;
    FakeAdapterRuntime replacementAdapter;
    assert(controller.RebindRadioRuntime(replacementRadio));
    assert(controller.BindAdapterRuntime(replacementAdapter));
    std::uint64_t replacementToken=0;
    assert(controller.Reserve(Record(100),route,replacementToken));
    assert(!lease.ReserveIssued(lease.Context,domain,replacementToken,250));
    assert(lease.ReserveIssued(lease.Context,domain,replacementToken,251));
    assert(!controller.HandleReceipt(route,domain,250,Primitive::PrimitiveAdmissionDisposition::Accepted));
    assert(controller.HandleReceipt(route,domain,251,Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted));
    assert(controller.ServiceOne()==Adapters::AdapterSubmissionDisposition::Accepted);
    assert(replacementAdapter.Count==1);
    assert(replacementAdapter.Completions[0].DestinationAdmission==Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted);

    return 0;
}
