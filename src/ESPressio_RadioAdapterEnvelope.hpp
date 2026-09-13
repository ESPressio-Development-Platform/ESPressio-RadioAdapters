#pragma once

#include <cstddef>
#include <cstdint>

#include <ESPressio_PrimitiveTypes.hpp>

namespace ESPressio::RadioAdapters {

/// <summary>Exact canonical direct-Radio Primitive prefix size.</summary>
inline constexpr std::size_t DirectRadioPrimitivePrefixBytes=4;

/// <summary>Decoded family/version facts from the exact four-byte direct-Radio Primitive prefix.</summary>
struct DirectRadioPrimitivePrefix final {
    Primitive::PrimitiveFamilyId Family{0};
    Primitive::PrimitiveProtocolVersion Protocol{0};

    constexpr bool IsValid() const noexcept { return Family!=0 && Protocol!=0; }
};

/// <summary>
/// Encodes the locked direct-Radio `{PrimitiveFamilyId, PrimitiveProtocolVersion}` prefix.
/// </summary>
/// <remarks>
/// Both 16-bit fields use the platform canonical little-endian Primitive encoding. Payload size,
/// serialization format, service class, fingerprints and provenance are deliberately not serialized here.
/// </remarks>
constexpr bool EncodeDirectRadioPrimitivePrefix(
    Primitive::PrimitiveFamilyId family,
    Primitive::PrimitiveProtocolVersion protocol,
    std::uint8_t* output,
    std::size_t capacity) noexcept {
    if(family==0 || protocol==0 || output==nullptr || capacity<DirectRadioPrimitivePrefixBytes) return false;
    output[0]=static_cast<std::uint8_t>(family&0xffU);
    output[1]=static_cast<std::uint8_t>((family>>8U)&0xffU);
    output[2]=static_cast<std::uint8_t>(protocol&0xffU);
    output[3]=static_cast<std::uint8_t>((protocol>>8U)&0xffU);
    return true;
}

/// <summary>Decodes and validates the exact canonical four-byte direct-Radio Primitive prefix.</summary>
constexpr bool DecodeDirectRadioPrimitivePrefix(
    const std::uint8_t* bytes,
    std::size_t size,
    DirectRadioPrimitivePrefix& prefix) noexcept {
    prefix={};
    if(bytes==nullptr || size<DirectRadioPrimitivePrefixBytes) return false;
    const auto family=static_cast<Primitive::PrimitiveFamilyId>(
        static_cast<std::uint16_t>(bytes[0]) |
        (static_cast<std::uint16_t>(bytes[1])<<8U));
    const auto protocol=static_cast<Primitive::PrimitiveProtocolVersion>(
        static_cast<std::uint16_t>(bytes[2]) |
        (static_cast<std::uint16_t>(bytes[3])<<8U));
    if(family==0 || protocol==0) return false;
    prefix={family,protocol};
    return true;
}

} // namespace ESPressio::RadioAdapters
