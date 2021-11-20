//
// Created by Seungwoo on 2021-08-25.
//
#pragma once
#include <any>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "perfkit/common/array_view.hxx"

namespace perfkit {
using json = nlohmann::json;

namespace detail {
class config_base;
}

class config_registry;
using config_shared_ptr = std::shared_ptr<detail::config_base>;
using config_wptr       = std::weak_ptr<detail::config_base>;
using std::shared_ptr;
using std::weak_ptr;

namespace detail {

/**
 * basic config class
 *
 * TODO: Attribute retrieval for config class
 */
class config_base {
 public:
  using deserializer = std::function<bool(nlohmann::json const&, void*)>;
  using serializer   = std::function<void(nlohmann::json&, void const*)>;

 public:
  config_base(class config_registry* owner,
              void* raw,
              std::string full_key,
              std::string description,
              deserializer fn_deserial,
              serializer fn_serial,
              nlohmann::json&& attribute);

  /**
   * @warning this function is not re-entrant!
   * @return
   */
  nlohmann::json serialize();
  void serialize(nlohmann::json&);
  void serialize(std::function<void(nlohmann::json const&)> const&);

  nlohmann::json const& attribute() const noexcept { return _attribute; }
  nlohmann::json const& default_value() const { return _attribute["default"]; }

  bool consume_dirty() { return _dirty.exchange(false); }

  auto const& full_key() const { return _full_key; }
  auto const& display_key() const { return _display_key; }
  auto const& description() const { return _description; }
  auto tokenized_display_key() const { return make_view(_categories); }
  void request_modify(nlohmann::json js);

  size_t num_modified() const { return _fence_modified; };
  size_t num_serialized() const { return _fence_serialized; }

  bool can_export() const noexcept { return not attribute().contains("transient"); }
  bool can_import() const noexcept { return not attribute().contains("block_read"); }

  /**
   * Check if latest marshalling result was invalid
   * @return
   */
  bool latest_marshal_failed() const {
    return _latest_marshal_failed.load(std::memory_order_relaxed);
  }

 private:
  bool _try_deserialize(nlohmann::json const& value);
  static void _split_categories(std::string_view view, std::vector<std::string_view>& out);

 private:
  friend class perfkit::config_registry;
  perfkit::config_registry* _owner;

  std::string _full_key;
  std::string _display_key;
  std::string _description;
  void* _raw;
  std::atomic_bool _dirty                 = true;  // default true to trigger initialization
  std::atomic_bool _latest_marshal_failed = false;

  std::atomic_size_t _fence_modified   = 0;
  std::atomic_size_t _fence_serialized = ~size_t{};
  nlohmann::json _cached_serialized;
  nlohmann::json _attribute;

  std::vector<std::string_view> _categories;

  deserializer _deserialize;
  serializer _serialize;
};
}  // namespace detail

/**
 *
 */
namespace configs {
// clang-format off
struct duplicated_flag_binding : std::logic_error { using std::logic_error::logic_error; };
struct invalid_flag_name : std::logic_error { using std::logic_error::logic_error; };
struct parse_error : std::runtime_error { using std::runtime_error::runtime_error; };
struct parse_help : parse_error { using parse_error::parse_error; };
// clang-format on

using flag_binding_table = std::map<std::string, config_shared_ptr, std::less<>>;
flag_binding_table& _flags() noexcept;

void parse_args(int* argc, char*** argv, bool consume, bool ignore_undefined = false);
void parse_args(std::vector<std::string_view>* args, bool consume, bool ignore_undefined = false);

void import_from(const json& data);
json export_all();

bool import_file(std::string_view path);
bool export_to(std::string_view path);

/** wait until any configuration update is applied. */
bool wait_any_change(std::chrono::milliseconds timeout, uint64_t* out_fence);
}  // namespace configs

/**
 *
 */
class config_registry : public std::enable_shared_from_this<config_registry> {
 public:
  using json_table        = std::map<std::string_view, nlohmann::json>;
  using config_table      = std::map<std::string_view, std::shared_ptr<detail::config_base>>;
  using string_view_table = std::map<std::string_view, std::string_view>;
  using container         = std::map<std::string, weak_ptr<config_registry>, std::less<>>;

 private:
  explicit config_registry(std::string name);

 public:
  ~config_registry() noexcept;

 public:
  bool update();
  void export_to(nlohmann::json*);
  void import_from(nlohmann::json);

  auto const& name() const { return _name; }

 public:
  bool bk_queue_update_value(std::string_view full_key, json value);
  std::string_view bk_find_key(std::string_view display_key);
  auto const& bk_all() const noexcept { return _entities; }
  uint64_t bk_schema_hash() const noexcept { return _schema_hash; }

 public:
  static auto bk_enumerate_registries() noexcept -> std::vector<std::shared_ptr<config_registry>>;
  static auto bk_find_reg(std::string_view name) noexcept -> shared_ptr<config_registry>;

  // TODO: deprecate this!
  static shared_ptr<config_registry> create(std::string name);

 public:  // for internal use only.
  auto _access_lock() { return std::unique_lock{_update_lock}; }

 public:
  void _put(std::shared_ptr<detail::config_base> o);
  bool _initially_updated() const noexcept { return _initial_update_done.load(); }

 private:
  std::string _name;
  config_table _entities;
  string_view_table _disp_keymap;
  std::vector<detail::config_base*> _pending_updates[2];
  std::mutex _update_lock;

  // this value is used for identifying config registry's schema type, as config registry's
  //  layout never changes after updated once.
  uint64_t _schema_hash;

  // since configurations can be loaded before registry instance loaded, this flag makes
  //  the first update of registry to apply loaded configurations.
  std::atomic_bool _initial_update_done{false};
};

namespace _attr_flag {
enum ty : uint64_t {
  has_min      = 0x01 << 0,
  has_max      = 0x01 << 1,
  has_validate = 0x01 << 2,
  has_one_of   = 0x01 << 3,
  has_verify   = 0x01 << 4,
};
}

enum class _config_io_type {
  persistent,
  transient,
  transient_readonly,
};

template <typename Ty_>
class config;

template <typename Ty_>
struct _config_attrib_data {
  std::string description;
  std::function<bool(Ty_&)> validate;
  std::function<bool(Ty_ const&)> verify;
  std::optional<Ty_> min;
  std::optional<Ty_> max;
  std::optional<std::set<Ty_>> one_of;
  std::string env_name;

  std::optional<std::vector<std::string>> flag_binding;
  _config_io_type transient_type = _config_io_type::persistent;
};

template <typename Ty_, uint64_t Flags_ = 0>
class _config_factory {
 public:
  enum { flag = Flags_ };

 private:
  template <uint64_t Flag_>
  auto& _added() { return *reinterpret_cast<_config_factory<Ty_, Flags_ | Flag_>*>(this); }

 public:
  auto& description(std::string&& s) { return _data.description = std::move(s), *this; }
  auto& min(Ty_ v) { return _data.min = v, _added<_attr_flag::has_min>(); }
  auto& max(Ty_ v) { return _data.max = v, _added<_attr_flag::has_max>(); }

  /** value should be one of given entities */
  auto& one_of(std::initializer_list<Ty_> v) {
    _data.one_of.emplace();
    _data.one_of->insert(v.begin(), v.end());
    return _added<_attr_flag::has_one_of>();
  }

  /**
   * validate function makes value assignable to destination.
   * if callback returns false, change won't be applied (same as verify()) */
  template <typename Callable_>
  auto& validate(Callable_&& v) {
    if constexpr (std::is_invocable_r_v<bool, Callable_, Ty_&>) {
      _data.validate = std::move(v);
    } else {
      _data.validate =
              [fn = std::forward<Callable_>(v)](Ty_& s) {
                fn(s);
                return true;
              };
    }
    return _added<_attr_flag::has_validate>();
  }

  /** verify function will discard updated value if verification failed. */
  auto& verify(std::function<bool(Ty_ const&)>&& v) {
    _data.verify = std::move(v);
    return _added<_attr_flag::has_verify>();
  }

  /** expose entities as flags. configs MUST NOT BE field of any config template class! */
  template <typename... Str_>
  auto& flags(Str_&&... args) {
    std::vector<std::string> flags;
    (flags.emplace_back(std::forward<Str_>(args)), ...);

    _data.flag_binding.emplace(std::move(flags));
    return *this;
  }

  template <typename... Str_>
  [[deprecated]] auto&
  make_flag(bool save_to_config, Str_&&... args) {
    if (not save_to_config)
      transient();
    return flags(std::forward<Str_>(args)...);
  }

  /** transient marked configs won't be saved or loaded from config files. */
  auto& transient() {
    _data.transient_type = _config_io_type::transient;
    return *this;
  }

  /** readonly marked configs won't export, but still can import from config files. */
  auto& readonly() {
    _data.transient_type = _config_io_type::transient_readonly;
    return *this;
  }

  /** */
  auto& env(std::string s) {
    _data.env_name = std::move(s);
    return *this;
  }

  auto confirm() noexcept {
    return config<Ty_>{
            *_pinfo->dispatcher,
            std::move(_pinfo->full_key),
            std::forward<Ty_>(_pinfo->default_value),
            std::move(_data)};
  }

 private:
  template <typename>
  friend class config;

  _config_attrib_data<Ty_> _data;

 public:
  struct _init_info {
    config_registry* dispatcher = {};
    std::string full_key        = {};
    Ty_ default_value           = {};
  };

  std::shared_ptr<_init_info> _pinfo;
};

template <typename Ty_>
class config {
 public:
 public:
  template <typename Attr_ = _config_factory<Ty_>>
  config(
          config_registry& repo,
          std::string full_key,
          Ty_&& default_value,
          _config_attrib_data<Ty_> attribute) noexcept
          : _owner(&repo), _value(std::forward<Ty_>(default_value)) {
    auto description = std::move(attribute.description);
    std::string env  = std::move(attribute.env_name);

    // define serializer
    detail::config_base::serializer fn_d = [this](nlohmann::json& out, const void* in) {
      out = *(Ty_*)in;
    };

    // set reference attribute
    nlohmann::json js_attrib;
    js_attrib["default"] = _value;

    if constexpr (Attr_::flag & _attr_flag::has_min) {
      js_attrib["min"] = *attribute.min;
    }
    if constexpr (Attr_::flag & _attr_flag::has_max) {
      js_attrib["max"] = *attribute.max;
    }
    if constexpr (Attr_::flag & _attr_flag::has_one_of) {
      js_attrib["oneof"] = *attribute.oneof;
    }
    if constexpr (Attr_::flag & _attr_flag::has_validate) {
      js_attrib["has_custom_validator"] = true;
    } else {
      js_attrib["has_custom_validator"] = false;
    }

    if (attribute.flag_binding) {
      std::vector<std::string>& binding = *attribute.flag_binding;
      js_attrib["is_flag"]              = true;
      if (not binding.empty()) {
        js_attrib["flag_binding"] = std::move(binding);
      }
    }

    if (attribute.transient_type != _config_io_type::persistent) {
      js_attrib["transient"] = true;
      if (attribute.transient_type == _config_io_type::transient)
        js_attrib["block_read"] = true;
    }

    // setup marshaller / de-marshaller with given rule of attribute
    detail::config_base::deserializer fn_m = [attrib = std::move(attribute)]  //
            (const nlohmann::json& in, void* out) {
              try {
                Ty_ parsed;

                bool okay = true;

                _config_attrib_data<Ty_> const& attr = attrib;
                nlohmann::from_json(in, parsed);

                if constexpr (Attr_::flag & _attr_flag::has_min) {
                  parsed = std::max<Ty_>(*attr.min, parsed);
                }
                if constexpr (Attr_::flag & _attr_flag::has_max) {
                  parsed = std::min<Ty_>(*attr.max, parsed);
                }
                if constexpr (Attr_::flag & _attr_flag::has_one_of) {
                  if (attr->oneof.find(parsed) == attr->oneof.end())
                    return false;
                }
                if constexpr (Attr_::flag & _attr_flag::has_verify) {
                  if (not attr.verify(parsed))
                    return false;
                }
                if constexpr (Attr_::flag & _attr_flag::has_validate) {
                  okay |= attr.validate(parsed);  // value should be validated
                }

                *(Ty_*)out = parsed;
                return okay;
              } catch (std::exception&) {
                return false;
              }
            };

    // instantiate config instance
    _opt = std::make_shared<detail::config_base>(
            _owner,
            &_value,
            std::move(full_key),
            std::move(description),
            std::move(fn_m),
            std::move(fn_d),
            std::move(js_attrib));

    // put instance to global queue
    repo._put(_opt);

    // queue environment value if available
    if (not env.empty())
      if (auto env_value = getenv(env.c_str())) {
        auto parsed_json = nlohmann::json::parse(
                env_value, env_value + strlen(env_value), nullptr, false);

        if (not parsed_json.is_discarded())
          _opt->request_modify(std::move(parsed_json));
      }
  }

  config(const config&) noexcept = delete;
  config(config&&) noexcept      = default;

  /**
   * @warning
   *    Reading Ty_ and invocation of update() MUST occur on same thread!
   *
   * @return
   */
  [[deprecated]] Ty_ const& get() const noexcept { return _value; }
  Ty_ value() const noexcept { return _copy(); }
  Ty_ const& ref() const noexcept { return _value; }

  /**
   * Provides thread-safe access for configuration.
   *
   * @return
   */
  Ty_ _copy() const noexcept { return _owner->_access_lock(), Ty_{_value}; }

  Ty_ const& operator*() const noexcept { return ref(); }
  Ty_ const* operator->() const noexcept { return &ref(); }
  operator Ty_() const noexcept { return _copy(); }

  [[deprecated]] bool check_dirty_and_consume() const { return _opt->consume_dirty(); }
  bool check_update() const { return _opt->consume_dirty(); }
  void async_modify(Ty_ v) { _owner->bk_queue_update_value(_opt->full_key(), std::move(v)); }

  auto& base() const { return *_opt; }

 private:
  config_shared_ptr _opt;

  config_registry* _owner;
  Ty_ _value;
};

template <typename Ty_>
using _cvt_ty = std::conditional_t<
        std::is_convertible_v<Ty_, std::string>,
        std::string,
        std::remove_reference_t<Ty_>>;

template <typename Ty_>
auto configure(config_registry& dispatcher,
               std::string&& full_key,
               Ty_&& default_value) noexcept {
  _config_factory<_cvt_ty<Ty_>> attribute;
  attribute._pinfo                = std::make_shared<typename _config_factory<_cvt_ty<Ty_>>::_init_info>();
  attribute._pinfo->dispatcher    = &dispatcher;
  attribute._pinfo->default_value = std::move(default_value);
  attribute._pinfo->full_key      = std::move(full_key);

  return attribute;
}

}  // namespace perfkit
