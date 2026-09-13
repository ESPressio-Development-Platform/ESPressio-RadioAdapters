#include <ESPressio_Adapters.hpp>
#include <ESPressio_CommandRadioAdapterBinding.hpp>
#include <ESPressio_Commands.hpp>
#include <ESPressio_Persistence.hpp>
#include <ESPressio_RadioAdapterIngress.hpp>
#include <ESPressio_RadioAdapterLowerTransport.hpp>
#include <ESPressio_RadioAdapterM1.hpp>
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
namespace C=ESPressio::Command;

namespace {

struct EvidencePolicy final {
    using PolicyCategory=Primitive::OccurrenceDeliveryPolicyTag;
    using RequiredEvidence=Primitive::DestinationPrimitiveAdmission;
    using TerminalDisposition=Primitive::DiagnosticOnlyAfterBudget;
    static constexpr std::uint64_t MaximumResidenceNanoseconds=1'000'000'000ULL;
    static constexpr std::uint16_t MaximumAttempts=1;
    static constexpr std::uint64_t MaximumAdapterAdmissionWaitNanoseconds=1'000'000ULL;
    static constexpr std::uint64_t MinimumRetrySpacingNanoseconds=0;
    static constexpr std::uint64_t MaximumRetrySpacingNanoseconds=0;
};

struct Retention final {
    static constexpr std::size_t MaximumTrackedOrigins=2;
    static constexpr std::size_t ReplayWindowEntries=4;
    using ResultRetention=C::VolatileResults;
};

struct Reply final {
    std::uint32_t Value=0;
    ESPRESSIO_SERIALIZABLE_TYPE(Reply)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(ESPRESSIO_PROPERTY("value",Value))
};

struct QueryCommand final : C::TransmissibleCommand<QueryCommand,Reply> {
    static constexpr C::CommandTypeId TypeId{0x6202};
    static constexpr std::string_view CanonicalName="RadioAdapters.Command.M1";
    static constexpr std::size_t MaximumLiveInstances=3;
    static constexpr std::size_t MaximumPendingExecutions=1;
    static constexpr std::size_t MaximumPendingResponses=2;
    using ExecutionAdmissionPolicy=C::RequiredExecution;
    using RequestDeliveryPolicy=EvidencePolicy;
    using ResponseDeliveryPolicy=EvidencePolicy;
    using CompletionRetentionPolicy=Retention;
    std::uint32_t Value=0;
    QueryCommand() noexcept=default;
    explicit QueryCommand(std::uint32_t value) noexcept:Value(value){}
    ESPRESSIO_SERIALIZABLE_TYPE(QueryCommand)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(ESPRESSIO_PROPERTY("value",Value))
};

template<std::size_t RecordBytes,std::size_t Records>
class Store final : public Persistence::IAtomicRecordStore {
    struct Entry final {
        bool Used=false;
        Persistence::AtomicRecordKey Key{};
        std::size_t Size=0;
        std::array<std::uint8_t,RecordBytes> Bytes{};
    };
    std::array<Entry,Records> _entries{};
public:
    Persistence::AtomicRecordCapabilities Capabilities() const noexcept override { return {true,true,RecordBytes,Records}; }
    Persistence::AtomicRecordStatus Recover() noexcept override { return Persistence::AtomicRecordStatus::Success; }
    Persistence::AtomicRecordStatus Read(const Persistence::AtomicRecordKey& key,std::uint8_t* buffer,
                                         std::size_t capacity,std::size_t& bytesRead) noexcept override {
        bytesRead=0;
        for(const auto& entry:_entries) {
            if(!entry.Used||!(entry.Key==key)) continue;
            if(!buffer||capacity<entry.Size) return Persistence::AtomicRecordStatus::BufferTooSmall;
            std::memcpy(buffer,entry.Bytes.data(),entry.Size);
            bytesRead=entry.Size;
            return Persistence::AtomicRecordStatus::Success;
        }
        return Persistence::AtomicRecordStatus::NotFound;
    }
    Persistence::AtomicRecordStatus ReplaceAtomically(const Persistence::AtomicRecordKey& key,
                                                       const std::uint8_t* data,std::size_t size) noexcept override {
        if(!key||!data||size>RecordBytes) return Persistence::AtomicRecordStatus::NoSpace;
        Entry* target=nullptr;
        for(auto& entry:_entries) {
            if(entry.Used&&entry.Key==key) { target=&entry;break; }
            if(!entry.Used&&!target) target=&entry;
        }
        if(!target) return Persistence::AtomicRecordStatus::NoSpace;
        target->Used=true;target->Key=key;target->Size=size;
        std::memcpy(target->Bytes.data(),data,size);
        return Persistence::AtomicRecordStatus::Success;
    }
    Persistence::AtomicRecordStatus RemoveAfterCommit(const Persistence::AtomicRecordKey& key) noexcept override {
        for(auto& entry:_entries) if(entry.Used&&entry.Key==key) { entry=Entry{};break; }
        return Persistence::AtomicRecordStatus::Success;
    }
};

Persistence::AtomicRecordKey StoreKey(std::string_view text) {
    Persistence::AtomicRecordKey key;
    assert(Persistence::AtomicRecordKey::TryCreate(text,key));
    return key;
}

System::DeviceRuntimeIdentity Identity(std::uint8_t marker,std::uint32_t runtime) {
    System::DeviceIdentifier::Storage bytes{};bytes.back()=marker;
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

struct Owner final {
    std::atomic<unsigned> HandlerCalls{0};
    std::atomic<unsigned> Callbacks{0};
    C::CommandCallerCompletionKind Kind=C::CommandCallerCompletionKind::ResponseTimedOut;
    std::uint32_t ReplyValue=0;

    Reply Handle(const QueryCommand& command,const C::CommandExecutionContext&) {
        HandlerCalls.fetch_add(1,std::memory_order_release);
        return {command.Value+1};
    }
    void OnResult(const C::CommandCompletion<QueryCommand>& completion) {
        Kind=completion.Kind();
        if(const auto* reply=completion.ResponseValue()) ReplyValue=reply->Value;
        Callbacks.fetch_add(1,std::memory_order_release);
    }
};

struct CapabilityHost final {
    std::atomic<std::uint64_t> Now{1000};
    std::atomic<unsigned> Wakes{0};
    static bool Wake(void* context,bool) noexcept { ++static_cast<CapabilityHost*>(context)->Wakes;return true; }
    static bool Accepting(const void*) noexcept { return true; }
    static std::uint64_t Time(const void* context) noexcept { return static_cast<const CapabilityHost*>(context)->Now.load(); }
};

struct Routes final {
    static bool ValidateSemantic(void*) noexcept { return true; }
    static bool ResolveSemantic(void*,System::DeviceIdentifier device,Adapters::AdapterRouteToken& route) noexcept {
        if(!device) return false;
        route.Value=0xC000ULL+device.Bytes().back();
        return true;
    }
    static bool ValidateLower(void*) noexcept { return true; }
    static bool ResolveLower(void*,Adapters::AdapterRouteToken route,Radio::RadioPeerHandle& peer) noexcept {
        if(route.Value<0xC001ULL||route.Value>0xC0ffULL) { peer={};return false; }
        peer={static_cast<std::uint16_t>(route.Value&0xffU),7};
        return true;
    }
};

using Arena=Adapters::StaticByteArena<Adapters::ByteClass<256,8>,Adapters::ByteClass<1024,4>>;
using Domain=Adapters::StaticCapacityDomain<1024,4,Arena>;
using Inbound=Adapters::CapacityPlane<Adapters::AdapterDirection::Inbound,
    Domain,Domain,Domain,Domain,Domain,Domain,Domain,Domain>;
using Outbound=Adapters::CapacityPlane<Adapters::AdapterDirection::Outbound,
    Domain,Domain,Domain,Domain,Domain,Domain,Domain,Domain>;
using AdapterRuntime=Adapters::AdapterRuntime<Inbound,Outbound,2,4,1,1,8>;

struct FakeRadioRuntime final {
    bool Running=true;
    Radio::RadioTransferIdLeaseTarget Lease{};
    Radio::RadioContentionDomainId Domain{7};
    Radio::RadioPeerHandle Peer{};
    Radio::RadioServiceProfile Profile{};
    Radio::RadioTransferTiming Timing{};
    std::array<std::uint8_t,1024> Bytes{};
    std::size_t Size=0;
    std::uint64_t Correlation=0;
    Radio::RadioTransferId LastTransfer=0;
    Radio::RadioTransferId NextTransfer=20;
    std::atomic<unsigned> Calls{0};

    bool IsRunning() const noexcept { return Running; }
    Radio::RadioTransferSubmissionResult SubmitPeer(
        Radio::RadioPeerHandle peer,const Radio::RadioServiceProfile& profile,
        const Radio::RadioTransferTiming& timing,const std::uint8_t* bytes,std::size_t size,
        std::uint64_t correlation=0) noexcept {
        assert(bytes&&size<=Bytes.size());
        const auto transfer=NextTransfer++;
        if(correlation!=0) {
            assert(Lease);
            assert(Lease.ReserveIssued(Lease.Context,Domain,correlation,transfer));
        }
        Peer=peer;Profile=profile;Timing=timing;Size=size;Correlation=correlation;LastTransfer=transfer;
        std::memcpy(Bytes.data(),bytes,size);
        Calls.fetch_add(1,std::memory_order_release);
        return {Radio::RadioSchedulerStatus::Success,transfer};
    }
};

bool ResolveTransferPolicy(
    void*,Primitive::PrimitiveFamilyId family,Primitive::PrimitiveProtocolVersion protocol,
    const Primitive::PrimitivePolicyDescriptor& policy,Adapters::AdapterServiceClass service,std::uint64_t now,
    Radio::RadioServiceProfile& profile,Radio::RadioTransferTiming& timing) noexcept {
    if(family!=Primitive::FamilyIds::Command||protocol!=C::CommandProtocolVersion||policy.Evidence==0||
       service!=Adapters::AdapterServiceClass::Responsive) return false;
    profile.Class=RadioAdapters::ToRadioServiceClass(service);
    profile.DeadlineTreatment=Radio::RadioDeadlineTreatment::ExpiryOnly;
    profile.RequiredDirectLinkEvidence=Radio::RadioDirectLinkEvidenceRequirement::TransmissionCompletion;
    timing.ExpiryNanoseconds=now+1'000'000'000ULL;
    timing.ServiceDeadlineNanoseconds=0;
    return true;
}
bool ValidateTransferPolicy(void*) noexcept { return true; }

class Provider final : public Radio::IRadio {
public:
    bool Start() override{return true;} void Stop() noexcept override{} bool IsStarted()const noexcept override{return true;}
    Radio::RadioCapabilities Capabilities()const noexcept override{return {};}
    Radio::RadioAddress LocalAddress()const noexcept override{const std::uint8_t b[2]{9,9};return Radio::RadioAddress::FromBytes(b,2);}
    Radio::RadioContentionDomainId ContentionDomain()const noexcept override{return {7};}
    Radio::RadioProviderResourceProfile ProviderResources()const noexcept override{return {};}
    bool IsTransmitReady()const noexcept override{return true;}
    Radio::RadioTransmissionCost EstimateTransmissionCost(const Radio::RadioAddress&,std::size_t n,const Radio::RadioServiceProfile&)const noexcept override{return {n+1,(n+1)*1000,Radio::RadioCostEstimateQuality::ConservativeAirtime};}
    Radio::RadioSendResult Send(const Radio::RadioAddress&,const std::uint8_t*,std::size_t)noexcept override{return Radio::RadioSendResult::Accepted(Radio::RadioDirectLinkEvidence::CompletedWithoutPeerAcknowledgement());}
    void SetReceiver(Radio::IRadioReceiver*)noexcept override{} void SetRuntimeSink(Radio::IRadioRuntimeSink*)noexcept override{}
    Radio::ManagedRadioIngressServiceResult ServiceInbound(std::size_t)noexcept override{return {};}
};

struct ProvenanceContext final { System::DeviceRuntimeIdentity Source{}; Adapters::AdapterRouteToken Route{}; };
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
    static void Complete(void* owner,const Adapters::AdapterInboundCompletion& completion) noexcept {
        auto& self=*static_cast<CompletionCapture*>(owner);
        self.Admission=completion.Admission;
        self.Done.store(true,std::memory_order_release);
    }
};

template<class TFamily>
struct OutboundOwner final {
    TFamily* Family=nullptr;
    C::CommandOutboundAdmission Admit(System::DeviceIdentifier target,
        const C::CommandRequestLease<QueryCommand>& request,C::CommandRequestDeliveryToken token) noexcept {
        return Family->template SubmitRequest<QueryCommand>(target,request,token);
    }
    bool Validate(const C::CommandOutboundContract& contract) noexcept {
        return Family->template ValidateOutboundContract<QueryCommand,Serializable::DirectBinary>(contract);
    }
    C::CommandRemoteResponseDestination ReserveRecovered(const C::CommandExecutionKey& key) noexcept {
        return Family->template ReserveRecoveredResponse<QueryCommand>(key);
    }
    void ReleaseRecovered(C::CommandRemoteResponseDestination destination) noexcept {
        Family->ReleaseRecoveredResponse(destination);
    }
};

Task::TaskExecutorConfiguration RouterConfiguration() {
    Task::TaskExecutorConfiguration configuration{};
    configuration.Execution.Name="radioCmdM1Router";
    configuration.Execution.StackSize=4096;
    configuration.QueueDepth=2;
    configuration.OverflowPolicy=Task::TaskQueueOverflowPolicy::Reject;
    configuration.QueueMemoryPolicy=Task::TaskMemoryPolicy::Internal;
    return configuration;
}

} // namespace

int main() {
    HostRuntime platform;
    const auto local=Identity(1,17);
    const auto remote=Identity(9,31);
    assert(System::RuntimeIdentity::Install(local)==System::RuntimeIdentity::InstallationStatus::Success);

    Primitive::TypeDirectory<1> directory;
    assert(directory.Register<QueryCommand>()==Primitive::TypeDirectoryRegistrationStatus::Success);
    assert(directory.Initialize()==Primitive::TypeDirectoryInitializationStatus::Success);

    AdapterRuntime adapter;
    Routes routes;
    const RadioAdapters::RadioAdapterSemanticRouteBinding semanticRoutes{
        &routes,&Routes::ResolveSemantic,&Routes::ValidateSemantic};
    using Family=RadioAdapters::CommandRadioAdapterFamilyBinding<AdapterRuntime,1,3>;
    Family family(adapter,semanticRoutes);
    assert((family.ConfigureType<QueryCommand,Serializable::DirectBinary>(
        Adapters::AdapterServiceClass::Responsive,Adapters::AdapterServiceClass::Responsive)==
        RadioAdapters::CommandRadioAdapterBindingStatus::Success));

    OutboundOwner<Family> outboundOwner{&family};
    C::CommandOutboundBinding<QueryCommand,Serializable::DirectBinary> outbound;
    assert((outbound.Initialize<OutboundOwner<Family>,
        &OutboundOwner<Family>::Admit,&OutboundOwner<Family>::Validate,
        &OutboundOwner<Family>::ReserveRecovered,&OutboundOwner<Family>::ReleaseRecovered>(outboundOwner)));

    Store<4096,4> store;
    C::CommandResponseRouter<2> router(RouterConfiguration());
    C::RuntimeConfiguration commandConfiguration{};
    commandConfiguration.ExecutionLane.Name="radioCmdM1";
    commandConfiguration.ExecutionLane.StackSize=4096;
    commandConfiguration.ResponseRouter=router.Binding();
    C::Runtime commandRuntime(commandConfiguration);
    Owner owner;
    assert(commandRuntime.BindHandler<QueryCommand>(owner,&Owner::Handle)==C::CommandRuntimeStatus::Success);
    assert(commandRuntime.BindPersistence<QueryCommand>(store,StoreKey("radio-command-m1"))==C::CommandRuntimeStatus::Success);
    assert((commandRuntime.BindTransport<QueryCommand,Serializable::DirectBinary>(outbound)==C::CommandRuntimeStatus::Success));
    assert(commandRuntime.Initialize(directory.View())==C::CommandRuntimeStatus::Success);
    assert((family.AttachRuntime<QueryCommand,Serializable::DirectBinary>(commandRuntime)==
        RadioAdapters::CommandRadioAdapterBindingStatus::Success));
    assert(family.Freeze()==RadioAdapters::CommandRadioAdapterBindingStatus::Success);

    RadioAdapters::RadioAdapterBindingRegistry<1> registry;
    assert(registry.Bind(family.RadioBinding())==Adapters::AdapterRuntimeStatus::Success);
    assert(registry.Freeze()==Adapters::AdapterRuntimeStatus::Success);

    FakeRadioRuntime radio;
    RadioAdapters::RadioAdapterM1Controller<FakeRadioRuntime,4> m1(radio);
    m1.BindAdapterRuntime(adapter);
    radio.Lease=m1.TransferIdLeaseTarget();
    assert(m1.IsValid());

    RadioAdapters::RadioAdapterRouteBinding lowerRoutes{&routes,&Routes::ResolveLower,&Routes::ValidateLower};
    RadioAdapters::RadioAdapterTransferPolicyBinding transferPolicy{&routes,&ResolveTransferPolicy,&ValidateTransferPolicy};
    RadioAdapters::RadioAdapterLowerTransport<FakeRadioRuntime,1024> lower(
        radio,lowerRoutes,transferPolicy,m1.TransportBinding());

    assert(adapter.BindFamily(family.AdapterBinding())==Adapters::AdapterRuntimeStatus::Success);
    assert(adapter.BindTransport(lower.AdapterBinding())==Adapters::AdapterRuntimeStatus::Success);
    Task::TaskExecutionConfiguration worker{};worker.Name="radioA2CmdM1";worker.StackSize=4096;
    assert(adapter.Initialize(worker,worker)==Adapters::AdapterRuntimeStatus::Success);
    assert(adapter.Start()==Adapters::AdapterRuntimeStatus::Success);
    assert(commandRuntime.Start()==C::CommandRuntimeStatus::Success);

    CapabilityHost host;
    Threads::ThreadHostServices services{};
    services.Owner=&host;
    services.WakeFunction=&CapabilityHost::Wake;
    services.AcceptingFunction=&CapabilityHost::Accepting;
    services.NowFunction=&CapabilityHost::Time;
    C::ResponseCapability<2> responses;
    assert(responses.Initialize(services)==Threads::ThreadStatus::Success);
    assert(responses.FinalizeInitialization()==Threads::ThreadStatus::Success);
    auto client=responses.Client(owner);

    // Terminal Radio failure is not M1. It must fail the retained Command delivery token.
    const auto failed=client.ExecuteTo<QueryCommand,&Owner::OnResult>(remote.Device,std::chrono::milliseconds(500),73U);
    assert(failed.Accepted()&&client.IsLive(failed.Request));
    Eventually([&]{return radio.Calls.load(std::memory_order_acquire)>=1;});
    assert(radio.Correlation!=0&&m1.OutstandingAttempts()==1);
    const auto failedTransfer=radio.LastTransfer;
    m1.RadioLogicalTransferResolved({radio.Domain,{failedTransfer,Radio::RadioTransferTerminalStatus::TransmissionFailed,{}}});
    assert(m1.ServiceOne()==Adapters::AdapterSubmissionDisposition::Accepted);
    Eventually([&]{return client.IsReady(failed.Request);});
    responses.Service({host.Now.load(),services});
    assert(owner.Callbacks.load(std::memory_order_acquire)==1);
    assert(owner.Kind==C::CommandCallerCompletionKind::RequestDeliveryFailed);
    assert(!client.IsLive(failed.Request));

    // Exact Accepted M1 releases request-delivery pursuit, but caller remains live until the matching Command response.
    const auto succeeded=client.ExecuteTo<QueryCommand,&Owner::OnResult>(remote.Device,std::chrono::milliseconds(500),80U);
    assert(succeeded.Accepted()&&client.IsLive(succeeded.Request));
    Eventually([&]{return radio.Calls.load(std::memory_order_acquire)>=2;});
    const auto successTransfer=radio.LastTransfer;
    assert(radio.Correlation!=0&&m1.OutstandingAttempts()==1);
    const Adapters::AdapterRouteToken remoteRoute{0xC009ULL};
    assert(m1.HandleReceipt(remoteRoute,radio.Domain,successTransfer,Primitive::PrimitiveAdmissionDisposition::Accepted));
    assert(m1.ServiceOne()==Adapters::AdapterSubmissionDisposition::Accepted);
    assert(client.IsLive(succeeded.Request)&&!client.IsReady(succeeded.Request));

    constexpr auto maxResponse=C::MaximumCompleteResponseWireBytes<QueryCommand,Serializable::DirectBinary>;
    std::array<std::uint8_t,RadioAdapters::DirectRadioPrimitivePrefixBytes+maxResponse> responseWire{};
    assert(RadioAdapters::EncodeDirectRadioPrimitivePrefix(
        Primitive::FamilyIds::Command,C::CommandProtocolVersion,responseWire.data(),responseWire.size()));
    Reply reply{81U};
    const auto encodedResponse=C::EncodeCommandResponse<QueryCommand,Serializable::DirectBinary>(
        succeeded.Request.Key(),remote,C::CommandResponseDisposition::Succeeded,&reply,
        responseWire.data()+RadioAdapters::DirectRadioPrimitivePrefixBytes,maxResponse);
    assert(encodedResponse);

    Provider provider;
    const std::uint8_t sourceBytes[2]{0x21,0x22};
    const auto source=Radio::RadioAddress::FromBytes(sourceBytes,2);
    ProvenanceContext provenanceContext{remote,remoteRoute};
    const RadioAdapters::RadioAdapterProvenanceBinding provenance{&provenanceContext,&ResolveProvenance};
    CompletionCapture responseCompletion{};
    auto disposition=RadioAdapters::AdmitDirectRadioLogicalMessage(
        adapter,registry,provenance,provider,source,Radio::RadioServiceClass::Responsive,
        {responseWire.data(),RadioAdapters::DirectRadioPrimitivePrefixBytes+encodedResponse.Bytes},701,
        {&responseCompletion,&CompletionCapture::Complete});
    assert(disposition==Adapters::AdapterSubmissionDisposition::Accepted);
    Eventually([&]{return responseCompletion.Done.load(std::memory_order_acquire);});
    assert(responseCompletion.Admission==Primitive::PrimitiveAdmissionDisposition::Accepted);
    Eventually([&]{return client.IsReady(succeeded.Request);});
    responses.Service({host.Now.load(),services});
    assert(owner.Callbacks.load(std::memory_order_acquire)==2);
    assert(owner.Kind==C::CommandCallerCompletionKind::Response&&owner.ReplyValue==81U);

    // A trusted remote request executes exactly once and its generated response reuses A2/direct Radio.
    constexpr auto maxRequest=C::MaximumCompleteRequestWireBytes<QueryCommand,Serializable::DirectBinary>;
    std::array<std::uint8_t,RadioAdapters::DirectRadioPrimitivePrefixBytes+maxRequest> requestWire{};
    assert(RadioAdapters::EncodeDirectRadioPrimitivePrefix(
        Primitive::FamilyIds::Command,C::CommandProtocolVersion,requestWire.data(),requestWire.size()));
    auto* commandRequest=requestWire.data()+RadioAdapters::DirectRadioPrimitivePrefixBytes;
    QueryCommand remoteRequest{91U};
    const auto requestPayload=Serializable::SerializeDirectBinary(
        remoteRequest,commandRequest+C::CommandRequestWireHeaderSize,maxRequest-C::CommandRequestWireHeaderSize);
    assert(requestPayload);
    const C::CommandExecutionKey remoteKey{QueryCommand::TypeId,remote.Device,remote.Incarnation,C::CommandId{44}};
    const C::CommandRequestWireHeader requestHeader{remoteKey,{900,Timing::TimeReliability::Acquiring},
        static_cast<std::uint32_t>(requestPayload.Bytes)};
    assert(C::EncodeCommandRequestHeader(requestHeader,commandRequest,maxRequest));
    const auto requestPhysicalBytes=RadioAdapters::DirectRadioPrimitivePrefixBytes+
        C::CommandRequestWireHeaderSize+requestPayload.Bytes;

    CompletionCapture requestCompletion{};
    disposition=RadioAdapters::AdmitDirectRadioLogicalMessage(
        adapter,registry,provenance,provider,source,Radio::RadioServiceClass::Responsive,
        {requestWire.data(),requestPhysicalBytes},702,{&requestCompletion,&CompletionCapture::Complete});
    assert(disposition==Adapters::AdapterSubmissionDisposition::Accepted);
    Eventually([&]{return requestCompletion.Done.load(std::memory_order_acquire);});
    assert(requestCompletion.Admission==Primitive::PrimitiveAdmissionDisposition::Accepted);
    Eventually([&]{return owner.HandlerCalls.load(std::memory_order_acquire)==1;});
    Eventually([&]{return radio.Calls.load(std::memory_order_acquire)>=3;});

    RadioAdapters::DirectRadioPrimitivePrefix generatedOuter{};
    assert(RadioAdapters::DecodeDirectRadioPrimitivePrefix(radio.Bytes.data(),radio.Size,generatedOuter));
    assert(generatedOuter.Family==Primitive::FamilyIds::Command&&generatedOuter.Protocol==C::CommandProtocolVersion);
    C::CommandResponseWireHeader generatedHeader{};
    assert(C::DecodeCommandResponseHeader(
        radio.Bytes.data()+RadioAdapters::DirectRadioPrimitivePrefixBytes,
        radio.Size-RadioAdapters::DirectRadioPrimitivePrefixBytes,generatedHeader));
    assert(generatedHeader.Key==remoteKey);
    assert(generatedHeader.Executor==local);
    assert(radio.Peer.Slot==9&&radio.Profile.Class==Radio::RadioServiceClass::Responsive);
    const auto generatedTransfer=radio.LastTransfer;
    assert(m1.HandleReceipt(remoteRoute,radio.Domain,generatedTransfer,Primitive::PrimitiveAdmissionDisposition::Accepted));
    assert(m1.ServiceOne()==Adapters::AdapterSubmissionDisposition::Accepted);

    // Exact duplicate is terminally known: no second handler execution, family reports AlreadyAccepted.
    CompletionCapture duplicateCompletion{};
    disposition=RadioAdapters::AdmitDirectRadioLogicalMessage(
        adapter,registry,provenance,provider,source,Radio::RadioServiceClass::Responsive,
        {requestWire.data(),requestPhysicalBytes},703,{&duplicateCompletion,&CompletionCapture::Complete});
    assert(disposition==Adapters::AdapterSubmissionDisposition::Accepted);
    Eventually([&]{return duplicateCompletion.Done.load(std::memory_order_acquire);});
    assert(duplicateCompletion.Admission==Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted);
    assert(owner.HandlerCalls.load(std::memory_order_acquire)==1);

    // Duplicate-terminal replay may emit the stored response, but it must remain a response replay, never re-execution.
    if(m1.OutstandingAttempts()!=0) {
        assert(m1.HandleReceipt(remoteRoute,radio.Domain,radio.LastTransfer,
            Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted));
        assert(m1.ServiceOne()==Adapters::AdapterSubmissionDisposition::Accepted);
    }

    assert(commandRuntime.Shutdown()==C::CommandRuntimeStatus::Success);
    assert(adapter.Shutdown()==Adapters::AdapterRuntimeStatus::Success);
    return 0;
}
