#include <ESPressio_Adapters.hpp>
#include <ESPressio_CommandRadioAdapterBinding.hpp>
#include <ESPressio_Persistence.hpp>
#include <ESPressio_RadioAdapterIngress.hpp>
#include <ESPressio_RadioAdapterLowerTransport.hpp>
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

struct NoEvidencePolicy final {
    using PolicyCategory=Primitive::OccurrenceDeliveryPolicyTag;
    using RequiredEvidence=Primitive::NoRemoteEvidence;
    using TerminalDisposition=Primitive::DiagnosticOnlyAfterBudget;
    static constexpr std::uint64_t MaximumResidenceNanoseconds=0;
    static constexpr std::uint16_t MaximumAttempts=1;
    static constexpr std::uint64_t MaximumAdapterAdmissionWaitNanoseconds=0;
    static constexpr std::uint64_t MinimumRetrySpacingNanoseconds=0;
    static constexpr std::uint64_t MaximumRetrySpacingNanoseconds=0;
};

struct Retention final {
    static constexpr std::size_t MaximumTrackedOrigins=2;
    static constexpr std::size_t ReplayWindowEntries=4;
    using ResultRetention=C::VolatileResults;
};

struct FireCommand final : C::TransmissibleCommand<FireCommand,C::NoCommandResponse> {
    static constexpr C::CommandTypeId TypeId{0x6201};
    static constexpr std::string_view CanonicalName="RadioAdapters.Command.Direct";
    static constexpr std::size_t MaximumLiveInstances=3;
    static constexpr std::size_t MaximumPendingExecutions=1;
    using ExecutionAdmissionPolicy=C::RequiredExecution;
    using RequestDeliveryPolicy=NoEvidencePolicy;
    using CompletionRetentionPolicy=Retention;
    std::uint32_t Value=0;
    FireCommand() noexcept=default;
    explicit FireCommand(std::uint32_t value) noexcept:Value(value){}
    ESPRESSIO_SERIALIZABLE_TYPE(FireCommand)
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

struct Handler final {
    std::atomic<unsigned> Calls{0};
    std::atomic<std::uint32_t> Last{0};
    void Fire(const FireCommand& command,const C::CommandExecutionContext&) {
        Last.store(command.Value,std::memory_order_release);
        Calls.fetch_add(1,std::memory_order_release);
    }
};

struct SemanticRoutes final {
    static bool Validate(void*) noexcept { return true; }
    static bool Resolve(void*,System::DeviceIdentifier device,Adapters::AdapterRouteToken& route) noexcept {
        if(!device) return false;
        route.Value=0xA000ULL+device.Bytes().back();
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
        return {Radio::RadioSchedulerStatus::Success,Radio::RadioTransferId{19}};
    }
};

bool ResolveLowerRoute(void*,Adapters::AdapterRouteToken route,Radio::RadioPeerHandle& peer) noexcept {
    if(route.Value<0xA001ULL||route.Value>0xA0ffULL) { peer={};return false; }
    peer={static_cast<std::uint16_t>(route.Value&0xffU),7};
    return true;
}
bool ValidateLowerRoute(void*) noexcept { return true; }

bool ResolveTransferPolicy(
    void*,Primitive::PrimitiveFamilyId family,Primitive::PrimitiveProtocolVersion protocol,
    const Primitive::PrimitivePolicyDescriptor& policy,Adapters::AdapterServiceClass service,std::uint64_t now,
    Radio::RadioServiceProfile& profile,Radio::RadioTransferTiming& timing) noexcept {
    if(family!=Primitive::FamilyIds::Command||protocol!=C::CommandProtocolVersion||policy.Evidence!=0||
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

template<class TFamily>
struct OutboundOwner final {
    TFamily* Family=nullptr;
    C::CommandOutboundAdmission Admit(System::DeviceIdentifier target,
        const C::CommandRequestLease<FireCommand>& request,C::CommandRequestDeliveryToken token) noexcept {
        return Family->template SubmitRequest<FireCommand>(target,request,token);
    }
    bool Validate(const C::CommandOutboundContract& contract) noexcept {
        return Family->template ValidateOutboundContract<FireCommand,Serializable::DirectBinary>(contract);
    }
};

} // namespace

int main() {
    HostRuntime platform;
    const auto local=Identity(1,17);
    const auto remote=Identity(7,31);
    assert(System::RuntimeIdentity::Install(local)==System::RuntimeIdentity::InstallationStatus::Success);

    Primitive::TypeDirectory<1> directory;
    assert(directory.Register<FireCommand>()==Primitive::TypeDirectoryRegistrationStatus::Success);
    assert(directory.Initialize()==Primitive::TypeDirectoryInitializationStatus::Success);

    AdapterRuntime adapter;
    SemanticRoutes routes;
    const RadioAdapters::RadioAdapterSemanticRouteBinding semanticRoutes{
        &routes,&SemanticRoutes::Resolve,&SemanticRoutes::Validate};
    using Family=RadioAdapters::CommandRadioAdapterFamilyBinding<AdapterRuntime,1,2>;
    Family family(adapter,semanticRoutes);
    assert((family.ConfigureType<FireCommand,Serializable::DirectBinary>(Adapters::AdapterServiceClass::BestEffort)==
            RadioAdapters::CommandRadioAdapterBindingStatus::Success));

    OutboundOwner<Family> outboundOwner{&family};
    C::CommandOutboundBinding<FireCommand,Serializable::DirectBinary> outbound;
    assert((outbound.Initialize<OutboundOwner<Family>,&OutboundOwner<Family>::Admit,&OutboundOwner<Family>::Validate>(outboundOwner)));

    Store<4096,4> store;
    C::RuntimeConfiguration commandConfiguration{};
    commandConfiguration.ExecutionLane.Name="radioCmd";
    commandConfiguration.ExecutionLane.StackSize=4096;
    C::Runtime commandRuntime(commandConfiguration);
    Handler handler;
    assert(commandRuntime.BindHandler<FireCommand>(handler,&Handler::Fire)==C::CommandRuntimeStatus::Success);
    assert(commandRuntime.BindPersistence<FireCommand>(store,StoreKey("radio-command"))==C::CommandRuntimeStatus::Success);
    assert((commandRuntime.BindTransport<FireCommand,Serializable::DirectBinary>(outbound)==C::CommandRuntimeStatus::Success));
    assert(commandRuntime.Initialize(directory.View())==C::CommandRuntimeStatus::Success);
    assert((family.AttachRuntime<FireCommand,Serializable::DirectBinary>(commandRuntime)==
            RadioAdapters::CommandRadioAdapterBindingStatus::Success));
    assert(family.Freeze()==RadioAdapters::CommandRadioAdapterBindingStatus::Success);

    RadioAdapters::RadioAdapterBindingRegistry<1> registry;
    assert(registry.Bind(family.RadioBinding())==Adapters::AdapterRuntimeStatus::Success);
    assert(registry.Freeze()==Adapters::AdapterRuntimeStatus::Success);

    FakeRadioRuntime radioRuntime;
    RadioAdapters::RadioAdapterRouteBinding lowerRoutes{&routes,&ResolveLowerRoute,&ValidateLowerRoute};
    RadioAdapters::RadioAdapterTransferPolicyBinding transferPolicy{&routes,&ResolveTransferPolicy,&ValidateTransferPolicy};
    RadioAdapters::RadioAdapterLowerTransport<FakeRadioRuntime,1024> lower(radioRuntime,lowerRoutes,transferPolicy);

    assert(adapter.BindFamily(family.AdapterBinding())==Adapters::AdapterRuntimeStatus::Success);
    assert(adapter.BindTransport(lower.AdapterBinding())==Adapters::AdapterRuntimeStatus::Success);
    Task::TaskExecutionConfiguration worker{};worker.Name="radioA2Cmd";worker.StackSize=4096;
    assert(adapter.Initialize(worker,worker)==Adapters::AdapterRuntimeStatus::Success);
    assert(adapter.Start()==Adapters::AdapterRuntimeStatus::Success);
    assert(commandRuntime.Start()==C::CommandRuntimeStatus::Success);

    const auto submitted=C::CommandTypeRuntime<FireCommand>::Get().SubmitRemoteNoResponse<false>(remote.Device,73U);
    assert(static_cast<bool>(submitted));
    Eventually([&]{return radioRuntime.Calls.load(std::memory_order_acquire)>=1;});
    assert(radioRuntime.Peer.ProviderSlot==7);
    assert(radioRuntime.Profile.Class==Radio::RadioServiceClass::BestEffort);
    assert(radioRuntime.Correlation==0);

    RadioAdapters::DirectRadioPrimitivePrefix outer{};
    assert(RadioAdapters::DecodeDirectRadioPrimitivePrefix(radioRuntime.Bytes.data(),radioRuntime.Size,outer));
    assert(outer.Family==Primitive::FamilyIds::Command&&outer.Protocol==C::CommandProtocolVersion);
    const auto* commandBytes=radioRuntime.Bytes.data()+RadioAdapters::DirectRadioPrimitivePrefixBytes;
    const auto commandSize=radioRuntime.Size-RadioAdapters::DirectRadioPrimitivePrefixBytes;
    C::CommandRequestWireHeader outboundHeader{};
    assert(C::DecodeCommandRequestHeader(commandBytes,commandSize,outboundHeader));
    assert(outboundHeader.Key.TypeId==FireCommand::TypeId);
    assert(outboundHeader.Key.OriginDevice==local.Device);
    assert(outboundHeader.Key.OriginRuntime==local.Incarnation);
    assert(outboundHeader.Key.Id==submitted.Id);
    FireCommand outboundPayload{};
    const auto decoded=Serializable::DeserializeBoundedDirectBinary(
        commandBytes+C::CommandRequestWireHeaderSize,commandSize-C::CommandRequestWireHeaderSize,outboundPayload);
    assert(decoded&&outboundPayload.Value==73U);

    constexpr auto maximum=C::MaximumCompleteRequestWireBytes<FireCommand,Serializable::DirectBinary>;
    std::array<std::uint8_t,RadioAdapters::DirectRadioPrimitivePrefixBytes+maximum> inbound{};
    assert(RadioAdapters::EncodeDirectRadioPrimitivePrefix(
        Primitive::FamilyIds::Command,C::CommandProtocolVersion,inbound.data(),inbound.size()));
    auto* inboundCommand=inbound.data()+RadioAdapters::DirectRadioPrimitivePrefixBytes;
    FireCommand remotePayload{91U};
    const auto payload=Serializable::SerializeDirectBinary(
        remotePayload,inboundCommand+C::CommandRequestWireHeaderSize,maximum-C::CommandRequestWireHeaderSize);
    assert(payload);
    const C::CommandExecutionKey key{FireCommand::TypeId,remote.Device,remote.Incarnation,C::CommandId{44}};
    const C::CommandRequestWireHeader inboundHeader{key,{900,Timing::TimeReliability::Acquiring},
        static_cast<std::uint32_t>(payload.Bytes)};
    assert(C::EncodeCommandRequestHeader(inboundHeader,inboundCommand,maximum));
    const auto inboundCommandBytes=C::CommandRequestWireHeaderSize+payload.Bytes;
    const auto inboundPhysicalBytes=RadioAdapters::DirectRadioPrimitivePrefixBytes+inboundCommandBytes;

    FakeProvider provider;
    const std::uint8_t sourceBytes[2]{0x21,0x22};
    const auto source=Radio::RadioAddress::FromBytes(sourceBytes,2);
    ProvenanceContext provenanceContext{remote,Adapters::AdapterRouteToken{0xA007}};
    const RadioAdapters::RadioAdapterProvenanceBinding provenance{&provenanceContext,&ResolveProvenance};

    CompletionCapture accepted{};
    auto disposition=RadioAdapters::AdmitDirectRadioLogicalMessage(
        adapter,registry,provenance,provider,source,Radio::RadioServiceClass::BestEffort,
        {inbound.data(),inboundPhysicalBytes},501,{&accepted,&CompletionCapture::Complete});
    assert(disposition==Adapters::AdapterSubmissionDisposition::Accepted);
    Eventually([&]{return accepted.Done.load(std::memory_order_acquire);});
    assert(accepted.Admission==Primitive::PrimitiveAdmissionDisposition::Accepted);
    Eventually([&]{return handler.Calls.load(std::memory_order_acquire)==1;});
    assert(handler.Last.load(std::memory_order_acquire)==91U);

    disposition=RadioAdapters::AdmitDirectRadioLogicalMessage(
        adapter,registry,provenance,provider,source,Radio::RadioServiceClass::Critical,
        {inbound.data(),inboundPhysicalBytes});
    assert(disposition==Adapters::AdapterSubmissionDisposition::Rejected);
    assert(handler.Calls.load(std::memory_order_acquire)==1);

    provenanceContext.Source=Identity(8,31);
    CompletionCapture forged{};
    disposition=RadioAdapters::AdmitDirectRadioLogicalMessage(
        adapter,registry,provenance,provider,source,Radio::RadioServiceClass::BestEffort,
        {inbound.data(),inboundPhysicalBytes},502,{&forged,&CompletionCapture::Complete});
    assert(disposition==Adapters::AdapterSubmissionDisposition::Accepted);
    Eventually([&]{return forged.Done.load(std::memory_order_acquire);});
    assert(forged.Admission==Primitive::PrimitiveAdmissionDisposition::Rejected);
    assert(handler.Calls.load(std::memory_order_acquire)==1);

    assert(commandRuntime.Shutdown()==C::CommandRuntimeStatus::Success);
    assert(adapter.Shutdown()==Adapters::AdapterRuntimeStatus::Success);
    return 0;
}
