/* xdelta3 FGK secondary-compression decoder. See fgk_decode.h.

   Derived from xdelta3 3.2.2, xdelta3-fgk.h
   Copyright 2016 Joshua MacDonald

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

   Changes made by RetroHub: the encoder is not included; fgk_node / fgk_block pointers became int32 indices into the
   two fixed arrays (-1 is NULL), every access goes through N() which refuses an index outside the array, and the
   loop that walks up to the root is bounded. The algorithm, the node and block bookkeeping and the bit order are
   unchanged, so the output is identical for every stream xdelta3 can write.

   Algorithm (D. E. Knuth, "Dynamic Huffman Coding", Journal of Algorithms 6): a Huffman tree over the 256 byte values
   is rebuilt after every symbol. Byte values not seen yet share one zero-weight leaf; the first time a value appears it
   is sent as its index among the values still unseen (a fixed-length number of bits) after the code of that leaf.
   Bits are read least-significant first within a byte, one symbol is complete when the walk reaches a leaf. */

#include "fgk_decode.h"

#include <utility>

namespace xd3port {
namespace {

constexpr int32_t kAlphabet = 256;
constexpr int32_t kNodes = 2 * kAlphabet - 1;       // the leaves and the internal nodes
constexpr int32_t kBlocks = 2 * kNodes;
constexpr int32_t kNil = -1;

struct Node {
    uint32_t weight = 0;
    int32_t parent = kNil, left_child = kNil, right_child = kNil;   // the tree
    int32_t left = kNil, right = kNil;                                // neighbours in the ordering by weight
    int32_t block = kNil;                                             // the block of equal-weight nodes this node is in
};

}  // namespace

struct FgkDecoder::Impl {
    Node nodes[kNodes];
    int32_t blocks[kBlocks];          // a block's leader while the block is in use, the next free block while it is free
    int32_t free_block = 0;
    int32_t decode_ptr = 0, remaining_zeros = 0, root = 0, free_node = 0;
    uint32_t zero_count = 0, zero_exp = 0, zero_rem = 0;
    uint32_t coded_depth = 0;
    uint8_t coded_bits[16] = {};
    Node dummy;
    bool bad = false;

    Impl() {
        root = 0;
        decode_ptr = root;
        free_node = kAlphabet;
        remaining_zeros = 0;
        coded_depth = 0;
        zero_count = kAlphabet + 2;
        factor_remaining();                 // the two calls leave zero_count == kAlphabet
        factor_remaining();
        for (int32_t i = 0; i < kBlocks - 1; ++i) blocks[i] = i + 1;
        blocks[kBlocks - 1] = kNil;
        free_block = 0;
        for (int32_t i = kAlphabet - 1; i >= 0; --i) {        // the zero-weight leaves form a list through the child links
            Node& n = nodes[i];
            n.right_child = i < kAlphabet - 1 ? i + 1 : kNil;
            n.left_child = i >= 1 ? i - 1 : kNil;
        }
    }

    Node& N(int32_t i) {
        if (i < 0 || i >= kNodes) { bad = true; dummy = Node(); return dummy; }
        return nodes[i];
    }

    void factor_remaining() {
        if (zero_count == 0) { bad = true; return; }
        uint32_t i = --zero_count;
        zero_exp = 0;
        while (i > 1) { ++zero_exp; i >>= 1; }
        zero_rem = zero_count - (1u << zero_exp);
    }

    int32_t make_block(int32_t lead) {
        const int32_t ret = free_block;
        if (ret < 0 || ret >= kBlocks) { bad = true; return 0; }
        free_block = blocks[ret];
        blocks[ret] = lead;
        return ret;
    }
    void release_block(int32_t b) {
        if (b < 0 || b >= kBlocks) { bad = true; return; }
        blocks[b] = free_block;
        free_block = b;
    }
    int32_t& leader(int32_t b) {
        static int32_t sink;
        if (b < 0 || b >= kBlocks) { bad = true; return sink; }
        return blocks[b];
    }

    /* Moves `fwd` to the right end of its block of equal weight, swapping places with the block's leader. */
    void move_right(int32_t fwd) {
        const int32_t back = leader(N(fwd).block);
        if (fwd == back || N(fwd).parent == back || N(fwd).weight == 0) return;

        N(N(back).right).left = fwd;
        if (N(fwd).left != kNil) N(N(fwd).left).right = back;

        int32_t tmp = N(fwd).right;
        N(fwd).right = N(back).right;
        if (tmp == back) {
            N(back).right = fwd;
        } else {
            N(tmp).left = back;
            N(back).right = tmp;
        }

        tmp = N(back).left;
        N(back).left = N(fwd).left;
        if (tmp == fwd) {
            N(fwd).left = back;
        } else {
            N(tmp).right = fwd;
            N(fwd).left = tmp;
        }

        const int32_t fp = N(fwd).parent, bp = N(back).parent;
        const bool f_right = N(fp).right_child == fwd, b_right = N(bp).right_child == back;
        std::swap(N(fwd).parent, N(back).parent);
        std::swap(f_right ? N(fp).right_child : N(fp).left_child, b_right ? N(bp).right_child : N(bp).left_child);

        leader(N(fwd).block) = fwd;
    }

    /* Shifts `node`, the leader of its block, into the next block. */
    void promote(int32_t node) {
        const int32_t my_right = N(node).right;
        const int32_t my_left = N(node).left;
        const int32_t cur_block = N(node).block;

        if (N(node).weight == 0) return;

        /* the parent of the zero leaf: its left neighbour is its right child */
        if (my_left == N(node).right_child && N(node).left_child != kNil && N(N(node).left_child).weight == 0) {
            if (N(node).weight == N(my_right).weight - 1 && my_right != root) {
                release_block(cur_block);
                N(node).block = N(my_right).block;
                N(my_left).block = N(my_right).block;
            }
            return;
        }

        if (my_left == remaining_zeros) return;

        if (N(my_left).block == cur_block) leader(N(my_left).block) = my_left;
        else release_block(cur_block);

        if (N(node).weight == N(my_right).weight - 1 && my_right != root) N(node).block = N(my_right).block;
        else N(node).block = make_block(node);
    }

    /* Takes `node` out of the list of zero-weight leaves. */
    void eliminate_zero(int32_t node) {
        if (zero_count == 1) return;
        factor_remaining();
        if (N(node).left_child == kNil) {
            remaining_zeros = N(remaining_zeros).right_child;
            N(remaining_zeros).left_child = kNil;
        } else if (N(node).right_child == kNil) {
            N(N(node).left_child).right_child = kNil;
        } else {
            N(N(node).right_child).left_child = N(node).left_child;
            N(N(node).left_child).right_child = N(node).right_child;
        }
    }

    /* A value is seen for the first time: it leaves the zero list and gets an internal node above it. */
    int32_t increase_zero_weight(int32_t n) {
        const int32_t this_zero = n;

        if (zero_count == 1) {                       // the last unseen value
            N(this_zero).right_child = kNil;
            if (N(N(this_zero).right).weight == 1) N(this_zero).block = N(N(this_zero).right).block;
            else N(this_zero).block = make_block(this_zero);
            remaining_zeros = kNil;
            return this_zero;
        }

        const int32_t zero_ptr = remaining_zeros;
        if (free_node >= kNodes) { bad = true; return this_zero; }
        const int32_t new_internal = free_node++;

        N(new_internal).parent = N(zero_ptr).parent;
        N(new_internal).right = N(zero_ptr).right;
        N(new_internal).weight = 0;
        N(new_internal).right_child = this_zero;
        N(new_internal).left = this_zero;

        if (remaining_zeros == root) {               // the very first value
            root = new_internal;
            N(this_zero).block = make_block(this_zero);
            N(new_internal).block = make_block(new_internal);
        } else {
            N(N(new_internal).right).left = new_internal;
            if (N(N(zero_ptr).parent).right_child == zero_ptr) N(N(zero_ptr).parent).right_child = new_internal;
            else N(N(zero_ptr).parent).left_child = new_internal;

            if (N(N(new_internal).right).weight == 1) N(new_internal).block = N(N(new_internal).right).block;
            else N(new_internal).block = make_block(new_internal);

            N(this_zero).block = N(new_internal).block;
        }

        eliminate_zero(this_zero);

        N(new_internal).left_child = remaining_zeros;

        N(this_zero).right = new_internal;
        N(this_zero).left = remaining_zeros;
        N(this_zero).parent = new_internal;
        N(this_zero).left_child = kNil;
        N(this_zero).right_child = kNil;

        N(remaining_zeros).parent = new_internal;
        N(remaining_zeros).right = this_zero;
        return this_zero;
    }

    /* The tree after value n was decoded. */
    void update_tree(int32_t n) {
        int32_t incr = N(n).weight == 0 ? increase_zero_weight(n) : n;
        for (int guard = 0; incr != root && !bad; ++guard) {
            if (guard > 2 * kNodes) { bad = true; return; }
            move_right(incr);
            promote(incr);
            N(incr).weight += 1;
            incr = N(incr).parent;
        }
        N(root).weight += 1;
    }

    /* One more bit of the code: true when a whole code (and, for a new value, its index) has been read. */
    bool decode_bit(unsigned b) {
        if (N(decode_ptr).weight == 0) {              // the zero leaf: a fixed-length index among the unseen values follows
            const uint32_t bitsreq = zero_rem == 0 ? zero_exp : zero_exp + 1;
            if (coded_depth >= sizeof coded_bits) { bad = true; return false; }
            coded_bits[coded_depth++] = (uint8_t)b;
            return coded_depth >= bitsreq;
        }
        decode_ptr = b ? N(decode_ptr).right_child : N(decode_ptr).left_child;
        if (N(decode_ptr).left_child == kNil) {       // a leaf
            if (N(decode_ptr).weight != 0) return true;
            return zero_count == 1;
        }
        return false;
    }

    int32_t nth_zero(uint32_t n) {
        int32_t ret = remaining_zeros;
        for (; n != 0 && N(ret).right_child != kNil; --n) ret = N(ret).right_child;
        return ret;
    }

    int32_t decode_data() {
        int32_t elt = decode_ptr;
        if (N(decode_ptr).weight == 0) {
            uint32_t n = 0, i = 0;
            if (coded_depth > 0) {
                for (; i < coded_depth - 1; ++i) {
                    n |= coded_bits[i];
                    n <<= 1;
                }
            }
            n |= coded_bits[i];
            elt = nth_zero(n);
        }
        coded_depth = 0;
        if (elt < 0 || elt >= kAlphabet) { bad = true; return 0; }
        update_tree(elt);
        decode_ptr = root;
        return elt;
    }
};

FgkDecoder::FgkDecoder() : p_(new Impl) {}
FgkDecoder::~FgkDecoder() = default;

const char* FgkDecoder::decode(const uint8_t* in, size_t in_n, uint8_t* out, size_t out_n) {
    Impl& h = *p_;
    if (out_n == 0) return "empty section";
    if (h.bad) return "model failed earlier";
    size_t o = 0;
    for (size_t i = 0; i < in_n; ++i) {
        const unsigned byte = in[i];
        for (unsigned mask = 1; mask != 0x100; mask <<= 1) {
            const bool done = h.decode_bit((byte & mask) ? 1u : 0u);
            if (h.bad) return "bad data";
            if (!done) continue;
            out[o++] = (uint8_t)h.decode_data();
            if (h.bad) return "bad data";
            if (o == out_n) return nullptr;
        }
    }
    return "truncated section";
}

}  // namespace xd3port
