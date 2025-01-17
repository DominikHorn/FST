#ifndef FSTBUILDER_H_
#define FSTBUILDER_H_

#include <cassert>
#include <string>
#include <vector>

#include "config.hpp"
#include "hash.hpp"

namespace mmphf_fst {

class FSTBuilder {
 public:
  FSTBuilder() : sparse_start_level_(0){};
  explicit FSTBuilder(bool include_dense, uint32_t sparse_dense_ratio)
      : include_dense_(include_dense),
        sparse_dense_ratio_(sparse_dense_ratio),
        sparse_start_level_(0){};

  ~FSTBuilder() = default;

  // Fills in the LOUDS-dense and sparse vectors (members of this class)
  // through a single scan of the sorted key list.
  // After build, the member vectors are used in FST constructor.
  // REQUIRED: provided key list must be sorted.
  void build(const std::vector<std::string> &keys);

  static bool readBit(const std::vector<word_t> &bits, const position_t pos) {
    assert(pos < (bits.size() * kWordSize));
    position_t word_id = pos / kWordSize;
    position_t offset = pos % kWordSize;
    return (bits[word_id] & (kMsbMask >> offset));
  }

  static void setBit(std::vector<word_t> &bits, const position_t pos) {
    assert(pos < (bits.size() * kWordSize));
    position_t word_id = pos / kWordSize;
    position_t offset = pos % kWordSize;
    bits[word_id] |= (kMsbMask >> offset);
  }

  level_t getTreeHeight() const { return labels_.size(); }

  // const accessors
  const std::vector<std::vector<word_t>> &getBitmapLabels() const {
    return bitmap_labels_;
  }
  const std::vector<std::vector<word_t>> &getBitmapChildIndicatorBits() const {
    return bitmap_child_indicator_bits_;
  }
  const std::vector<std::vector<word_t>> &getPrefixkeyIndicatorBits() const {
    return prefixkey_indicator_bits_;
  }
  const std::vector<std::vector<label_t>> &getLabels() const { return labels_; }
  const std::vector<std::vector<word_t>> &getChildIndicatorBits() const {
    return child_indicator_bits_;
  }
  const std::vector<std::vector<word_t>> &getLoudsBits() const {
    return louds_bits_;
  }

  const std::vector<position_t> &getNodeCounts() const { return node_counts_; }
  level_t getSparseStartLevel() const { return sparse_start_level_; }

  std::vector<uint64_t> getDenseOffsets() const { return positions_dense_; }

  std::vector<uint64_t> getSparseOffsets() const { return positions_sparse_; }

 private:
  static bool isSameKey(const std::string &a, const std::string &b) {
    return a == b;
  }

  // Fill in the LOUDS-Sparse vectors through a single scan
  // of the sorted key list.
  void buildSparse(const std::vector<std::string> &keys);

  // Walks down the current partially-filled trie by comparing key to
  // its previous key in the list until their prefixes do not match.
  // The previous key is stored as the last items in the per-level
  // label vector.
  // For each matching prefix byte(label), it sets the corresponding
  // child indicator bit to 1 for that label.
  level_t skipCommonPrefix(const std::string &key);

  // Starting at the start_level of the trie, the function inserts
  // key bytes to the trie vectors until the first byte/label where
  // key and next_key do not match.
  // This function is called after skipCommonPrefix. Therefore, it
  // guarantees that the stored prefix of key is unique in the trie.
  level_t insertKeyBytesToTrieUntilUnique(const std::string &key,
                                          uint64_t position,
                                          const std::string &next_key,
                                          level_t start_level);

  inline bool isCharCommonPrefix(label_t c, level_t level) const;
  inline bool isLevelEmpty(level_t level) const;
  inline void moveToNextItemSlot(level_t level);
  void insertKeyByte(char c, level_t level, bool is_start_of_node,
                     bool is_term);

  // Compute sparse_start_level_ according to the pre-defined
  // size ratio between Sparse and Dense levels.
  // Dense size < Sparse size / sparse_dense_ratio_
  inline void determineCutoffLevel();

  inline uint64_t computeDenseMem(level_t downto_level) const;
  inline uint64_t computeSparseMem(level_t start_level) const;

  // Fill in the LOUDS-Dense vectors based on the built
  // Sparse vectors.
  // Called after sparse_start_level_ is set.
  void buildDense();

  void initDenseVectors(level_t level);
  void setLabelAndChildIndicatorBitmap(level_t level, position_t node_num,
                                       position_t pos);

  position_t getNumItems(level_t level) const;
  void addLevel();
  bool isStartOfNode(level_t level, position_t pos) const;
  bool isTerminator(level_t level, position_t pos) const;

 private:
  // trie level < sparse_start_level_: LOUDS-Dense
  // trie level >= sparse_start_level_: LOUDS-Sparse
  bool include_dense_{};
  uint32_t sparse_dense_ratio_{};
  level_t sparse_start_level_;

  std::vector<std::vector<uint64_t>> positions_;

  // LOUDS-Sparse bit/byte vectors
  std::vector<std::vector<label_t>> labels_;
  std::vector<std::vector<word_t>> child_indicator_bits_;
  std::vector<std::vector<word_t>> louds_bits_;
  std::vector<uint64_t> positions_sparse_;

  // LOUDS-Dense bit vectors
  std::vector<std::vector<word_t>> bitmap_labels_;
  std::vector<std::vector<word_t>> bitmap_child_indicator_bits_;
  std::vector<std::vector<word_t>> prefixkey_indicator_bits_;
  std::vector<uint64_t> positions_dense_;

  // auxiliary per level bookkeeping vectors
  std::vector<position_t> node_counts_;
  std::vector<bool> is_last_item_terminator_;
};

void FSTBuilder::build(const std::vector<std::string> &keys) {
  assert(keys.size() > 0);
  buildSparse(keys);
  if (include_dense_) {
    determineCutoffLevel();
    buildDense();
  }
}

void FSTBuilder::buildSparse(const std::vector<std::string> &keys) {
  for (position_t i = 0; i < keys.size(); i++) {
    level_t level = skipCommonPrefix(keys[i]);
    position_t curpos = i;
    while ((i + 1 < keys.size()) && isSameKey(keys[curpos], keys[i + 1])) i++;
    if (i < keys.size() - 1)
      insertKeyBytesToTrieUntilUnique(keys[curpos], curpos, keys[i + 1], level);
    else  // for last key, there is no successor key in the list
      insertKeyBytesToTrieUntilUnique(keys[curpos], curpos, std::string(),
                                      level);
  }
}

level_t FSTBuilder::skipCommonPrefix(const std::string &key) {
  level_t level = 0;
  while (level < key.length() &&
         isCharCommonPrefix((label_t)key[level], level)) {
    setBit(child_indicator_bits_[level], getNumItems(level) - 1);
    level++;
  }
  return level;
}

level_t FSTBuilder::insertKeyBytesToTrieUntilUnique(const std::string &key,
                                                    const uint64_t position,
                                                    const std::string &next_key,
                                                    const level_t start_level) {
  assert(start_level < key.length());

  level_t level = start_level;
  bool is_start_of_node = false;
  bool is_term = false;
  // If it is the start of level, the louds bit needs to be set.
  if (isLevelEmpty(level)) {
    is_start_of_node = true;
  }

  // After skipping the common prefix, the first following byte
  // should be in the node as the previous key.
  insertKeyByte(key[level], level, is_start_of_node, is_term);
  level++;

  if (level > next_key.length() ||
      !isSameKey(key.substr(0, level), next_key.substr(0, level))) {
    positions_[level - 1].emplace_back(position);
    return level;
  }

  // All the following bytes inserted must be the start of a new node.
  is_start_of_node = true;

  while (level < key.length() && level < next_key.length() &&
         key[level - 1] == next_key[level - 1]) {
    insertKeyByte(key[level], level, is_start_of_node, is_term);
    level++;
  }
  positions_[level - 1].emplace_back(position);
  return level;
}

inline bool FSTBuilder::isCharCommonPrefix(const label_t c,
                                           const level_t level) const {
  return (level < getTreeHeight()) && (!is_last_item_terminator_[level]) &&
         (c == labels_[level].back());
}

inline bool FSTBuilder::isLevelEmpty(const level_t level) const {
  return (level >= getTreeHeight()) || (labels_[level].empty());
}

inline void FSTBuilder::moveToNextItemSlot(const level_t level) {
  assert(level < getTreeHeight());
  position_t num_items = getNumItems(level);
  if (num_items % kWordSize == 0) {
    child_indicator_bits_[level].push_back(0);
    louds_bits_[level].push_back(0);
  }
}

void FSTBuilder::insertKeyByte(const char c, const level_t level,
                               const bool is_start_of_node,
                               const bool is_term) {
  // level should be at most equal to tree height
  if (level >= getTreeHeight()) addLevel();

  assert(level < getTreeHeight());

  // sets parent node's child indicator
  if (level > 0)
    setBit(child_indicator_bits_[level - 1], getNumItems(level - 1) - 1);

  labels_[level].push_back(c);
  if (is_start_of_node) {
    setBit(louds_bits_[level], getNumItems(level) - 1);
    node_counts_[level]++;
  }
  is_last_item_terminator_[level] = is_term;

  moveToNextItemSlot(level);
}

inline void FSTBuilder::determineCutoffLevel() {
  level_t cutoff_level = 0;
  uint64_t dense_mem = computeDenseMem(cutoff_level);
  uint64_t sparse_mem = computeSparseMem(cutoff_level);
  while ((cutoff_level < getTreeHeight()) &&
         (dense_mem * sparse_dense_ratio_ < sparse_mem)) {
    cutoff_level++;
    dense_mem = computeDenseMem(cutoff_level);
    sparse_mem = computeSparseMem(cutoff_level);
  }
  // cutoff_level = 3;
  sparse_start_level_ = cutoff_level--;

  // CA build dense and sparse values vectors
  for (uint64_t level = 0; level < sparse_start_level_; level++) {
    positions_dense_.insert(positions_dense_.end(), positions_[level].begin(),
                            positions_[level].end());
  }

  for (uint64_t level = sparse_start_level_; level < positions_.size();
       level++) {
    positions_sparse_.insert(positions_sparse_.end(), positions_[level].begin(),
                             positions_[level].end());
  }
  positions_.clear();
}

inline uint64_t FSTBuilder::computeDenseMem(const level_t downto_level) const {
  assert(downto_level <= getTreeHeight());
  uint64_t mem = 0;
  for (level_t level = 0; level < downto_level; level++) {
    mem += (2 * kFanout * node_counts_[level]);
    if (level > 0) mem += (node_counts_[level - 1] / 8 + 1);
  }
  return mem;
}

inline uint64_t FSTBuilder::computeSparseMem(const level_t start_level) const {
  uint64_t mem = 0;
  for (level_t level = start_level; level < getTreeHeight(); level++) {
    position_t num_items = labels_[level].size();
    mem += (num_items + 2 * num_items / 8 + 1);
  }
  return mem;
}

void FSTBuilder::buildDense() {
  for (level_t level = 0; level < sparse_start_level_; level++) {
    initDenseVectors(level);
    if (getNumItems(level) == 0) continue;

    position_t node_num = 0;
    if (isTerminator(level, 0))
      setBit(prefixkey_indicator_bits_[level], 0);
    else
      setLabelAndChildIndicatorBitmap(level, node_num, 0);
    for (position_t pos = 1; pos < getNumItems(level); pos++) {
      if (isStartOfNode(level, pos)) {
        node_num++;
        if (isTerminator(level, pos)) {
          setBit(prefixkey_indicator_bits_[level], node_num);
          continue;
        }
      }
      setLabelAndChildIndicatorBitmap(level, node_num, pos);
    }
  }
}

void FSTBuilder::initDenseVectors(const level_t level) {
  bitmap_labels_.emplace_back();
  bitmap_child_indicator_bits_.emplace_back(std::vector<word_t>());
  prefixkey_indicator_bits_.emplace_back(std::vector<word_t>());

  for (position_t nc = 0; nc < node_counts_[level]; nc++) {
    for (int i = 0; i < (int)kFanout; i += kWordSize) {
      bitmap_labels_[level].push_back(0);
      bitmap_child_indicator_bits_[level].push_back(0);
    }
    if (nc % kWordSize == 0) prefixkey_indicator_bits_[level].push_back(0);
  }
}

void FSTBuilder::setLabelAndChildIndicatorBitmap(const level_t level,
                                                 const position_t node_num,
                                                 const position_t pos) {
  label_t label = labels_[level][pos];
  setBit(bitmap_labels_[level], node_num * kFanout + label);
  if (readBit(child_indicator_bits_[level], pos))
    setBit(bitmap_child_indicator_bits_[level], node_num * kFanout + label);
}

void FSTBuilder::addLevel() {
  labels_.emplace_back(std::vector<label_t>());
  positions_.emplace_back(std::vector<uint64_t>());
  child_indicator_bits_.emplace_back(std::vector<word_t>());
  louds_bits_.emplace_back(std::vector<word_t>());

  node_counts_.push_back(0);
  is_last_item_terminator_.push_back(false);

  child_indicator_bits_[getTreeHeight() - 1].push_back(0);
  louds_bits_[getTreeHeight() - 1].push_back(0);
}

position_t FSTBuilder::getNumItems(const level_t level) const {
  return labels_[level].size();
}

bool FSTBuilder::isStartOfNode(const level_t level,
                               const position_t pos) const {
  return readBit(louds_bits_[level], pos);
}

bool FSTBuilder::isTerminator(const level_t level, const position_t pos) const {
  label_t label = labels_[level][pos];
  return ((label == kTerminator) &&
          !readBit(child_indicator_bits_[level], pos));
}
}  // namespace mmphf_fst

#endif  // FSTBUILDER_H_
