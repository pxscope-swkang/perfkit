//
// Created by ki608 on 2021-11-21.
//

#pragma once
#include <functional>
#include <memory>

#include <nlohmann/json.hpp>
#include <perfkit/common/event.hxx>
#include <perfkit/extension/net.hpp>

namespace perfkit::terminal::net::detail {
class basic_dispatcher_impl;
}

namespace perfkit::terminal::net {
class dispatcher
{
    using impl_ptr          = std::unique_ptr<detail::basic_dispatcher_impl>;
    using recv_archive_type = nlohmann::json;
    using send_archive_type = nlohmann::json;

   public:
    explicit dispatcher(terminal_init_info const& init_info);
    ~dispatcher();

    template <typename MsgTy_,
              typename HandlerFn_,
              typename = std::enable_if_t<std::is_invocable_v<HandlerFn_, MsgTy_>>>
    void on_recv(HandlerFn_&& handler)
    {
        // TODO: change parser as SAX parser one with reflection, to minimize heap usage
        _register_recv(
                MsgTy_::ROUTE,
                [fn = std::forward<HandlerFn_>(handler)](recv_archive_type const& message) {
                    try
                    {
                        fn(message.get<MsgTy_>());
                        return true;
                    }
                    catch (std::exception&)
                    {
                        return false;  // failed to parse message ...
                    }
                });
    }

    template <typename MsgTy_>
    void send(MsgTy_ const& message)
    {
        // TODO: change serializer to use builder, which doesn't need to be marshaled into
        //        json object to be dumped to string.
        _send(MsgTy_::ROUTE, ++_fence, &message,
              [](send_archive_type* archive, void const* data) {
                  *archive = *(MsgTy_ const*)data;
              });
    }

    void dispatch(perfkit::function<void()>);
    void post(perfkit::function<void()>);

    std::pair<size_t, size_t>
    num_bytes_in_out() const noexcept;

    perfkit::event<int>& on_new_connection();
    perfkit::event<>& on_no_connection();

    void launch();

   private:
    void _register_recv(
            std::string route,
            std::function<bool(recv_archive_type const& parameter)>);

    void _send(
            std::string_view route,
            int64_t fence,
            void const* userobj,
            void (*payload)(send_archive_type*, void const*));

   private:
    std::unique_ptr<detail::basic_dispatcher_impl> self;
    int64_t _fence = 0;
};

}  // namespace perfkit::terminal::net
