#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <utility>

#include "common/rid.h"
#include "record/record_page.h"
#include "storage/buffer_pool_manager.h"
#include "txn/txn_types.h"
#include "txn/transaction_manager.h"

namespace ocera {

class TableHeap {
public:
    // Creates the first RecordPage and returns its page ID.
    // The caller must retain this ID to reopen the heap later.
    static bool Create(
        BufferPoolManager& bpm,
        page_id_t* first_page_id,
        txn_id_t txn_id = 0
    );
    bool Scan(
        std::vector<std::pair<RID, std::vector<std::uint8_t>>>* records,
        txn_id_t txn_id = 0
    );

    // Opens an existing heap using its first RecordPage ID.
    TableHeap(
        BufferPoolManager& bpm,
        page_id_t first_page_id,
        TransactionManager* transaction_manager = nullptr
    );

    bool Insert(
        const std::vector<std::uint8_t>& record,
        RID* rid,
        txn_id_t txn_id = 0
    );

    bool Get(
        const RID& rid,
        std::vector<std::uint8_t>* record,
        txn_id_t txn_id = 0
    );

    bool Update(
        const RID& rid,
        const std::vector<std::uint8_t>& record,
        txn_id_t txn_id = 0
    );

    bool Delete(const RID& rid, txn_id_t txn_id = 0);

    page_id_t GetFirstPageId() const {
        return first_page_id_;
    }

private:
    BufferPoolManager& bpm_;
    page_id_t first_page_id_;
    TransactionManager* transaction_manager_;

    // Returns the requested page pinned if it belongs to this heap.
    // Intermediate pages receive shared locks; the target receives target_mode.
    // The caller is responsible for unpinning the returned page.
    Page* FetchPageInHeap(page_id_t target_page_id, txn_id_t txn_id,
                          LockMode target_mode) const;
    bool AcquirePageLock(page_id_t page_id, txn_id_t txn_id,
                         LockMode mode) const;
};

}  // namespace ocera