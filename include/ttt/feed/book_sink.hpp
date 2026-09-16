// One symbol's order book, driven by ITCH messages: the receiver's book, the
// snapshot server's book and the chaos oracle are all this.
//
// It wraps Project 1's BookBuilder over its FlatBook. A reset constructs both
// again in place rather than clearing them, because the book's pools and index
// are prefaulted at construction and have no clear operation. A reset only
// happens when a snapshot is applied, which is rare, so paying for that there is
// the right place.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>

#include "itch/book/flat_book.hpp"
#include "itch/core/assert.hpp"
#include "itch/core/types.hpp"
#include "itch/replay/book_builder.hpp"
#include "itch/wire/messages.hpp"
#include "itch/wire/views.hpp"

namespace ttt::feed {

struct BookCapacity {
    itch::u32 orders = itch::book::FlatBook::kDefaultOrders;
    itch::u32 levels = itch::book::FlatBook::kDefaultLevels;
    itch::u32 index = itch::book::FlatBook::kDefaultIndex;
};

class BookSink {
public:
    using Book = itch::book::FlatBook;
    using Builder = itch::replay::BookBuilder<Book>;

    BookSink(std::string symbol, BookCapacity cap) : symbol_(std::move(symbol)), cap_(cap) {
        reset();
    }

    BookSink(const BookSink&) = delete;
    BookSink& operator=(const BookSink&) = delete;

    void reset() {
        builder_.reset();
        book_.reset();
        book_.emplace(cap_.orders, cap_.levels, cap_.index);
        builder_.emplace(*book_, symbol_);
    }

    // Applies one ITCH message. A message whose length disagrees with its type
    // is refused rather than dispatched, since the views read at fixed offsets.
    bool apply(std::span<const std::byte> msg) {
        if (msg.empty() || itch::wire::message_length(static_cast<char>(msg[0])) != msg.size()) {
            ++malformed_;
            return false;
        }
        itch::wire::dispatch(msg.data(), *builder_);
        return true;
    }

    [[nodiscard]] const Book&        book() const noexcept { return *book_; }
    [[nodiscard]] const Builder&     builder() const noexcept { return *builder_; }
    [[nodiscard]] const std::string& symbol() const noexcept { return symbol_; }
    [[nodiscard]] itch::u64          malformed() const noexcept { return malformed_; }

private:
    std::string            symbol_;
    BookCapacity           cap_;
    std::optional<Book>    book_;
    std::optional<Builder> builder_;
    itch::u64              malformed_ = 0;
};

}  // namespace ttt::feed
