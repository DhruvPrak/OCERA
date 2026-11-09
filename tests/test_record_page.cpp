#include "record/record_page.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

using namespace ocera;

static std::vector<std::uint8_t> Payload(
    std::size_t size,
    std::uint8_t seed) {

    return std::vector<std::uint8_t>(size, seed);
}

int main() {
    Page page;

    // Initialization and page links.
    assert(RecordPage::Init(&page));
    assert(RecordPage::IsValid(&page));

    assert(RecordPage::GetNextPageId(&page) == INVALID_PAGE_ID);
    assert(!RecordPage::SetNextPageId(&page, 1));

    assert(RecordPage::SetNextPageId(&page, 7));
    assert(RecordPage::GetNextPageId(&page) == 7);

    assert(RecordPage::SetNextPageId(&page, INVALID_PAGE_ID));

    // Insert records.
    RID first;
    RID second;

    const auto a = Payload(100, 0x11);
    const auto b = Payload(120, 0x22);

    assert(RecordPage::Insert(&page, 2, a, &first));
    assert(RecordPage::Insert(&page, 2, b, &second));

    assert(first.page_id == 2);
    assert(first.slot_num == 0);
    assert(first.generation == 1);

    assert(second.slot_num == 1);
    assert(second.generation == 1);

    assert(RecordPage::IsValid(&page));

    // Read and verify.
    std::vector<std::uint8_t> out;

    assert(RecordPage::Get(&page, 2, first, &out));
    assert(out == a);

    assert(!RecordPage::Get(&page, 3, first, &out));

    // Failed oversized update must preserve the original record.
    const Page before_failed_update = page;

    assert(!RecordPage::Update(
        &page, 2, first, Payload(4000, 0x33)));

    assert(std::memcmp(
        page.data,
        before_failed_update.data,
        PAGE_SIZE) == 0);

    assert(RecordPage::Get(&page, 2, first, &out));
    assert(out == a);

    // Successful update preserves the RID generation.
    const auto smaller = Payload(64, 0x44);

    assert(RecordPage::Update(&page, 2, first, smaller));
    assert(RecordPage::Get(&page, 2, first, &out));
    assert(out == smaller);
    assert(first.generation == 1);

    // Delete and reuse a slot.
    assert(RecordPage::Delete(&page, 2, second));
    assert(!RecordPage::Get(&page, 2, second, &out));

    RID reused;

    assert(RecordPage::Insert(
        &page, 2, Payload(80, 0x55), &reused));

    assert(reused.slot_num == second.slot_num);
    assert(reused.generation == second.generation + 1);

    // Old RID must not access the reused slot.
    assert(!RecordPage::Delete(&page, 2, second));

    // Compaction must preserve surviving records.
    assert(RecordPage::Compact(&page));
    assert(RecordPage::IsValid(&page));

    assert(RecordPage::Get(&page, 2, first, &out));
    assert(out == smaller);

    assert(RecordPage::Get(&page, 2, reused, &out));
    assert(out == Payload(80, 0x55));

    assert(!RecordPage::HasSpace(&page, PAGE_SIZE));

    // Invalid magic must be rejected.
    Page corrupted = page;
    corrupted.data[0] = 'X';

    assert(!RecordPage::IsValid(&corrupted));

    std::cout << "All RecordPage tests passed.\n";

    return 0;
}