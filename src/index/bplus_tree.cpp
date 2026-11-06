#include "index/bplus_tree.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ocera {

BPlusTree::BPlusTree(int max_keys) : max_keys_(max_keys) {
    if (max_keys < 3) {
        throw std::invalid_argument("BPlusTree: max_keys must be >= 3");
    }
    root_ = std::make_unique<Node>(true);  // empty tree = one empty leaf
}

BPlusTree::~BPlusTree() = default;

// Walks from the root down to the leaf that should contain `key`.
// In an internal node, keys[i] separates children: child i holds keys < keys[i],
// child i+1 holds keys >= keys[i]. upper_bound gives exactly that child index.
const BPlusTree::Node* BPlusTree::FindLeaf(Key key) const {
    const Node* node = root_.get();
    while (!node->is_leaf) {
        std::size_t idx =
            std::upper_bound(node->keys.begin(), node->keys.end(), key) -
            node->keys.begin();
        node = node->children[idx].get();
    }
    return node;
}

bool BPlusTree::Search(Key key, RID* rid) const {
    const Node* leaf = FindLeaf(key);
    auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
    if (it == leaf->keys.end() || *it != key) return false;
    if (rid) *rid = leaf->values[it - leaf->keys.begin()];
    return true;
}

std::vector<std::pair<BPlusTree::Key, RID>> BPlusTree::RangeScan(Key low,
                                                                 Key high) const {
    std::vector<std::pair<Key, RID>> out;
    if (low > high) return out;
    const Node* leaf = FindLeaf(low);
    while (leaf != nullptr) {
        std::size_t i =
            std::lower_bound(leaf->keys.begin(), leaf->keys.end(), low) -
            leaf->keys.begin();
        for (; i < leaf->keys.size(); ++i) {
            if (leaf->keys[i] > high) return out;
            out.emplace_back(leaf->keys[i], leaf->values[i]);
        }
        leaf = leaf->next;
        low = std::numeric_limits<Key>::min();  // later leaves: take from start
    }
    return out;
}

bool BPlusTree::Insert(Key key, const RID& rid) {
    bool inserted = false;
    std::optional<SplitResult> split = InsertInto(root_.get(), key, rid, &inserted);
    if (!inserted) return false;

    if (split) {
        // The root itself split: grow the tree one level taller.
        auto new_root = std::make_unique<Node>(false);
        new_root->keys.push_back(split->separator);
        new_root->children.push_back(std::move(root_));
        new_root->children.push_back(std::move(split->right));
        root_ = std::move(new_root);
    }
    ++size_;
    return true;
}

// Inserts into the subtree at `node`. If `node` overflows it is split in two
// and the caller receives (separator key, new right node) to add to the parent.
std::optional<BPlusTree::SplitResult> BPlusTree::InsertInto(Node* node, Key key,
                                                            const RID& rid,
                                                            bool* inserted) {
    if (node->is_leaf) {
        auto it = std::lower_bound(node->keys.begin(), node->keys.end(), key);
        if (it != node->keys.end() && *it == key) {
            *inserted = false;  // duplicate: reject
            return std::nullopt;
        }
        std::size_t pos = it - node->keys.begin();
        node->keys.insert(node->keys.begin() + pos, key);
        node->values.insert(node->values.begin() + pos, rid);
        *inserted = true;

        if (static_cast<int>(node->keys.size()) <= max_keys_) return std::nullopt;

        // Leaf split: right half moves to a new leaf; the first key of the new
        // leaf is COPIED up as the separator (it also stays in the leaf).
        std::size_t mid = node->keys.size() / 2;
        auto right = std::make_unique<Node>(true);
        right->keys.assign(node->keys.begin() + mid, node->keys.end());
        right->values.assign(node->values.begin() + mid, node->values.end());
        node->keys.resize(mid);
        node->values.resize(mid);
        right->next = node->next;
        node->next = right.get();
        Key sep = right->keys.front();
        return SplitResult{sep, std::move(right)};
    }

    // Internal node: descend into the right child first.
    std::size_t idx = std::upper_bound(node->keys.begin(), node->keys.end(), key) -
                      node->keys.begin();
    std::optional<SplitResult> child_split =
        InsertInto(node->children[idx].get(), key, rid, inserted);
    if (!child_split) return std::nullopt;

    // The child split: add its separator and new right sibling here.
    node->keys.insert(node->keys.begin() + idx, child_split->separator);
    node->children.insert(node->children.begin() + idx + 1,
                          std::move(child_split->right));

    if (static_cast<int>(node->keys.size()) <= max_keys_) return std::nullopt;

    // Internal split: the middle key MOVES up (it does not stay in either half).
    std::size_t mid = node->keys.size() / 2;
    Key sep = node->keys[mid];
    auto right = std::make_unique<Node>(false);
    right->keys.assign(node->keys.begin() + mid + 1, node->keys.end());
    for (std::size_t i = mid + 1; i < node->children.size(); ++i) {
        right->children.push_back(std::move(node->children[i]));
    }
    node->keys.resize(mid);
    node->children.resize(mid + 1);
    return SplitResult{sep, std::move(right)};
}

int BPlusTree::Height() const {
    int h = 1;
    const Node* node = root_.get();
    while (!node->is_leaf) {
        node = node->children.front().get();
        ++h;
    }
    return h;
}

bool BPlusTree::CheckInvariants(std::string* error) const {
    int leaf_depth = -1;
    std::vector<const Node*> leaves;
    std::size_t key_count = 0;
    std::string err;
    if (!CheckNode(root_.get(), nullptr, nullptr, 1, &leaf_depth, &leaves,
                   &key_count, &err)) {
        if (error) *error = err;
        return false;
    }
    if (key_count != size_) {
        if (error) *error = "size_ does not match number of keys in leaves";
        return false;
    }
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        const Node* expected = (i + 1 < leaves.size()) ? leaves[i + 1] : nullptr;
        if (leaves[i]->next != expected) {
            if (error) *error = "leaf chain (next pointers) is broken";
            return false;
        }
    }
    return true;
}

// `lo` (inclusive) and `hi` (exclusive) bound every key allowed in this subtree.
bool BPlusTree::CheckNode(const Node* node, const Key* lo, const Key* hi,
                          int depth, int* leaf_depth,
                          std::vector<const Node*>* leaves,
                          std::size_t* key_count, std::string* err) const {
    auto fail = [&](const std::string& msg) {
        *err = msg;
        return false;
    };
    const bool is_root = (node == root_.get());

    for (std::size_t i = 1; i < node->keys.size(); ++i) {
        if (node->keys[i - 1] >= node->keys[i]) return fail("keys not strictly increasing");
    }
    if (!node->keys.empty()) {
        if (lo && node->keys.front() < *lo) return fail("key below its subtree's lower bound");
        if (hi && node->keys.back() >= *hi) return fail("key at/above its subtree's upper bound");
    }
    if (static_cast<int>(node->keys.size()) > max_keys_) return fail("node holds more than max_keys");

    if (node->is_leaf) {
        if (node->values.size() != node->keys.size()) return fail("leaf keys/values size mismatch");
        if (!is_root && static_cast<int>(node->keys.size()) < (max_keys_ + 1) / 2) {
            return fail("non-root leaf is under-filled");
        }
        if (*leaf_depth == -1) {
            *leaf_depth = depth;
        } else if (*leaf_depth != depth) {
            return fail("leaves are not all at the same depth");
        }
        leaves->push_back(node);
        *key_count += node->keys.size();
        return true;
    }

    if (node->keys.empty()) return fail("internal node has no keys");
    if (node->children.size() != node->keys.size() + 1) return fail("internal node: children != keys + 1");
    if (!is_root && static_cast<int>(node->keys.size()) < max_keys_ - (max_keys_ + 1) / 2) {
        return fail("non-root internal node is under-filled");
    }
    for (std::size_t i = 0; i < node->children.size(); ++i) {
        const Key* child_lo = (i == 0) ? lo : &node->keys[i - 1];
        const Key* child_hi = (i == node->keys.size()) ? hi : &node->keys[i];
        if (!CheckNode(node->children[i].get(), child_lo, child_hi, depth + 1,
                       leaf_depth, leaves, key_count, err)) {
            return false;
        }
    }
    return true;
}

}  // namespace ocera
