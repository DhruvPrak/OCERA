#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/rid.h"

namespace ocera {

// A B+Tree index: a sorted lookup structure that finds a key in a handful of
// steps instead of scanning every row. Internal nodes only guide the search;
// all (key, RID) pairs live in the leaves, and the leaves are chained left to
// right so a range of keys can be read in order.
//
// ---------------------------------------------------------------------------
// STATUS: IN-MEMORY VERSION (temporary).
// Nodes here are ordinary C++ objects, NOT database pages. Nothing is saved
// to the .db file, there is no Buffer Pool use, and no locking. This lets the
// B+Tree *algorithm* (search, insert, split, root change, range scan) be
// built and tested now. When the Buffer Pool API and node page layout are
// agreed, the node storage is what gets replaced; the algorithm stays.
// ---------------------------------------------------------------------------
//
// Behaviour (proposed, not yet team-approved):
//  - Keys are int64_t (matches INT values in the SQL AST).
//  - Values are RIDs.
//  - DUPLICATE KEYS ARE NOT ALLOWED: Insert of an existing key returns false
//    and changes nothing.
//  - Supported: Insert, Search, RangeScan. NOT implemented: Remove, persistence.
//  - Not thread-safe (locking will be done above this layer, page-level).
class BPlusTree {
public:
    using Key = std::int64_t;

    // max_keys = the most keys a single node may hold before it splits.
    // Must be >= 3. Small values (3, 4) are useful in tests because they make
    // the tree split often; real page-sized nodes will hold hundreds of keys.
    explicit BPlusTree(int max_keys = 4);
    ~BPlusTree();

    BPlusTree(const BPlusTree&) = delete;
    BPlusTree& operator=(const BPlusTree&) = delete;

    // Adds (key, rid). Returns false (and leaves the tree unchanged) if the
    // key is already present.
    bool Insert(Key key, const RID& rid);

    // Looks up `key`. Returns true and fills *rid (if non-null) when found.
    bool Search(Key key, RID* rid) const;

    // All entries with low <= key <= high, in ascending key order.
    // Returns an empty vector if low > high or nothing matches.
    std::vector<std::pair<Key, RID>> RangeScan(Key low, Key high) const;

    std::size_t Size() const { return size_; }
    bool Empty() const { return size_ == 0; }

    // Number of levels (an empty tree or a single leaf has height 1).
    int Height() const;

    // Self-check used by tests: verifies sorted keys, key ranges per child,
    // equal leaf depth, node fill limits, and the leaf chain. Returns true if
    // the tree is well-formed; otherwise false and describes the problem in
    // *error (if non-null).
    bool CheckInvariants(std::string* error = nullptr) const;

private:
    struct Node {
        explicit Node(bool leaf) : is_leaf(leaf) {}
        bool is_leaf;
        std::vector<Key> keys;
        std::vector<RID> values;                      // leaves only
        std::vector<std::unique_ptr<Node>> children;  // internal only
        Node* next = nullptr;                         // leaves only: right sibling
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
    bool CheckNode(const Node* node, const Key* lo, const Key* hi, int depth,
                   int* leaf_depth, std::vector<const Node*>* leaves,
                   std::size_t* key_count, std::string* err) const;
};

}  // namespace ocera
