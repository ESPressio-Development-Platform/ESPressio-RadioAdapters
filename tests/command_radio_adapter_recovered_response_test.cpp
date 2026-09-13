#include <ESPressio_Adapters.hpp>
#include <ESPressio_RadioAdapters.hpp>
#include <ESPressio_Commands.hpp>
#include <ESPressio_Persistence.hpp>
#include <ESPressio_SerializationMacros.hpp>
#include <HostRuntime.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
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

struct RecoveryResponse final {
    std::uint32_t Value=0;
    ESPRESSIO_SERIALIZABLE_TYPE(RecoveryResponse)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(ESPRESSIO_PROPERTY("value",Value))
};

struct RecoveryRetention final {
    static constexpr std::size_t MaximumTrackedOrigins=1;
    static constexpr std::size_t ReplayWindowEntries=2;
    using ResultRetention=C::PersistentResults<1,512>;
};

struct RecoveryCommand final : C::TransmissibleCommand<RecoveryCommand,RecoveryResponse> {
    static constexpr C::CommandTypeId TypeId{0x5203};
    static constexpr std::string_view CanonicalName="RadioAdapters.Command.RecoveredResponse";
    static constexpr std::size_t MaximumLiveInstances=1;
    static constexpr std::size_t MaximumPendingExecutions=0;
    static constexpr std::size_t MaximumPendingResponses=1;
    using ExecutionAdmissionPolicy=C::RequiredExecution;
    using RequestDeliveryPolicy=EvidencePolicy;
    using ResponseDeliveryPolicy=EvidencePolicy;
    using CompletionRetentionPolicy=RecoveryRetention;
    std::uint32_t Value=0;
    RecoveryCommand() noexcept=default;
    explicit RecoveryCommand(std::uint32_t value) noexcept:Value(value){}
    ESPRESSIO_SERIALIZABLE_TYPE(RecoveryCommand)
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
    mutable std::mutex _mutex;
    std::array<Entry,Records> _entries{};
public:
    Persistence::AtomicRecordCapabilities Capabilities() const noexcept override { return {true,true,RecordBytes,Records}; }
    Persistence::AtomicRecordStatus Recover() noexcept override { return Persistence::AtomicRecordStatus::Success; }
    Persistence::AtomicRecordStatus Read(const Persistence::AtomicRecordKey& key,std::uint8_t* buffer,
                                         std::size_t capacity,std::size_t& bytesRead) noexcept override {
        std::lock_guard<std::mutex> lock(_mutex);
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
        std::lock_guard<std::mutex> lock(_mutex);
        if(!key||!data||size>RecordBytes) return Persistence::AtomicRecordStatus::NoSpace;
        Entry* target=nullptr;
        for(auto& entry:_entries) {
            if(entry.Used&&entry.Key==key) { target=&entry;break; }
            if(!entry.Used&&!target) target=&entry;
        }
        if(!target) return Persistence::AtomicRecordStatus::NoSpace;
        target->Used=true;
        target->Key=key;
        target->Size=size;
        std::memcpy(target->Bytes.data(),data,size);
        return Persistence::AtomicRecordStatus::Success;
    }
    Persistence::AtomicRecordStatus RemoveAfterCommit(const Persistence::AtomicRecordKey& key) noexcept override {
        std::lock_guard<std::mutex> lock(_mutex);
        for(auto& entry:_entries) {
            if(entry.Used&&entry.Key==key) {
                entry=Entry{};
                break;
            }
        }
        return Persistence::AtomicRecordStatus::Success;
    }
};

Persistence::AtomicRecordKey StoreKey(std::string_view text) {
    Persistence::AtomicRecordKey key;
    assert(Persistence::AtomicRecordKey::TryCreate(text,key));
    return key;
}

System::DeviceIdentifier Device(std::uint8_t marker) {
    System::DeviceIdentifier::Storage bytes{};
    bytes.back()=marker;
    return System::DeviceIdentifier{bytes};
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
    RecoveryResponse Handle(const RecoveryCommand& command,const C::CommandExecutionContext&) {
        ++Calls;
        return {command.Value+1};
    }
};

struct RouteResolver final {
    static bool Resolve(void*,System::DeviceIdentifier device,Adapters::AdapterRouteToken& route) noexcept {
        if(!device) return false;
        route.Value=0xF000U+device.Bytes().back();
        return true;
    }
    static bool Validate(void*) noexcept { return true; }
};

using Arena=Adapters::StaticByteArena<Adapters::ByteClass<256,8>,Adapters::ByteClass<1024,4>>;
using Domain=Adapters::StaticCapacityDomain<1024,4,Arena>;
using Inbound=Adapters::CapacityPlane<Adapters::AdapterDirection::Inbound,
    Domain,Domain,Domain,Domain,Domain,Domain,Domain,Domain>;
using Outbound=Adapters::CapacityPlane<Adapters::AdapterDirection::Outbound,
    Domain,Domain,Domain,Domain,Domain,Domain,Domain,Domain>;
using AdapterRuntime=Adapters::AdapterRuntime<Inbound,Outbound,2,4,1,1,8>;

struct LowerTransport final {
    std::array<std::uint8_t,512> Last{};
    std::size_t Bytes=0;
    Adapters::AdapterRouteToken Route{};
    Adapters::AdapterServiceClass Service{Adapters::AdapterServiceClass::BestEffort};
    std::atomic<unsigned> Calls{0};

    static bool Validate(void*) noexcept { return true; }
    static Adapters::LowerTransportSubmitResult Submit(
        void* owner,
        Adapters::AdapterRecordIdentity,
        Primitive::PrimitiveFamilyId,
        Primitive::PrimitiveProtocolVersion,
        const Primitive::PrimitivePolicyDescriptor&,
        Adapters::AdapterServiceClass service,
        Adapters::AdapterByteView bytes,
        Adapters::AdapterRouteToken route) noexcept {
        auto& self=*static_cast<LowerTransport*>(owner);
        assert(bytes.Data&&bytes.Size<=self.Last.size());
        std::memcpy(self.Last.data(),bytes.Data,bytes.Size);
        self.Bytes=bytes.Size;
        self.Route=route;
        self.Service=service;
        const auto generation=++self.Calls;
        return {Adapters::LowerTransportDisposition::Accepted,generation,false};
    }

    Adapters::LowerTransportBinding Binding() noexcept {
        Adapters::LowerTransportBinding binding{};
        binding.Owner=this;
        binding.Submit=&LowerTransport::Submit;
        binding.Validate=&LowerTransport::Validate;
        binding.ServiceClassMask=0x3f;
        binding.ProvidesDestinationPrimitiveAdmission=true;
        binding.ProvidesValidatedOriginalSource=false;
        return binding;
    }
};

template<class TFamily>
struct OutboundOwner final {
    TFamily* Family=nullptr;
    C::CommandOutboundAdmission Admit(
        System::DeviceIdentifier target,const C::CommandRequestLease<RecoveryCommand>& request,
        C::CommandRequestDeliveryToken token) noexcept {
        return Family->template SubmitRequest<RecoveryCommand>(target,request,token);
    }
    bool Validate(const C::CommandOutboundContract& contract) noexcept {
        return Family->template ValidateOutboundContract<RecoveryCommand,Serializable::DirectBinary>(contract);
    }
    C::CommandRemoteResponseDestination ReserveRecovered(const C::CommandExecutionKey& key) noexcept {
        return Family->template ReserveRecoveredResponse<RecoveryCommand>(key);
    }
    void ReleaseRecovered(C::CommandRemoteResponseDestination destination) noexcept {
        Family->ReleaseRecoveredResponse(destination);
    }
};

Task::TaskExecutorConfiguration RouterConfiguration() {
    Task::TaskExecutorConfiguration configuration{};
    configuration.Execution.Name="radioCmdRecoveryRouter";
    configuration.Execution.StackSize=4096;
    configuration.QueueDepth=1;
    configuration.OverflowPolicy=Task::TaskQueueOverflowPolicy::Reject;
    configuration.QueueMemoryPolicy=Task::TaskMemoryPolicy::Internal;
    return configuration;
}

} // namespace

int main() {
    HostRuntime platform;
    const System::DeviceRuntimeIdentity current{Device(1),System::RuntimeIncarnationId{41}};
    const System::DeviceRuntimeIdentity prior{current.Device,System::RuntimeIncarnationId{17}};
    const C::CommandExecutionKey recoveredKey{
        RecoveryCommand::TypeId,Device(3),System::RuntimeIncarnationId{5},C::CommandId{77}};
    assert(System::RuntimeIdentity::Install(current)==System::RuntimeIdentity::InstallationStatus::Success);

    Store<2048,8> ledgerStore,resultStore;
    const auto ledgerKey=StoreKey("radio-recovery-ledger");
    {
        C::CommandExecutionLedger<RecoveryCommand> seed;
        assert(seed.Bind({&ledgerStore,ledgerKey,&resultStore,C::CommandPayloadFormat::DirectBinary,true})==
               C::CommandRuntimeStatus::Success);
        assert(seed.Initialize()==C::CommandRuntimeStatus::Success);
        assert(seed.ReservePersistentResult(recoveredKey));
        auto reserved=seed.TryReserve(recoveredKey);
        assert(reserved);
        assert(reserved.Reserved.CommitStarted(prior.Incarnation));
        assert(reserved.Reserved.CommitTerminal(C::CommandResponseDisposition::Succeeded));
        assert(seed.PersistPersistentResult(recoveredKey,prior,RecoveryResponse{321}));
    }

    Primitive::TypeDirectory<1> directory;
    assert(directory.Register<RecoveryCommand>()==Primitive::TypeDirectoryRegistrationStatus::Success);
    assert(directory.Initialize()==Primitive::TypeDirectoryInitializationStatus::Success);

    AdapterRuntime adapter;
    RouteResolver routes;
    const RadioAdapters::RadioAdapterSemanticRouteBinding routeBinding{
        &routes,&RouteResolver::Resolve,&RouteResolver::Validate};
    using Family=RadioAdapters::CommandRadioAdapterFamilyBinding<AdapterRuntime,1,2>;
    Family family(adapter,routeBinding);
    assert((family.ConfigureType<RecoveryCommand,Serializable::DirectBinary>(
                Adapters::AdapterServiceClass::Responsive,Adapters::AdapterServiceClass::Responsive)==
            RadioAdapters::CommandRadioAdapterBindingStatus::Success));

    OutboundOwner<Family> outboundOwner{&family};
    C::CommandOutboundBinding<RecoveryCommand,Serializable::DirectBinary> outbound;
    assert((outbound.Initialize<OutboundOwner<Family>,
        &OutboundOwner<Family>::Admit,&OutboundOwner<Family>::Validate,
        &OutboundOwner<Family>::ReserveRecovered,&OutboundOwner<Family>::ReleaseRecovered>(outboundOwner)));

    C::CommandResponseRouter<1> router(RouterConfiguration());
    C::RuntimeConfiguration configuration{};
    configuration.ExecutionLane.Name="radioCmdRecovery";
    configuration.ExecutionLane.StackSize=4096;
    configuration.ResponseRouter=router.Binding();
    C::Runtime runtime(configuration);
    Handler handler;
    assert(runtime.BindHandler<RecoveryCommand>(handler,&Handler::Handle)==C::CommandRuntimeStatus::Success);
    assert(runtime.BindPersistence<RecoveryCommand>(
        ledgerStore,ledgerKey,resultStore,C::CommandPayloadFormat::DirectBinary)==C::CommandRuntimeStatus::Success);
    assert((runtime.BindTransport<RecoveryCommand,Serializable::DirectBinary>(outbound)==C::CommandRuntimeStatus::Success));

    // Runtime recovery reserves the semantic return route before the RadioAdapter family is frozen or A2 is started.
    assert(runtime.Initialize(directory.View())==C::CommandRuntimeStatus::Success);
    assert((family.AttachRuntime<RecoveryCommand,Serializable::DirectBinary>(runtime)==
            RadioAdapters::CommandRadioAdapterBindingStatus::Success));
    assert(family.Freeze()==RadioAdapters::CommandRadioAdapterBindingStatus::Success);

    LowerTransport lower;
    assert(adapter.BindFamily(family.AdapterBinding())==Adapters::AdapterRuntimeStatus::Success);
    assert(adapter.BindTransport(lower.Binding())==Adapters::AdapterRuntimeStatus::Success);
    Task::TaskExecutionConfiguration worker{};
    worker.Name="radioA2CmdRecovery";
    worker.StackSize=4096;
    assert(adapter.Initialize(worker,worker)==Adapters::AdapterRuntimeStatus::Success);
    assert(adapter.Start()==Adapters::AdapterRuntimeStatus::Success);

    assert(runtime.Start()==C::CommandRuntimeStatus::Success);
    Eventually([&]{ return lower.Calls.load()>=1; });
    assert(handler.Calls.load()==0);
    assert(lower.Route.Value==0xF003U);
    assert(lower.Service==Adapters::AdapterServiceClass::Responsive);

    C::CommandResponseWireHeader header{};
    assert(C::DecodeCommandResponseHeader(lower.Last.data(),lower.Bytes,header));
    assert(header.Key==recoveredKey);
    assert(header.Executor==prior);
    assert(header.Disposition==C::CommandResponseDisposition::Succeeded);
    RecoveryResponse response{};
    const auto decoded=Serializable::DeserializeBoundedDirectBinary(
        lower.Last.data()+C::CommandResponseWireHeaderSize,
        lower.Bytes-C::CommandResponseWireHeaderSize,response);
    assert(decoded&&response.Value==321U);

    assert(runtime.Shutdown()==C::CommandRuntimeStatus::Success);
    assert(adapter.Shutdown()==Adapters::AdapterRuntimeStatus::Success);
    return 0;
}
