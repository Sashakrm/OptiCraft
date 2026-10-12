#ifndef OPTICRAFT_FAST_RNG_H
#define OPTICRAFT_FAST_RNG_H

#include <cstdint>
#include <limits>

// Быстрый детерминированный генератор (splitmix64) для декораций: деревья, цветы, трава.
// Это обычный UniformRandomBitGenerator, поэтому работает с std::uniform_int_distribution.
// Зачем не std::mt19937: засев mt19937 заполняет 624 слова состояния (несколько тысяч
// инструкций), а декорации создают по генератору на каждую колонку каждого чанка.
class Fast_Rng {
public:
    using result_type = uint32_t;

    explicit Fast_Rng(uint64_t seed) : m_state(seed) {}

    static constexpr result_type min() { return std::numeric_limits<result_type>::min(); }
    static constexpr result_type max() { return std::numeric_limits<result_type>::max(); }

    result_type operator()() {
        m_state += 0x9E3779B97F4A7C15ull;
        uint64_t z = m_state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return static_cast<result_type>((z ^ (z >> 31)) >> 32);
    }

private:
    uint64_t m_state;
};

#endif // OPTICRAFT_FAST_RNG_H
