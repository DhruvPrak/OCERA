#include "record/table_heap.h"

#include <unordered_set>

namespace ocera {

bool TableHeap::Create(
    BufferPoolManager& bpm,
    page_id_t* first_page_id
) {
    if (first_page_id == nullptr) {
        return false;
    }

    *first_page_id = INVALID_PAGE_ID;

    page_id_t page_id = INVALID_PAGE_ID;
    Page* page = bpm.NewPage(&page_id);

    if (page == nullptr || page_id < 2) {
        return false;
    }

    if (!RecordPage::Init(page)) {
        bpm.UnpinPage(page_id, true);
        bpm.DeletePage(page_id);
        return false;
    }

    if (!bpm.UnpinPage(page_id, true)) {
        return false;
    }

    *first_page_id = page_id;
    return true;
}

TableHeap::TableHeap(
    BufferPoolManager& bpm,
    page_id_t first_page_id
)
    : bpm_(bpm),
      first_page_id_(first_page_id) {
}

Page* TableHeap::FetchPageInHeap(
    page_id_t target_page_id
) const {
    if (first_page_id_ < 2 || target_page_id < 2) {
        return nullptr;
    }

    std::unordered_set<page_id_t> visited;
    page_id_t current_id = first_page_id_;

    while (current_id != INVALID_PAGE_ID) {
        if (!visited.insert(current_id).second) {
            return nullptr;
        }

        Page* page = bpm_.FetchPage(current_id);

        if (page == nullptr) {
            return nullptr;
        }

        if (!RecordPage::IsValid(page)) {
            bpm_.UnpinPage(current_id, false);
            return nullptr;
        }

        if (current_id == target_page_id) {
            return page;
        }

        const page_id_t next_id =
            RecordPage::GetNextPageId(page);

        if (!bpm_.UnpinPage(current_id, false)) {
            return nullptr;
        }

        current_id = next_id;
    }

    return nullptr;
}

bool TableHeap::Insert(
    const std::vector<std::uint8_t>& record,
    RID* rid
) {
    if (rid == nullptr ||
        record.empty() ||
        first_page_id_ < 2) {
        return false;
    }

    *rid = RID{};

    std::unordered_set<page_id_t> visited;
    page_id_t current_id = first_page_id_;

    while (current_id != INVALID_PAGE_ID) {
        if (!visited.insert(current_id).second) {
            return false;
        }

        Page* page = bpm_.FetchPage(current_id);

        if (page == nullptr) {
            return false;
        }

        if (!RecordPage::IsValid(page)) {
            bpm_.UnpinPage(current_id, false);
            return false;
        }

        RID inserted_rid;

        if (RecordPage::Insert(
                page,
                current_id,
                record,
                &inserted_rid)) {

            if (!bpm_.UnpinPage(current_id, true)) {
                return false;
            }

            *rid = inserted_rid;
            return true;
        }

        // RecordPage::HasSpace distinguishes a full page from
        // a record that cannot fit on any page.
        if (RecordPage::HasSpace(page, record.size())) {
            // Space was reported, but insertion failed. Treat this
            // as an unexpected page-operation failure.
            bpm_.UnpinPage(current_id, false);
            return false;
        }

        const page_id_t next_id =
            RecordPage::GetNextPageId(page);

        if (next_id != INVALID_PAGE_ID) {
            if (!bpm_.UnpinPage(current_id, false)) {
                return false;
            }

            current_id = next_id;
            continue;
        }

        // We reached the last page and it cannot hold this record.
        // Release it before allocating so a one-frame buffer pool
        // does not remain pinned during allocation.
        if (!bpm_.UnpinPage(current_id, false)) {
            return false;
        }

        page_id_t new_page_id = INVALID_PAGE_ID;
        Page* new_page = bpm_.NewPage(&new_page_id);

        if (new_page == nullptr || new_page_id < 2) {
            return false;
        }

        bool new_page_ready =
            RecordPage::Init(new_page) &&
            RecordPage::Insert(
                new_page,
                new_page_id,
                record,
                &inserted_rid
            );

        if (!new_page_ready) {
            bpm_.UnpinPage(new_page_id, true);
            bpm_.DeletePage(new_page_id);
            return false;
        }

        if (!bpm_.UnpinPage(new_page_id, true)) {
            return false;
        }

        // Re-fetch the previous last page and link the new page.
        Page* previous_page = bpm_.FetchPage(current_id);

        if (previous_page == nullptr) {
            bpm_.DeletePage(new_page_id);
            return false;
        }

        const bool can_link =
            RecordPage::IsValid(previous_page) &&
            RecordPage::GetNextPageId(previous_page) ==
                INVALID_PAGE_ID;

        const bool linked =
            can_link &&
            RecordPage::SetNextPageId(
                previous_page,
                new_page_id
            );

        const bool unpinned =
            bpm_.UnpinPage(current_id, linked);

        if (!linked || !unpinned) {
            // The new page is not reachable from the heap if linking
            // failed. It is already unpinned and can be deallocated.
            if (!linked) {
                bpm_.DeletePage(new_page_id);
            }
            return false;
        }

        *rid = inserted_rid;
        return true;
    }

    return false;
}

bool TableHeap::Get(
    const RID& rid,
    std::vector<std::uint8_t>* record
) {
    if (record == nullptr) {
        return false;
    }

    Page* page = FetchPageInHeap(rid.page_id);

    if (page == nullptr) {
        return false;
    }

    const bool success =
        RecordPage::Get(page, rid.page_id, rid, record);

    const bool unpinned =
        bpm_.UnpinPage(rid.page_id, false);

    return success && unpinned;
}

bool TableHeap::Update(
    const RID& rid,
    const std::vector<std::uint8_t>& record
) {
    Page* page = FetchPageInHeap(rid.page_id);

    if (page == nullptr) {
        return false;
    }

    const bool success =
        RecordPage::Update(page, rid.page_id, rid, record);

    const bool unpinned =
        bpm_.UnpinPage(rid.page_id, success);

    return success && unpinned;
}

bool TableHeap::Delete(const RID& rid) {
    Page* page = FetchPageInHeap(rid.page_id);

    if (page == nullptr) {
        return false;
    }

    const bool success =
        RecordPage::Delete(page, rid.page_id, rid);

    const bool unpinned =
        bpm_.UnpinPage(rid.page_id, success);

    return success && unpinned;
}

}  // namespace ocera