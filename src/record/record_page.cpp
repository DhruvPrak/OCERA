#include "record/record_page.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace ocera {
namespace {

constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kSlotCountOffset = 6;
constexpr std::size_t kFreeStartOffset = 8;
constexpr std::size_t kFreeEndOffset = 10;
constexpr std::size_t kNextPageOffset = 12;

constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kSlotSize = 12;

constexpr std::uint16_t kFormatVersion = 1;

constexpr std::uint8_t kFree = 0;
constexpr std::uint8_t kLive = 1;
constexpr std::uint8_t kRetired = 2;

constexpr char kMagic[4] = {'O', 'C', 'R', 'P'};

std::uint8_t* Bytes(Page* page) {
    return reinterpret_cast<std::uint8_t*>(page->data);
}

const std::uint8_t* Bytes(const Page* page) {
    return reinterpret_cast<const std::uint8_t*>(page->data);
}

std::uint16_t ReadU16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(p[1]) << 8U);
}

void WriteU16(std::uint8_t* p, std::uint16_t value) {
    p[0] = static_cast<std::uint8_t>(value & 0xffU);
    p[1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

std::uint32_t ReadU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) |
           (static_cast<std::uint32_t>(p[3]) << 24U);
}

void WriteU32(std::uint8_t* p, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        p[i] = static_cast<std::uint8_t>(
            (value >> (8U * i)) & 0xffU);
    }
}

std::int32_t ReadI32(const std::uint8_t* p) {
    const std::uint32_t bits = ReadU32(p);
    std::int32_t value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void WriteI32(std::uint8_t* p, std::int32_t value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    WriteU32(p, bits);
}

std::size_t SlotOffset(std::uint16_t slot) {
    return kHeaderSize + static_cast<std::size_t>(slot) * kSlotSize;
}

std::uint16_t SlotFieldU16(
    const std::uint8_t* b,
    std::uint16_t slot,
    std::size_t field) {
    return ReadU16(b + SlotOffset(slot) + field);
}

std::uint32_t SlotGeneration(
    const std::uint8_t* b,
    std::uint16_t slot) {
    return ReadU32(b + SlotOffset(slot) + 4);
}

std::uint8_t SlotStatus(
    const std::uint8_t* b,
    std::uint16_t slot) {
    return b[SlotOffset(slot) + 8];
}

void SetSlotU16(
    std::uint8_t* b,
    std::uint16_t slot,
    std::size_t field,
    std::uint16_t value) {
    WriteU16(b + SlotOffset(slot) + field, value);
}

void SetSlotGeneration(
    std::uint8_t* b,
    std::uint16_t slot,
    std::uint32_t value) {
    WriteU32(b + SlotOffset(slot) + 4, value);
}

void SetSlotStatus(
    std::uint8_t* b,
    std::uint16_t slot,
    std::uint8_t value) {
    b[SlotOffset(slot) + 8] = value;
}

// Compacts live records while preserving slot numbers and generations.
// The caller operates on a candidate page when atomicity is required.
bool CompactInPlace(Page* page) {
    auto* b = Bytes(page);
    const std::uint16_t count = ReadU16(b + kSlotCountOffset);

    struct SavedRecord {
        std::uint16_t slot;
        std::vector<std::uint8_t> bytes;
    };

    std::vector<SavedRecord> saved;
    saved.reserve(count);

    for (std::uint16_t i = 0; i < count; ++i) {
        if (SlotStatus(b, i) != kLive) {
            continue;
        }

        const auto offset = SlotFieldU16(b, i, 0);
        const auto length = SlotFieldU16(b, i, 2);

        SavedRecord item;
        item.slot = i;
        item.bytes.assign(b + offset, b + offset + length);
        saved.push_back(std::move(item));
    }

    const std::uint16_t free_start =
        ReadU16(b + kFreeStartOffset);

    std::memset(b + free_start, 0, PAGE_SIZE - free_start);

    std::uint16_t free_end =
        static_cast<std::uint16_t>(PAGE_SIZE);

    for (const auto& item : saved) {
        free_end = static_cast<std::uint16_t>(
            free_end - item.bytes.size());

        std::memcpy(
            b + free_end,
            item.bytes.data(),
            item.bytes.size());

        SetSlotU16(b, item.slot, 0, free_end);
        SetSlotU16(
            b,
            item.slot,
            2,
            static_cast<std::uint16_t>(item.bytes.size()));
    }

    WriteU16(b + kFreeEndOffset, free_end);
    return true;
}

bool FindLiveSlot(
    const Page* page,
    page_id_t page_id,
    const RID& rid,
    std::uint16_t* slot_out) {

    if (page == nullptr ||
        page_id < 2 ||
        rid.page_id != page_id ||
        !RecordPage::IsValid(page)) {
        return false;
    }

    const auto* b = Bytes(page);
    const auto count = ReadU16(b + kSlotCountOffset);

    if (rid.slot_num >= count) {
        return false;
    }

    const auto slot = static_cast<std::uint16_t>(rid.slot_num);

    if (SlotStatus(b, slot) != kLive ||
        SlotGeneration(b, slot) != rid.generation) {
        return false;
    }

    if (slot_out != nullptr) {
        *slot_out = slot;
    }

    return true;
}

}  // namespace

bool RecordPage::Init(Page* page) {
    if (page == nullptr) {
        return false;
    }

    std::memset(page->data, 0, PAGE_SIZE);

    auto* b = Bytes(page);

    std::memcpy(b + kMagicOffset, kMagic, sizeof(kMagic));

    WriteU16(b + kVersionOffset, kFormatVersion);
    WriteU16(b + kSlotCountOffset, 0);
    WriteU16(
        b + kFreeStartOffset,
        static_cast<std::uint16_t>(kHeaderSize));
    WriteU16(
        b + kFreeEndOffset,
        static_cast<std::uint16_t>(PAGE_SIZE));

    WriteI32(b + kNextPageOffset, INVALID_PAGE_ID);

    return true;
}

bool RecordPage::IsValid(const Page* page) {
    if (page == nullptr) {
        return false;
    }

    const auto* b = Bytes(page);

    if (std::memcmp(
            b + kMagicOffset,
            kMagic,
            sizeof(kMagic)) != 0 ||
        ReadU16(b + kVersionOffset) != kFormatVersion) {
        return false;
    }

    const auto count = ReadU16(b + kSlotCountOffset);

    const std::size_t expected_start =
        kHeaderSize + static_cast<std::size_t>(count) * kSlotSize;

    if (expected_start > PAGE_SIZE ||
        ReadU16(b + kFreeStartOffset) != expected_start) {
        return false;
    }

    const auto free_start = ReadU16(b + kFreeStartOffset);
    const auto free_end = ReadU16(b + kFreeEndOffset);

    if (free_start > free_end || free_end > PAGE_SIZE) {
        return false;
    }

    const auto next = ReadI32(b + kNextPageOffset);

    if (next != INVALID_PAGE_ID && next < 2) {
        return false;
    }

    std::uint16_t minimum_offset =
        static_cast<std::uint16_t>(PAGE_SIZE);

    bool has_live = false;

    for (std::uint16_t i = 0; i < count; ++i) {
        const auto status = SlotStatus(b, i);
        const auto offset = SlotFieldU16(b, i, 0);
        const auto length = SlotFieldU16(b, i, 2);
        const auto generation = SlotGeneration(b, i);

        if (status > kRetired) {
            return false;
        }

        if (status == kLive) {
            if (generation == 0 ||
                length == 0 ||
                offset < free_end ||
                static_cast<std::size_t>(offset) + length > PAGE_SIZE) {
                return false;
            }

            has_live = true;
            minimum_offset = std::min(minimum_offset, offset);

            // Ensure this record does not overlap an earlier live record.
            for (std::uint16_t j = 0; j < i; ++j) {
                if (SlotStatus(b, j) != kLive) {
                    continue;
                }

                const auto other_offset = SlotFieldU16(b, j, 0);
                const auto other_length = SlotFieldU16(b, j, 2);

                const std::size_t a0 = offset;
                const std::size_t a1 = a0 + length;
                const std::size_t b0 = other_offset;
                const std::size_t b1 = b0 + other_length;

                if (a0 < b1 && b0 < a1) {
                    return false;
                }
            }
        } else {
            if (offset != 0 || length != 0) {
                return false;
            }

            if (status == kRetired &&
                generation != std::numeric_limits<std::uint32_t>::max()) {
                return false;
            }
        }
    }

    if (has_live) {
        return minimum_offset == free_end;
    }

    return free_end == PAGE_SIZE;
}

bool RecordPage::Insert(
    Page* page,
    page_id_t page_id,
    const std::vector<std::uint8_t>& record,
    RID* rid) {

    if (page == nullptr ||
        rid == nullptr ||
        page_id < 2 ||
        record.empty() ||
        record.size() > std::numeric_limits<std::uint16_t>::max() ||
        !IsValid(page)) {
        return false;
    }

    Page candidate = *page;

    if (!CompactInPlace(&candidate)) {
        return false;
    }

    auto* b = Bytes(&candidate);
    auto count = ReadU16(b + kSlotCountOffset);

    std::uint16_t chosen = count;
    bool reused = false;

    for (std::uint16_t i = 0; i < count; ++i) {
        if (SlotStatus(b, i) == kFree) {
            chosen = i;
            reused = true;
            break;
        }
    }

    if (!reused) {
        if (count == std::numeric_limits<std::uint16_t>::max()) {
            return false;
        }

        const std::size_t new_start =
            kHeaderSize + (static_cast<std::size_t>(count) + 1) * kSlotSize;

        if (new_start > PAGE_SIZE) {
            return false;
        }
    }

    const auto free_start = ReadU16(b + kFreeStartOffset);
    const auto free_end = ReadU16(b + kFreeEndOffset);

    const std::size_t needed =
        record.size() + (reused ? 0 : kSlotSize);

    if (free_end < free_start) {
        return false;
    }

    const std::size_t available = free_end - free_start;

    if (available < needed) {
        return false;
    }

    std::uint32_t generation = 1;

    if (reused) {
        const auto old_generation = SlotGeneration(b, chosen);

        if (old_generation == std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }

        generation = old_generation == 0 ? 1 : old_generation + 1;
    } else {
        ++count;

        WriteU16(b + kSlotCountOffset, count);
        WriteU16(
            b + kFreeStartOffset,
            static_cast<std::uint16_t>(kHeaderSize + count * kSlotSize));

        const std::size_t base = SlotOffset(chosen);
        std::memset(b + base, 0, kSlotSize);
    }

    const auto new_end = static_cast<std::uint16_t>(
        free_end - record.size());

    std::memcpy(b + new_end, record.data(), record.size());

    SetSlotU16(b, chosen, 0, new_end);
    SetSlotU16(
        b,
        chosen,
        2,
        static_cast<std::uint16_t>(record.size()));

    SetSlotGeneration(b, chosen, generation);
    SetSlotStatus(b, chosen, kLive);

    WriteU16(b + kFreeEndOffset, new_end);

    if (!IsValid(&candidate)) {
        return false;
    }

    *page = candidate;
    *rid = RID{page_id, chosen, generation};

    return true;
}

bool RecordPage::Get(
    const Page* page,
    page_id_t page_id,
    const RID& rid,
    std::vector<std::uint8_t>* record) {

    if (record == nullptr) {
        return false;
    }

    std::uint16_t slot;

    if (!FindLiveSlot(page, page_id, rid, &slot)) {
        return false;
    }

    const auto* b = Bytes(page);
    const auto offset = SlotFieldU16(b, slot, 0);
    const auto length = SlotFieldU16(b, slot, 2);

    std::vector<std::uint8_t> result(
        b + offset,
        b + offset + length);

    record->swap(result);

    return true;
}

bool RecordPage::Update(
    Page* page,
    page_id_t page_id,
    const RID& rid,
    const std::vector<std::uint8_t>& record) {

    std::uint16_t slot;

    if (record.empty() ||
        record.size() > std::numeric_limits<std::uint16_t>::max() ||
        !FindLiveSlot(page, page_id, rid, &slot)) {
        return false;
    }

    Page candidate = *page;
    auto* b = Bytes(&candidate);

    // Temporarily free this slot so compaction removes its old payload.
    // Its generation is preserved because this is still the same RID.
    SetSlotU16(b, slot, 0, 0);
    SetSlotU16(b, slot, 2, 0);
    SetSlotStatus(b, slot, kFree);

    if (!CompactInPlace(&candidate)) {
        return false;
    }

    b = Bytes(&candidate);

    const auto free_start = ReadU16(b + kFreeStartOffset);
    const auto free_end = ReadU16(b + kFreeEndOffset);

    if (free_end < free_start) {
        return false;
    }

    const std::size_t available = free_end - free_start;

    if (available < record.size()) {
        return false;
    }

    const auto new_end = static_cast<std::uint16_t>(
        free_end - record.size());

    std::memcpy(b + new_end, record.data(), record.size());

    SetSlotU16(b, slot, 0, new_end);
    SetSlotU16(
        b,
        slot,
        2,
        static_cast<std::uint16_t>(record.size()));

    SetSlotStatus(b, slot, kLive);
    WriteU16(b + kFreeEndOffset, new_end);

    if (!IsValid(&candidate)) {
        return false;
    }

    *page = candidate;

    return true;
}

bool RecordPage::Delete(
    Page* page,
    page_id_t page_id,
    const RID& rid) {

    std::uint16_t slot;

    if (!FindLiveSlot(page, page_id, rid, &slot)) {
        return false;
    }

    Page candidate = *page;
    auto* b = Bytes(&candidate);

    SetSlotU16(b, slot, 0, 0);
    SetSlotU16(b, slot, 2, 0);

    if (SlotGeneration(b, slot) ==
        std::numeric_limits<std::uint32_t>::max()) {
        SetSlotStatus(b, slot, kRetired);
    } else {
        SetSlotStatus(b, slot, kFree);
    }

    if (!CompactInPlace(&candidate) || !IsValid(&candidate)) {
        return false;
    }

    *page = candidate;

    return true;
}

bool RecordPage::HasSpace(
    const Page* page,
    std::size_t record_size) {

    if (record_size == 0 ||
        record_size > std::numeric_limits<std::uint16_t>::max() ||
        !IsValid(page)) {
        return false;
    }

    const auto* b = Bytes(page);
    const auto count = ReadU16(b + kSlotCountOffset);

    bool reusable = false;

    for (std::uint16_t i = 0; i < count; ++i) {
        if (SlotStatus(b, i) == kFree) {
            reusable = true;
            break;
        }
    }

    const auto free_start = ReadU16(b + kFreeStartOffset);
    const auto free_end = ReadU16(b + kFreeEndOffset);

    const std::size_t available = free_end - free_start;

    return available >= record_size + (reusable ? 0 : kSlotSize);
}

bool RecordPage::Compact(Page* page) {
    if (!IsValid(page)) {
        return false;
    }

    Page candidate = *page;

    if (!CompactInPlace(&candidate) || !IsValid(&candidate)) {
        return false;
    }

    *page = candidate;

    return true;
}

page_id_t RecordPage::GetNextPageId(const Page* page) {
    if (!IsValid(page)) {
        return INVALID_PAGE_ID;
    }

    return ReadI32(Bytes(page) + kNextPageOffset);
}

bool RecordPage::SetNextPageId(
    Page* page,
    page_id_t next_page_id) {

    if (!IsValid(page) ||
        (next_page_id != INVALID_PAGE_ID && next_page_id < 2)) {
        return false;
    }

    Page candidate = *page;

    WriteI32(Bytes(&candidate) + kNextPageOffset, next_page_id);

    if (!IsValid(&candidate)) {
        return false;
    }

    *page = candidate;

    return true;
}
bool RecordPage::Scan(
    const Page* page,
    page_id_t page_id,
    std::vector<std::pair<RID, std::vector<std::uint8_t>>>* records
) {
    if (page == nullptr || page_id < 2 ||
        records == nullptr || !IsValid(page)) {
        return false;
    }

    try {
        const auto* b = Bytes(page);
        const auto count = ReadU16(b + kSlotCountOffset);

        std::vector<std::pair<RID, std::vector<std::uint8_t>>> result;
        result.reserve(count);

        for (std::uint16_t slot = 0; slot < count; ++slot) {
            if (SlotStatus(b, slot) != kLive) {
                continue;
            }

            const auto offset = SlotFieldU16(b, slot, 0);
            const auto length = SlotFieldU16(b, slot, 2);
            const auto generation = SlotGeneration(b, slot);

            std::vector<std::uint8_t> payload(
                b + offset,
                b + offset + length
            );

            result.emplace_back(
                RID{page_id, slot, generation},
                std::move(payload)
            );
        }

        records->swap(result);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace ocera