#include "index/bplus_tree.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace ocera {

BPlusTree::BPlusTree(int max_keys) : max_keys_(max_keys) {
    if (max_keys < 3) throw std::invalid_argument("BPlusTree: max_keys must be >= 3");
    root_ = std::make_unique<Node>(true);
}

BPlusTree::~BPlusTree() = default;

const BPlusTree::Node* BPlusTree::FindLeaf(Key key) const {
    const Node* node = root_.get();
    while (!node->is_leaf) {
        const auto it = std::upper_bound(node->keys.begin(), node->keys.end(), key);
        node = node->children[static_cast<std::size_t>(it - node->keys.begin())].get();
    }
    return node;
}

bool BPlusTree::Search(Key key, RID* rid) const {
    const Node* leaf = FindLeaf(key);
    const auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
    if (it == leaf->keys.end() || *it != key) return false;
    if (rid) *rid = leaf->values[static_cast<std::size_t>(it - leaf->keys.begin())].front();
    return true;
}

std::vector<RID> BPlusTree::SearchAll(Key key) const {
    const Node* leaf = FindLeaf(key);
    const auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
    if (it == leaf->keys.end() || *it != key) return {};
    return leaf->values[static_cast<std::size_t>(it - leaf->keys.begin())];
}

std::vector<std::pair<BPlusTree::Key, RID>> BPlusTree::RangeScan(Key low, Key high) const {
    std::vector<std::pair<Key, RID>> out;
    if (low > high) return out;
    const Node* leaf = FindLeaf(low);
    while (leaf) {
        std::size_t i = static_cast<std::size_t>(std::lower_bound(leaf->keys.begin(), leaf->keys.end(), low) - leaf->keys.begin());
        for (; i < leaf->keys.size(); ++i) {
            if (leaf->keys[i] > high) return out;
            for (const RID& rid : leaf->values[i]) out.emplace_back(leaf->keys[i], rid);
        }
        leaf = leaf->next;
        low = std::numeric_limits<Key>::min();
    }
    return out;
}

bool BPlusTree::Insert(Key key, const RID& rid) {
    bool inserted = false;
    auto split = InsertInto(root_.get(), key, rid, &inserted);
    if (!inserted) return false;
    if (split) {
        auto new_root = std::make_unique<Node>(false);
        new_root->children.push_back(std::move(root_));
        new_root->children.push_back(std::move(split->right));
        new_root->keys.push_back(split->separator);
        root_ = std::move(new_root);
    }
    ++size_;
    return true;
}

std::optional<BPlusTree::SplitResult> BPlusTree::InsertInto(Node* node, Key key,
                                                             const RID& rid, bool* inserted) {
    if (node->is_leaf) {
        auto it = std::lower_bound(node->keys.begin(), node->keys.end(), key);
        if (it != node->keys.end() && *it == key) {
            auto& rids = node->values[static_cast<std::size_t>(it - node->keys.begin())];
            if (std::find(rids.begin(), rids.end(), rid) != rids.end()) {
                *inserted = false;
                return std::nullopt;
            }
            rids.push_back(rid);
            *inserted = true;
            return std::nullopt;
        }
        const std::size_t pos = static_cast<std::size_t>(it - node->keys.begin());
        node->keys.insert(it, key);
        node->values.insert(node->values.begin() + static_cast<std::ptrdiff_t>(pos), std::vector<RID>{rid});
        *inserted = true;
        if (static_cast<int>(node->keys.size()) <= max_keys_) return std::nullopt;

        // Split the overflowing leaf while keeping the leaf chain in key order.
        const std::size_t mid = node->keys.size() / 2;
        auto right = std::make_unique<Node>(true);
        right->keys.assign(node->keys.begin() + static_cast<std::ptrdiff_t>(mid), node->keys.end());
        right->values.assign(node->values.begin() + static_cast<std::ptrdiff_t>(mid), node->values.end());
        node->keys.resize(mid);
        node->values.resize(mid);
        right->next = node->next;
        node->next = right.get();
        return SplitResult{right->keys.front(), std::move(right)};
    }

    const std::size_t idx = static_cast<std::size_t>(std::upper_bound(node->keys.begin(), node->keys.end(), key) - node->keys.begin());
    auto child_split = InsertInto(node->children[idx].get(), key, rid, inserted);
    if (!child_split) return std::nullopt;
    node->keys.insert(node->keys.begin() + static_cast<std::ptrdiff_t>(idx), child_split->separator);
    node->children.insert(node->children.begin() + static_cast<std::ptrdiff_t>(idx + 1), std::move(child_split->right));
    if (static_cast<int>(node->keys.size()) <= max_keys_) return std::nullopt;

    // Promote the middle separator; its right-side children move to the new node.
    const std::size_t mid = node->keys.size() / 2;
    const Key separator = node->keys[mid];
    auto right = std::make_unique<Node>(false);
    right->children.reserve(node->children.size() - mid - 1);
    for (std::size_t i = mid + 1; i < node->children.size(); ++i) right->children.push_back(std::move(node->children[i]));
    node->keys.resize(mid);
    node->children.resize(mid + 1);
    RefreshSeparators(node);
    RefreshSeparators(right.get());
    return SplitResult{separator, std::move(right)};
}

bool BPlusTree::Remove(Key key, const RID& rid) {
    if (!RemoveFrom(root_.get(), key, rid)) return false;
    --size_;
    if (!root_->is_leaf && root_->children.size() == 1) root_ = std::move(root_->children.front());
    RefreshSeparators(root_.get());
    return true;
}

bool BPlusTree::RemoveFrom(Node* node, Key key, const RID& rid) {
    if (node->is_leaf) {
        auto it = std::lower_bound(node->keys.begin(), node->keys.end(), key);
        if (it == node->keys.end() || *it != key) return false;
        const std::size_t pos = static_cast<std::size_t>(it - node->keys.begin());
        auto& rids = node->values[pos];
        auto rit = std::find(rids.begin(), rids.end(), rid);
        if (rit == rids.end()) return false;
        rids.erase(rit);
        if (!rids.empty()) return true;
        node->keys.erase(node->keys.begin() + static_cast<std::ptrdiff_t>(pos));
        node->values.erase(node->values.begin() + static_cast<std::ptrdiff_t>(pos));
        return true;
    }
    const std::size_t idx = static_cast<std::size_t>(std::upper_bound(node->keys.begin(), node->keys.end(), key) - node->keys.begin());
    if (!RemoveFrom(node->children[idx].get(), key, rid)) return false;
    // A child may have become under-filled after deleting its final key/RID.
    RebalanceChild(node, idx);
    RefreshSeparators(node);
    return true;
}

// Restore minimum occupancy by borrowing from a sibling or merging nodes.
// Leaf merges must also repair the forward sibling link used by RangeScan().
void BPlusTree::RebalanceChild(Node* parent, std::size_t idx) {
    Node* child = parent->children[idx].get();
    const int min_leaf_keys = (max_keys_ + 1) / 2;
    const int min_internal_keys = max_keys_ - (max_keys_ + 1) / 2;
    const int child_count = static_cast<int>(child->is_leaf ? child->keys.size() : child->keys.size());
    const int minimum = child->is_leaf ? min_leaf_keys : min_internal_keys;
    if (child_count >= minimum) return;

    if (idx > 0) {
        Node* left = parent->children[idx - 1].get();
        const int left_count = static_cast<int>(left->keys.size());
        if (left_count > (left->is_leaf ? min_leaf_keys : min_internal_keys)) {
            if (child->is_leaf) {
                child->keys.insert(child->keys.begin(), left->keys.back());
                child->values.insert(child->values.begin(), std::move(left->values.back()));
                left->keys.pop_back(); left->values.pop_back();
            } else {
                child->children.insert(child->children.begin(), std::move(left->children.back()));
                left->children.pop_back();
                RefreshSeparators(left); RefreshSeparators(child);
            }
            RefreshSeparators(parent);
            return;
        }
    }
    if (idx + 1 < parent->children.size()) {
        Node* right = parent->children[idx + 1].get();
        const int right_count = static_cast<int>(right->keys.size());
        if (right_count > (right->is_leaf ? min_leaf_keys : min_internal_keys)) {
            if (child->is_leaf) {
                child->keys.push_back(right->keys.front());
                child->values.push_back(std::move(right->values.front()));
                right->keys.erase(right->keys.begin()); right->values.erase(right->values.begin());
            } else {
                child->children.push_back(std::move(right->children.front()));
                right->children.erase(right->children.begin());
                RefreshSeparators(right); RefreshSeparators(child);
            }
            RefreshSeparators(parent);
            return;
        }
    }

    if (idx > 0) {
        Node* left = parent->children[idx - 1].get();
        if (child->is_leaf) {
            left->keys.insert(left->keys.end(), child->keys.begin(), child->keys.end());
            left->values.insert(left->values.end(), std::make_move_iterator(child->values.begin()), std::make_move_iterator(child->values.end()));
            left->next = child->next;
        } else {
            for (auto& grandchild : child->children) left->children.push_back(std::move(grandchild));
            RefreshSeparators(left);
        }
        parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(idx));
    } else if (idx + 1 < parent->children.size()) {
        Node* right = parent->children[idx + 1].get();
        if (child->is_leaf) {
            child->keys.insert(child->keys.end(), right->keys.begin(), right->keys.end());
            child->values.insert(child->values.end(), std::make_move_iterator(right->values.begin()), std::make_move_iterator(right->values.end()));
            child->next = right->next;
        } else {
            for (auto& grandchild : right->children) child->children.push_back(std::move(grandchild));
            RefreshSeparators(child);
        }
        parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(idx + 1));
    }
    RefreshSeparators(parent);
}

// Internal separators are derived from the first key in each right subtree.
// Recomputing them after structural changes keeps routing keys consistent.
void BPlusTree::RefreshSeparators(Node* node) {
    if (!node || node->is_leaf) return;
    for (auto& child : node->children) RefreshSeparators(child.get());
    node->keys.clear();
    for (std::size_t i = 1; i < node->children.size(); ++i) {
        Node* first = node->children[i].get();
        while (!first->is_leaf) first = first->children.front().get();
        if (!first->keys.empty()) node->keys.push_back(first->keys.front());
    }
}

int BPlusTree::Height() const {
    int h = 1;
    const Node* node = root_.get();
    while (!node->is_leaf) { node = node->children.front().get(); ++h; }
    return h;
}

bool BPlusTree::CheckInvariants(std::string* error) const {
    int leaf_depth = -1;
    std::vector<const Node*> leaves;
    std::size_t pair_count = 0;
    std::string err;
    if (!CheckNode(root_.get(), nullptr, nullptr, 1, &leaf_depth, &leaves, &pair_count, &err)) {
        if (error) *error = err;
        return false;
    }
    if (pair_count != size_) { if (error) *error = "size_ does not match number of key/RID pairs"; return false; }
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        const Node* expected = i + 1 < leaves.size() ? leaves[i + 1] : nullptr;
        if (leaves[i]->next != expected) { if (error) *error = "leaf chain (next pointers) is broken"; return false; }
    }
    return true;
}

bool BPlusTree::CheckNode(const Node* node, const Key* lo, const Key* hi, int depth,
                          int* leaf_depth, std::vector<const Node*>* leaves,
                          std::size_t* pair_count, std::string* err) const {
    auto fail = [&](const std::string& msg) { *err = msg; return false; };
    const bool is_root = node == root_.get();
    for (std::size_t i = 1; i < node->keys.size(); ++i)
        if (node->keys[i - 1] >= node->keys[i]) return fail("keys not strictly increasing");
    if (!node->keys.empty()) {
        if (lo && node->keys.front() < *lo) return fail("key below subtree lower bound");
        if (hi && node->keys.back() >= *hi) return fail("key at/above subtree upper bound");
    }
    if (static_cast<int>(node->keys.size()) > max_keys_) return fail("node exceeds max_keys");
    if (node->is_leaf) {
        if (node->values.size() != node->keys.size()) return fail("leaf keys/values size mismatch");
        if (!is_root && static_cast<int>(node->keys.size()) < (max_keys_ + 1) / 2) return fail("non-root leaf under-filled");
        for (const auto& group : node->values) {
            if (group.empty()) return fail("leaf contains key with no RIDs");
            *pair_count += group.size();
        }
        if (*leaf_depth == -1) *leaf_depth = depth;
        else if (*leaf_depth != depth) return fail("leaves are not at the same depth");
        leaves->push_back(node);
        return true;
    }
    if (node->keys.empty()) return fail("internal node has no keys");
    if (node->children.size() != node->keys.size() + 1) return fail("internal children != keys + 1");
    if (!is_root && static_cast<int>(node->keys.size()) < max_keys_ - (max_keys_ + 1) / 2) return fail("non-root internal node under-filled");
    for (std::size_t i = 0; i < node->children.size(); ++i) {
        const Key* child_lo = i == 0 ? lo : &node->keys[i - 1];
        const Key* child_hi = i == node->keys.size() ? hi : &node->keys[i];
        if (!CheckNode(node->children[i].get(), child_lo, child_hi, depth + 1, leaf_depth, leaves, pair_count, err)) return false;
        if (i > 0) {
            const Node* first = node->children[i].get();
            while (!first->is_leaf) first = first->children.front().get();
            if (first->keys.empty() || node->keys[i - 1] != first->keys.front()) return fail("separator does not match first key in right child");
        }
    }
    return true;
}

}  // namespace ocera
