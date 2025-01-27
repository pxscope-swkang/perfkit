#include <chrono>

#include <spdlog/spdlog.h>

#include "perfkit/extension/cli.hpp"
#include "perfkit/perfkit.h"
#include "test_configs.hxx"

using namespace std::literals;

PERFKIT_T_CATEGORY(
        my_cat,
        PERFKIT_T_CONFIGURE(some_name, "vlvl")
                .confirm();

        PERFKIT_T_SUBCATEGORY(
                subc,
                PERFKIT_T_CONFIGURE(some_varnam2e, 1.31)
                        .confirm();

                PERFKIT_T_SUBCATEGORY(
                        subc2,
                        PERFKIT_T_CONFIGURE(somvar, "hell,world!").confirm()))

                PERFKIT_T_CONFIGURE(some_other_var, 5.51)
                        .confirm());

int main(int argc, char** argv)
{
    perfkit::configs::parse_args(&argc, &argv, true);
    perfkit::glog()->set_level(spdlog::level::trace);

    auto term = perfkit::terminal::create_cli();
    perfkit::terminal::initialize_with_basic_commands(term.get());

    spdlog::info("create");
    spdlog::set_level(spdlog::level::trace);

    term->commands()
            ->root()
            ->add_subcommand("test space command")
            ->add_subcommand("test command 2")
            ->add_subcommand("test command 3");

    term->commands()
            ->root()
            ->add_subcommand("dump", [&] { term->write(perfkit::configs::export_all().dump(2)); });

    std::vector<std::unique_ptr<my_cat>> sample;
    nlohmann::json conf;

    std::string latest_cmd;
    for (size_t ic = 0;;)
    {
        cfg::registry().update();
        do_trace(++ic, latest_cmd);

        auto cmd = term->fetch_command(1000ms);
        if (!cmd.has_value() || cmd->empty()) { continue; }
        latest_cmd = *cmd;

        if (cmd == "q") { break; }

        if (cmd == "gg_push")
        {
            sample.push_back(std::make_unique<my_cat>("zc-" + std::to_string(rand())));
            continue;
        }
        else if (cmd == "gg_pop")
        {
            if (sample.empty() == false)
                sample.pop_back();
        }
        else if (cmd == "gg_dump")
        {
            conf = perfkit::configs::export_all();
            spdlog::info("dump: {}", conf.dump(2));
        }
        else if (cmd == "gg_load")
        {
            spdlog::info("config: {}", conf.dump(2));
            spdlog::info("importing ...");
            perfkit::configs::import_from(conf);
        }

        if (!term->commands()->invoke_command(*cmd))
            spdlog::error("execution failed .. '{}'", *cmd);
    }

    spdlog::info("done.");
    return 0;
}