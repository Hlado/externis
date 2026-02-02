/**
 * Copyright (C) 2022 Roy Jacobson
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "externis.h"
#include "utils.h"

#include <cp/cp-tree.h>
#include <options.h>
#include <tree-check.h>
#include <tree-pass.h>
#include <tree.h>

#include "c-family/c-pragma.h"
#include "cpplib.h"

#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <unordered_map>

int plugin_is_GPL_compatible = 1;

namespace insight {

std::shared_ptr<std::ostream> trace;
Clock::time_point compilationStartTimestamp = Clock::now();
Clock::time_point lastEventTimestamp = Clock::now();
std::string stage;
std::deque<std::pair<std::string, std::chrono::nanoseconds>> actionLog;
std::string activePass;

std::chrono::nanoseconds measure()
{
  return Clock::now() - lastEventTimestamp;
}

template <void (*CallbackV)(void *, void *)>
void generic_callback(void *gccData, void *userData)
{
  if (!trace) {
    return;
  }

  CallbackV(gccData, userData);

  lastEventTimestamp = Clock::now();
}

template <void (*CallbackV)(void *, void *)>
void frontend_callback(void *gccData, void *userData)
{
  stage = "Frontend";

  generic_callback<CallbackV>(gccData, userData);
}

template <void (*CallbackV)(void *, void *)>
void backend_callback(void *gccData, void *userData)
{
  stage = "Backend";

  if (!activePass.empty()) {
    actionLog.emplace_back(stage + ";" + activePass, measure());
    activePass.clear();
  }

  generic_callback<CallbackV>(gccData, userData);
}

} // namespace insight

namespace externis {

void cb_finish_parse_function(void *gcc_data, void *user_data)
{
  tree decl = (tree)gcc_data;
  auto expanded_location = expand_location(decl->decl_minimal.locus);
  auto decl_name = decl_as_string(decl, 0);
  auto parent_decl = DECL_CONTEXT(decl);
  const char *scope_name = nullptr;
  externis::EventCategory scope_type = externis::EventCategory::UNKNOWN;
  if (parent_decl) {
    if (TREE_CODE(parent_decl) != TRANSLATION_UNIT_DECL) {
      scope_name = decl_as_string(parent_decl, 0);
      switch (TREE_CODE(parent_decl)) {
      case NAMESPACE_DECL:
        scope_type = externis::EventCategory::NAMESPACE;
        break;
      case RECORD_TYPE:
      case UNION_TYPE:
        scope_type = externis::EventCategory::STRUCT;
        break;
      default:
        fprintf(stderr, "Unkown tree code %d\n", TREE_CODE(parent_decl));
        break;
      }
    }
  }
  end_parse_function(FinishedFunction{gcc_data, decl_name, expanded_location.file, scope_name, scope_type});
}

void cb_plugin_finish(void *gcc_data, void *user_data)
{
  auto compilationFinishTimestamp = insight::Clock::now();

  insight::log("finished processing of '", main_input_filename, "'");

  std::unordered_map<std::string, std::chrono::nanoseconds> actions;
  for (auto &&[name, duration] : insight::actionLog) {
    auto base = actions.count(name) == 0 ? std::chrono::nanoseconds::zero() : actions[name];
    actions[name] = base + duration;
  }

  auto total = std::chrono::nanoseconds::zero();
  for (auto &&[name, duration] : actions) {
    *insight::trace << main_input_filename << ";" << name << " "
                    << (std::chrono::duration_cast<std::chrono::microseconds>(duration)).count() << "\n";
    total += duration;
  }

  auto uncategorized = (compilationFinishTimestamp - insight::compilationStartTimestamp) - total;
  *insight::trace << main_input_filename << ";" << "Uncategorized "
                  << (std::chrono::duration_cast<std::chrono::microseconds>(uncategorized)).count() << "\n";
}

void (*old_file_change_cb)(cpp_reader *, const line_map_ordinary *);
void cb_file_change(cpp_reader *pfile, const line_map_ordinary *new_map)
{
  if (new_map) {
    const char *file_name = ORDINARY_MAP_FILE_NAME(new_map);
    if (file_name) {
      switch (new_map->reason) {
      case LC_ENTER:
        start_preprocess_file(file_name, pfile);
        break;
      case LC_LEAVE:
        end_preprocess_file();
        break;
      default:
        break;
      }
    }
  }
  (*old_file_change_cb)(pfile, new_map);
}

// void cb_start_compilation(void *gcc_data, void *user_data) {
//   start_preprocess_file(main_input_filename, nullptr);
//   cpp_callbacks *cpp_cbs = cpp_get_callbacks(parse_in);
//   old_file_change_cb = cpp_cbs->file_change;
//   cpp_cbs->file_change = cb_file_change;
// }


void cb_start_unit(void *gcc_data, void *user_data)
{
  insight::log("started processing of '", main_input_filename, "'");

  if (!std::filesystem::exists(main_input_filename)) {
    insight::log("input is not a file, dumping to cout");
    insight::trace = std::shared_ptr<std::ostream>(&std::cout, [](auto &&) {});
  } else {
    auto dumpPath = std::filesystem::path(dump_base_name);
    dumpPath += ".trace.collapsed";
    insight::log("dumping to '", dumpPath.string(), "'");

    auto tmp = std::make_shared<std::ofstream>(dumpPath);
    if (!tmp->fail()) {
      insight::trace = tmp;
    }
  }

  insight::stage = "Preprocessing";

  insight::compilationStartTimestamp = insight::Clock::now();
}

// void cb_pass_execution(void *gcc_data, void *user_data) {
//   auto pass = (opt_pass *)gcc_data;
//   start_opt_pass(pass);
// }

void cb_pass_execution(void *gcc_data, void *user_data)
{
  auto pass = (opt_pass *)gcc_data;

  if (pass->type == opt_pass_type::GIMPLE_PASS || pass->type == opt_pass_type::RTL_PASS) {
    auto fndecl = cfun->decl;
    location_t loc = DECL_SOURCE_LOCATION(fndecl);
    unsigned int line = 0;
    unsigned int column = 0;
    if (loc != UNKNOWN_LOCATION) {
      line = LOCATION_LINE(loc);
      column = LOCATION_COLUMN(loc);
    }
    auto funcName = fndecl ?
      IDENTIFIER_POINTER(DECL_NAME(fndecl)) :
      std::string("<anonymous>:") + std::to_string(line) + ":" + std::to_string(column);
    insight::activePass = funcName;
    insight::activePass +=
      ";" + (pass->type == opt_pass_type::GIMPLE_PASS ? std::string{"GIMPLE;"} : std::string{"RTL;"});
    insight::activePass += pass->name;
  } else if (pass->type == opt_pass_type::IPA_PASS || pass->type == opt_pass_type::SIMPLE_IPA_PASS) {
    insight::activePass = "IPA;";
    insight::activePass += pass->name;
  } else {
    insight::log("unknown pass type (", pass->type, ")");
  }
}

void cb_finish_decl(void *gcc_data, void *user_data)
{
  finish_preprocessing_stage();
}

} // namespace externis

static const char *PLUGIN_NAME = "externis";

bool setup_output(int argc, plugin_argument *argv)
{
  const char *flag_name = "trace";
  const char *dir_flag_name = "trace-dir";
  // TODO: Maybe make the default filename related to the source filename.
  // TODO: Validate we only compile one TU at a time.
  FILE *trace_file = nullptr;
  if (argc == 0) {
    char file_template[] = "/tmp/trace_XXXXXX.json";
    int fd = mkstemps(file_template, 5);
    if (fd == -1) {
      perror("Externis mkstemps error: ");
      return false;
    }
    trace_file = fdopen(fd, "w");
  } else if (argc == 1 && !strcmp(argv[0].key, flag_name)) {
    trace_file = fopen(argv[0].value, "w");
    if (!trace_file) {
      fprintf(stderr, "Externis Error! Couldn't open %s for writing\n", argv[0].value);
    }
  } else if (argc == 1 && !strcmp(argv[0].key, dir_flag_name)) {
    std::string file_template{argv[0].value};
    file_template += "/trace_XXXXXX.json";
    int fd = mkstemps(file_template.data(), 5);
    if (fd == -1) {
      perror("Externis mkstemps error: ");
      return false;
    }
    trace_file = fdopen(fd, "w");
  } else {
    fprintf(stderr, "Externis Error! Arguments must be -fplugin-arg-%s-%s=FILENAME or -fplugin-arg-%s-%s=DIRECTORY\n",
            PLUGIN_NAME, flag_name, PLUGIN_NAME, dir_flag_name);
    return false;
  }
  if (trace_file) {
    externis::set_output_file(trace_file);
    return true;
  } else {
    return false;
  }
}

int plugin_init(struct plugin_name_args *plugin_info, struct plugin_gcc_version *ver)
{

  static struct plugin_info externis_info = {.version = "0.1", .help = "Generate time traces of the compilation."};
  externis::COMPILATION_START = externis::clock_t::now();

  register_callback(PLUGIN_NAME, PLUGIN_START_UNIT, &externis::cb_start_unit, nullptr);
  register_callback(PLUGIN_NAME, PLUGIN_FINISH, &insight::generic_callback<&externis::cb_plugin_finish>, nullptr);

  // if (!setup_output(plugin_info->argc, plugin_info->argv)) {
  //   return -1;
  // }

  // register_callback(PLUGIN_NAME, PLUGIN_FINISH_PARSE_FUNCTION,
  //                   &externis::cb_finish_parse_function, nullptr);

  register_callback(PLUGIN_NAME, PLUGIN_PASS_EXECUTION,
                    &insight::backend_callback<&externis::cb_pass_execution>, nullptr);

  // register_callback(PLUGIN_NAME, PLUGIN_FINISH_DECL, &externis::cb_finish_decl,
  //                   nullptr);
  register_callback(PLUGIN_NAME, PLUGIN_INFO, nullptr, &externis_info);
  return 0;
}
