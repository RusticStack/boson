#pragma once

#if __has_include(<inplace_vector>)
#include <inplace_vector>

namespace boson {
template <typename T, std::size_t N>
using inplace_vector = std::inplace_vector<T, N>;
} // namespace boson
#else

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace boson {

/// Fixed-capacity stack-allocated vector with zero heap allocations.
template <typename T, std::size_t N>
class inplace_vector {
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;
    using iterator = T*;
    using const_iterator = const T*;

    constexpr inplace_vector() noexcept = default;

    inplace_vector(std::initializer_list<T> init) {
        if (init.size() > N) {
            throw std::bad_alloc{};
        }
        for (const auto& item : init) {
            emplace_back(item);
        }
    }

    inplace_vector(const inplace_vector& other) {
        for (size_type i = 0; i < other.m_size; ++i) {
            emplace_back(other[i]);
        }
    }

    inplace_vector(inplace_vector&& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        for (size_type i = 0; i < other.m_size; ++i) {
            emplace_back(std::move(other[i]));
        }
        other.clear();
    }

    inplace_vector& operator=(const inplace_vector& other) {
        if (this == &other) {
            return *this;
        }
        clear();
        for (size_type i = 0; i < other.m_size; ++i) {
            emplace_back(other[i]);
        }
        return *this;
    }

    inplace_vector& operator=(inplace_vector&& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        if (this == &other) {
            return *this;
        }
        clear();
        for (size_type i = 0; i < other.m_size; ++i) {
            emplace_back(std::move(other[i]));
        }
        other.clear();
        return *this;
    }

    ~inplace_vector() {
        clear();
    }

    template <typename... Args>
    reference emplace_back(Args&&... args) {
        if (m_size >= N) {
            throw std::bad_alloc{};
        }
        pointer ptr = std::construct_at(slot_ptr(m_size), std::forward<Args>(args)...);
        ++m_size;
        return *ptr;
    }

    template <typename... Args>
    pointer try_emplace_back(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        if (m_size >= N) {
            return nullptr;
        }
        pointer ptr = std::construct_at(slot_ptr(m_size), std::forward<Args>(args)...);
        ++m_size;
        return ptr;
    }

    void push_back(const T& value) {
        emplace_back(value);
    }

    void push_back(T&& value) {
        emplace_back(std::move(value));
    }

    pointer try_push_back(const T& value) {
        return try_emplace_back(value);
    }

    pointer try_push_back(T&& value) {
        return try_emplace_back(std::move(value));
    }

    void pop_back() noexcept {
        if (m_size > 0) {
            --m_size;
            std::destroy_at(slot_ptr(m_size));
        }
    }

    void clear() noexcept {
        while (m_size > 0) {
            pop_back();
        }
    }

    [[nodiscard]] constexpr size_type size() const noexcept { return m_size; }
    [[nodiscard]] static constexpr size_type capacity() noexcept { return N; }
    [[nodiscard]] static constexpr size_type max_size() noexcept { return N; }
    [[nodiscard]] constexpr bool empty() const noexcept { return m_size == 0; }
    [[nodiscard]] constexpr bool full() const noexcept { return m_size == N; }

    [[nodiscard]] pointer data() noexcept { return slot_ptr(0); }
    [[nodiscard]] const_pointer data() const noexcept { return slot_ptr(0); }

    [[nodiscard]] reference operator[](size_type index) noexcept { return *slot_ptr(index); }
    [[nodiscard]] const_reference operator[](size_type index) const noexcept { return *slot_ptr(index); }

    [[nodiscard]] reference at(size_type index) {
        if (index >= m_size) {
            throw std::out_of_range{"boson::inplace_vector::at out of range"};
        }
        return *slot_ptr(index);
    }

    [[nodiscard]] const_reference at(size_type index) const {
        if (index >= m_size) {
            throw std::out_of_range{"boson::inplace_vector::at out of range"};
        }
        return *slot_ptr(index);
    }

    [[nodiscard]] reference front() noexcept { return *slot_ptr(0); }
    [[nodiscard]] const_reference front() const noexcept { return *slot_ptr(0); }
    [[nodiscard]] reference back() noexcept { return *slot_ptr(m_size - 1); }
    [[nodiscard]] const_reference back() const noexcept { return *slot_ptr(m_size - 1); }

    [[nodiscard]] iterator begin() noexcept { return data(); }
    [[nodiscard]] const_iterator begin() const noexcept { return data(); }
    [[nodiscard]] const_iterator cbegin() const noexcept { return data(); }
    [[nodiscard]] iterator end() noexcept { return data() + m_size; }
    [[nodiscard]] const_iterator end() const noexcept { return data() + m_size; }
    [[nodiscard]] const_iterator cend() const noexcept { return data() + m_size; }

private:
    static constexpr size_type kStorageBytes = (N == 0 ? 1 : N) * sizeof(T);

    pointer slot_ptr(size_type index) noexcept {
        return std::launder(reinterpret_cast<pointer>(m_storage + index * sizeof(T)));
    }

    const_pointer slot_ptr(size_type index) const noexcept {
        return std::launder(reinterpret_cast<const_pointer>(m_storage + index * sizeof(T)));
    }

    alignas(T) std::byte m_storage[kStorageBytes]{};
    size_type m_size{0};
};

} // namespace boson

#endif
