#include <ESPressio_RadioAdapters.hpp>

#include <array>
#include <cassert>
#include <cstdint>

using namespace ESPressio;

namespace {

constexpr bool PrefixVector() {
    std::array<std::uint8_t,RadioAdapters::DirectRadioPrimitivePrefixBytes> bytes{};
    if(!RadioAdapters::EncodeDirectRadioPrimitivePrefix(0x1234U,0xabcdU,bytes.data(),bytes.size())) return false;
    if(bytes[0]!=0x34U||bytes[1]!=0x12U||bytes[2]!=0xcdU||bytes[3]!=0xabU) return false;
    RadioAdapters::DirectRadioPrimitivePrefix decoded{};
    return RadioAdapters::DecodeDirectRadioPrimitivePrefix(bytes.data(),bytes.size(),decoded)&&
        decoded.Family==0x1234U&&decoded.Protocol==0xabcdU;
}

constexpr bool ZeroVectorIsExactScalar() {
    std::array<std::uint8_t,RadioAdapters::DirectRadioPrimitivePrefixBytes> bytes{{1,1,1,1}};
    if(!RadioAdapters::EncodeDirectRadioPrimitivePrefix(0,0,bytes.data(),bytes.size())) return false;
    RadioAdapters::DirectRadioPrimitivePrefix decoded{1,1};
    return bytes[0]==0&&bytes[1]==0&&bytes[2]==0&&bytes[3]==0&&
        RadioAdapters::DecodeDirectRadioPrimitivePrefix(bytes.data(),bytes.size(),decoded)&&
        decoded.Family==0&&decoded.Protocol==0;
}

constexpr bool ServiceMappingsAreExplicitAndComplete() {
    using A=Adapters::AdapterServiceClass;
    using R=Radio::RadioServiceClass;
    if(RadioAdapters::ToRadioServiceClass(A::Infrastructure)!=R::Infrastructure) return false;
    if(RadioAdapters::ToRadioServiceClass(A::Clock)!=R::Clock) return false;
    if(RadioAdapters::ToRadioServiceClass(A::Critical)!=R::Critical) return false;
    if(RadioAdapters::ToRadioServiceClass(A::Responsive)!=R::Responsive) return false;
    if(RadioAdapters::ToRadioServiceClass(A::Convergent)!=R::Convergent) return false;
    if(RadioAdapters::ToRadioServiceClass(A::BestEffort)!=R::BestEffort) return false;
    A mapped=A::BestEffort;
    return RadioAdapters::TryToAdapterServiceClass(R::Infrastructure,mapped)&&mapped==A::Infrastructure&&
        RadioAdapters::TryToAdapterServiceClass(R::Clock,mapped)&&mapped==A::Clock&&
        RadioAdapters::TryToAdapterServiceClass(R::Critical,mapped)&&mapped==A::Critical&&
        RadioAdapters::TryToAdapterServiceClass(R::Responsive,mapped)&&mapped==A::Responsive&&
        RadioAdapters::TryToAdapterServiceClass(R::Convergent,mapped)&&mapped==A::Convergent&&
        RadioAdapters::TryToAdapterServiceClass(R::BestEffort,mapped)&&mapped==A::BestEffort;
}

static_assert(RadioAdapters::DirectRadioPrimitivePrefixBytes==4);
static_assert(PrefixVector());
static_assert(ZeroVectorIsExactScalar());
static_assert(ServiceMappingsAreExplicitAndComplete());

} // namespace

int main() {
    std::array<std::uint8_t,4> bytes{};
    assert(!RadioAdapters::EncodeDirectRadioPrimitivePrefix(1,1,nullptr,4));
    assert(!RadioAdapters::EncodeDirectRadioPrimitivePrefix(1,1,bytes.data(),3));
    RadioAdapters::DirectRadioPrimitivePrefix decoded{};
    assert(!RadioAdapters::DecodeDirectRadioPrimitivePrefix(nullptr,4,decoded));
    assert(!RadioAdapters::DecodeDirectRadioPrimitivePrefix(bytes.data(),3,decoded));

    auto invalidRadio=static_cast<Radio::RadioServiceClass>(0xffU);
    Adapters::AdapterServiceClass mapped=Adapters::AdapterServiceClass::BestEffort;
    assert(!RadioAdapters::TryToAdapterServiceClass(invalidRadio,mapped));
    assert(RadioAdapters::ToRadioServiceClass(static_cast<Adapters::AdapterServiceClass>(0xffU))==
           Radio::RadioServiceClass::Invalid);
    return 0;
}
