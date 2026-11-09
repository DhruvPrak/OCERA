#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/rid.h"

namespace ocera {

// In-memory B+Tree index.
//
// Internal nodes route searches; leaves store sorted keys and their associated
// RIDs. Leaf nodes are linked for ordered range scans. A key may map to multiple
// RIDs, stored in insertion order. This implementation uses ordinary C++
// objects: it is not persisted to database pages and is not thread-safe.
class BPlusTree {
public:
    using Key = std::int64_t;

    explicit BPlusTree(int max_keys = 4);
    ~BPlusTree();

    BPlusTree(const BPlusTree&) = delete;
    BPlusTree& operator=(const BPlusTree&) = delete;

    // Returns false only when the exact (key, RID) pair already exists.
    bool Insert(Key key, const RID& rid);

    // Returns the first RID for key, preserving insertion order.
    bool Search(Key key, RID* rid) const;
    std::vector<RID> SearchAll(Key key) const;

    // Removes exactly one (key, RID) pair. Missing pairs return false.
    bool Remove(Key key, const RID& rid);

    // Inclusive bounds; results are ordered by key, then RID insertion order.
    std::vector<std::pair<Key, RID>> RangeScan(Key low, Key high) const;

    // Number of key/RID pairs, not number of distinct keys.
    std::size_t Size() const { return size_; }
    bool Empty() const { return size_ == 0; }

    int Height() const;
    bool CheckInvariants(std::string* error = nullptr) const;

private:
    struct Node {
        explicit Node(bool leaf) : is_leaf(leaf) {}
        bool is_leaf;
        std::vector<Key> keys;
        std::vector<std::vector<RID>> values;        // Leaf: one RID list per key.
        std::vector<std::unique_ptr<Node>> children; // Internal: keys.size() + 1.
        Node* next = nullptr;                        // Leaf sibling link; non-owning.
    };

    struct SplitResult {
        Key separator;
        std::unique_ptr<Node> right;
    };

    int max_keys_;
    std::unique_ptr<Node> root_;
    std::size_t size_ = 0;

    const Node* FindLeaf(Key key) const;
    std::optional<SplitResult> InsertInto(Node* node, Key key, const RID& rid,
                                          bool* inserted);
    bool RemoveFrom(Node* node, Key key, const RID& rid);
    void RebalanceChild(Node* parent, std::size_t child_index);
    void RefreshSeparators(Node* node);
    bool CheckNode(const Node* node, const Key* lo, const Key* hi, int depth,
                   int* leaf_depth, std::vector<const Node*>* leaves,
                   std::size_t* pair_count, std::string* err) const;
};

}  // namespace ocera
