#pragma once

#define STR(x) #x

template<typename T>
inline T RoundUp(const T& in_rValue, uint32_t in_round)
{
    return (in_rValue + in_round - 1) & ~(T(in_round) - 1);
}

template<typename T>
inline T RoundDown(const T& in_rValue, uint32_t in_round)
{
    return in_rValue & ~(T(in_round) - 1);
}

inline size_t StringHash(const std::string_view& str)
{
    return XXH3_64bits(str.data(), str.size());
}

template<typename TValue>
constexpr size_t FirstBitLow(TValue value)
{
    constexpr size_t nbits = sizeof(TValue) * 8;
    constexpr auto zero = TValue{};
    constexpr auto one = static_cast<TValue>(1);

    for (size_t i = 0; i < nbits; i++)
    {
        if ((value & (one << i)) != zero)
        {
            return i;
        }
    }

    return 0;
}

inline std::vector<uint8_t> ReadAllBytes(const std::filesystem::path& filePath)
{
    std::vector<uint8_t> data;
    std::ifstream file(filePath, std::ios::binary);
    if (!file)
        return data;

    file.seekg(0, std::ios::end);
    data.resize(size_t(file.tellg()));
    file.seekg(0, std::ios::beg);
    file.read((char*)data.data(), data.size());
    return data;
}
