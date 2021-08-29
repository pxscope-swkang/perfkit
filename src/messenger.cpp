//
// Created by Seungwoo on 2021-08-25.
//
#include <nlohmann/detail/conversions/from_json.hpp>
#include <perfkit/detail/messenger.hpp>

namespace {
struct hasher {
  constexpr static uint64_t FNV_PRIME        = 0x100000001b3ull;
  constexpr static uint64_t FNV_OFFSET_BASIS = 0xcbf29ce484222325ull;

  /// hash a single byte
  static inline uint64_t fnv1a_byte(unsigned char byte, uint64_t hash) {
    return (hash ^ byte) * FNV_PRIME;
  }
};
}  // namespace

using namespace perfkit;

messenger::_msg_entity* messenger::_fork_branch(
    _msg_entity const* parent, std::string_view name, bool initial_subscribe_state) {
  auto hash = _hash_active(parent, name);

  auto [it, is_new] = _table.try_emplace(hash);
  auto& data        = it->second;

  if (is_new) {
    data.key_buffer          = std::string(name);
    data.body.hash           = hash;
    data.body.key            = data.key_buffer;
    data.body._is_subscribed = &data.is_subscribed;
    data.is_subscribed.store(initial_subscribe_state, std::memory_order_relaxed);
    parent && (data.hierarchy = parent->hierarchy, true);  // only includes parent hierarchy.
    data.hierarchy.push_back(data.key_buffer);
    data.body.hierarchy = data.hierarchy;
  }

  data.body.fence = _fence_active;
  data.body.order = _order_active++;

  return &data;
}

uint64_t messenger::_hash_active(_msg_entity const* parent, std::string_view top) {
  // --> 계층은 전역으로 관리되면 안 됨 ... 각각의 프록시가 관리해야함!!
  // Hierarchy 각각의 데이터 엔티티 기반으로 관리되게 ... _hierarchy_hash 관련 기능 싹 갈아엎기

  auto hash = hasher::FNV_OFFSET_BASIS;
  if (parent) {
    hash = parent->body.hash;
  }

  for (auto c : top) { hash = hasher::fnv1a_byte(c, hash); }
  return hash;
}

messenger::proxy messenger::fork(const std::string& n) {
  if (auto _lck = std::unique_lock{_sort_merge_lock}) {
    // perform queued sort-merge operation
    if (_msg_promise) {
      auto& promise = *_msg_promise;

      // copies all messages and put them to cache buffer to prevent memory reallocation
      _local_reused_memory.resize(_table.size());
      std::transform(_table.begin(), _table.end(),
                     _local_reused_memory.begin(),
                     [](std::pair<const uint64_t, _msg_entity> const& g) { return g.second.body; });

      msg_fetch_result rs;
      rs._mtx_access = &_sort_merge_lock;
      rs._data       = &_local_reused_memory;
      promise.set_value(rs);

      _msg_future  = {};
      _msg_promise = {};
    }
  }

  // init new iteration
  _fence_active++;
  _order_active = 0;

  proxy prx;
  prx._owner             = this;
  prx._ref               = _fork_branch(nullptr, n, false);
  prx._epoch_if_required = clock_type::now();

  return prx;
}

namespace {
struct message_block_sorter {
  int         n;
  friend bool operator<(messenger* ptr, message_block_sorter s) { return s.n < ptr->order(); }
};
}  // namespace

messenger::messenger(int order, std::string_view name) noexcept
    : _occurence_order(order), _name(name) {
  auto it_insert = std::lower_bound(_all().begin(), _all().end(), message_block_sorter{order});
  _all().insert(it_insert, this);
}

std::vector<messenger*>& messenger::_all() noexcept {
  static std::vector<messenger*> inst;
  return inst;
}

array_view<messenger*> messenger::all_blocks() noexcept {
  return _all();
}

std::shared_future<messenger::msg_fetch_result> messenger::async_fetch_request() {
  auto _lck = std::unique_lock{_sort_merge_lock};

  // if there's already queued merge operation, share future once more.
  if (_msg_promise) {
    return _msg_future;
  }

  // if not, queue new promise and make its future shared.
  _msg_promise.emplace();
  _msg_future = std::shared_future(_msg_promise->get_future());
  return _msg_future;
}

messenger::proxy messenger::proxy::branch(std::string_view n) noexcept {
  proxy px;
  px._owner = _owner;
  px._ref   = _owner->_fork_branch(_ref, n, false);
  return px;
}

messenger::proxy messenger::proxy::timer(std::string_view n) noexcept {
  proxy px;
  px._owner             = _owner;
  px._ref               = _owner->_fork_branch(_ref, n, false);
  px._epoch_if_required = clock_type::now();
  return px;
}

messenger::proxy::~proxy() noexcept {
  if (!_owner) { return; }
  if (_epoch_if_required != clock_type::time_point{}) {
    data() = clock_type::now() - _epoch_if_required;
  }
}

messenger::variant_type& messenger::proxy::data() noexcept {
  return _ref->body.data;
}

void messenger::msg_fetch_result::copy_sorted(fetched_messages& out) noexcept {
  {
    auto [lck, ptr] = acquire();
    out             = *ptr;
  }
  sort_messages_by_rule(out);
}

namespace {
enum class hierarchy_compare_result {
  irrelevant,
  a_contains_b,
  b_contains_a,
  equal
};

auto compare_hierarchy(array_view<std::string_view> a, array_view<std::string_view> b) {
  size_t num_equals = 0;

  for (size_t i = 0, n_max = std::min(a.size(), b.size()); i < n_max; ++i) {
    auto r_cmp = a[i].compare(b[i]);
    if (r_cmp != 0) { break; }
    ++num_equals;
  }

  if (a.size() == b.size()) {
    return num_equals == a.size()
               ? hierarchy_compare_result::equal
               : hierarchy_compare_result::irrelevant;
  }
  if (num_equals != a.size() && num_equals != b.size()) {
    return hierarchy_compare_result::irrelevant;
  }

  if (num_equals == a.size()) {
    return hierarchy_compare_result::b_contains_a;
  } else {
    return hierarchy_compare_result::a_contains_b;
  }
}

}  // namespace

void perfkit::sort_messages_by_rule(messenger::fetched_messages& msg) noexcept {
  std::sort(
      msg.begin(), msg.end(),
      [](messenger::message const& a, messenger::message const& b) {
        auto r_hcmp = compare_hierarchy(a.hierarchy, b.hierarchy);
        if (r_hcmp == hierarchy_compare_result::equal) {
          return a.fence == b.fence
                     ? a.order < b.order
                     : a.fence < b.fence;
        }
        if (r_hcmp == hierarchy_compare_result::irrelevant) {
          return a.order < b.order;
        }
        if (r_hcmp == hierarchy_compare_result::a_contains_b) {
          return true;
        }
        if (r_hcmp == hierarchy_compare_result::b_contains_a) {
          return false;
        }

        throw;
      });
}